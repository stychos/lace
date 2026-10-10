/*
 * laced - Lace Database Daemon
 * SSH tunnel implementation
 *
 * Creates an SSH local port forward by forking the system ssh command.
 * Password authentication is handled via the SSH_ASKPASS mechanism.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ssh_tunnel.h"
#include "log.h"
#include <util/mem.h>
#include <util/str.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Timeout waiting for tunnel readiness */
#define TUNNEL_READY_TIMEOUT_MS 30000
#define TUNNEL_POLL_INTERVAL_MS 50

/* Max time to wait for graceful shutdown */
#define TUNNEL_SHUTDOWN_RETRIES 20
#define TUNNEL_SHUTDOWN_INTERVAL_US 50000 /* 50ms */

struct SshTunnel {
  pid_t pid;         /* SSH child process PID (0 once it exited) */
  int local_port;    /* Local forwarded port (kept across restarts) */
  char *askpass_path; /* Temp askpass script (NULL if key auth) */
  char *pass_path;    /* Temp password file (NULL if key auth) */
  /* What ssh_tunnel_restart() needs to start ssh again */
  char *ssh_host;
  char *ssh_user;
  char *ssh_password; /* Secure-freed */
  int ssh_port;       /* 0: from ~/.ssh/config */
  char *remote_host;
  int remote_port;
};

/* Set when the daemon shuts down: tunnel setups stop waiting */
static _Atomic bool g_abort = false;

void ssh_tunnel_abort_pending(void) { g_abort = true; }

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/*
 * Find a free local port by binding to port 0 and reading the assignment.
 * Returns the port number, or -1 on failure.
 */
static int find_free_port(void) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(sock, (struct sockaddr *)&addr, &len) < 0) {
    close(sock);
    return -1;
  }

  int port = ntohs(addr.sin_port);
  close(sock);
  return port;
}

/*
 * Create temporary files for SSH_ASKPASS password authentication.
 *
 * Creates two files:
 * 1. A password file containing the raw password
 * 2. An askpass script that cats the password file
 *
 * Both are created with restrictive permissions (0600/0700).
 * Returns true on success.
 */
static bool create_askpass_files(const char *password,
                                 char **out_pass_path,
                                 char **out_script_path) {
  /* Create password file */
  char pass_template[] = "/tmp/lace-sshpass-XXXXXX";
  int pass_fd = mkstemp(pass_template);
  if (pass_fd < 0)
    return false;

  fchmod(pass_fd, 0600);

  size_t pass_len = strlen(password);
  ssize_t written = write(pass_fd, password, pass_len);
  if (written < 0 || (size_t)written != pass_len) {
    close(pass_fd);
    unlink(pass_template);
    return false;
  }
  /* Write trailing newline */
  write(pass_fd, "\n", 1);
  close(pass_fd);

  /* Create askpass script that cats the password file */
  char script_template[] = "/tmp/lace-askpass-XXXXXX";
  int script_fd = mkstemp(script_template);
  if (script_fd < 0) {
    unlink(pass_template);
    return false;
  }

  fchmod(script_fd, 0700);

  /* Write script content -- cat the password file */
  char script_content[512];
  int script_len = snprintf(script_content, sizeof(script_content),
                            "#!/bin/sh\ncat '%s'\n", pass_template);
  if (script_len < 0 || (size_t)script_len >= sizeof(script_content)) {
    close(script_fd);
    unlink(pass_template);
    unlink(script_template);
    return false;
  }

  written = write(script_fd, script_content, (size_t)script_len);
  close(script_fd);

  if (written < 0 || written != script_len) {
    unlink(pass_template);
    unlink(script_template);
    return false;
  }

  *out_pass_path = str_dup(pass_template);
  *out_script_path = str_dup(script_template);
  return true;
}

/*
 * Clean up temporary askpass files.
 */
static void cleanup_askpass_files(SshTunnel *tunnel) {
  if (tunnel->askpass_path) {
    unlink(tunnel->askpass_path);
    str_secure_free(tunnel->askpass_path);
    tunnel->askpass_path = NULL;
  }
  if (tunnel->pass_path) {
    unlink(tunnel->pass_path);
    str_secure_free(tunnel->pass_path);
    tunnel->pass_path = NULL;
  }
}

/*
 * Wait for the SSH tunnel to become ready by polling the local port.
 *
 * Returns true when the port is accepting connections (SSH listener is up),
 * or false on timeout / SSH process death.
 */
static bool wait_for_tunnel_ready(pid_t pid, int local_port, int timeout_ms) {
  int elapsed = 0;

  while (elapsed < timeout_ms && !g_abort) {
    /* Check if SSH process died early (auth failure, host unreachable, etc.) */
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result > 0) {
      /* SSH process exited */
      return false;
    }

    /* Try connecting to the local forwarded port */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
      return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)local_port);

    /* Set a short connect timeout */
    struct timeval tv = {.tv_sec = 0, .tv_usec = TUNNEL_POLL_INTERVAL_MS * 1000};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);

    if (ret == 0) {
      /* Port is listening -- tunnel is ready */
      return true;
    }

    usleep(TUNNEL_POLL_INTERVAL_MS * 1000);
    elapsed += TUNNEL_POLL_INTERVAL_MS;
  }

  return false;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

static void tunnel_free(SshTunnel *tunnel) {
  cleanup_askpass_files(tunnel);
  free(tunnel->ssh_host);
  free(tunnel->ssh_user);
  str_secure_free(tunnel->ssh_password);
  free(tunnel->remote_host);
  free(tunnel);
}

/* Start ssh for tunnel->local_port and wait until it forwards */
static bool tunnel_spawn(SshTunnel *tunnel, char **err) {
  int lport = tunnel->local_port;

  /* Set up password authentication if needed */
  if (tunnel->ssh_password && *tunnel->ssh_password) {
    if (!create_askpass_files(tunnel->ssh_password, &tunnel->pass_path,
                              &tunnel->askpass_path)) {
      err_set(err, "Failed to create SSH askpass helper");
      return false;
    }
  }

  /* Build ssh -L forward spec: "localport:remotehost:remoteport" */
  char forward_spec[320];
  snprintf(forward_spec, sizeof(forward_spec), "%d:%s:%d", lport,
           tunnel->remote_host, tunnel->remote_port);

  char ssh_port_str[16];
  snprintf(ssh_port_str, sizeof(ssh_port_str), "%d",
           tunnel->ssh_port > 0 ? tunnel->ssh_port : CONNSTR_PORT_SSH);

  LOG_INFO("Starting SSH tunnel: %s:%d -> %s -> %s:%d", "127.0.0.1", lport,
           tunnel->ssh_host, tunnel->remote_host, tunnel->remote_port);

  /* Environment for ssh, built before fork: the daemon is multithreaded,
   * so the child may only do async-signal-safe work (no setenv/malloc) */
  char **child_env = NULL;
  char *askpass_var = NULL;
  if (tunnel->askpass_path) {
    askpass_var = str_printf("SSH_ASKPASS=%s", tunnel->askpass_path);
    size_t n = 0;
    while (environ[n])
      n++;
    child_env = safe_calloc(n + 4, sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
      if (strncmp(environ[i], "SSH_ASKPASS=", 12) != 0 &&
          strncmp(environ[i], "SSH_ASKPASS_REQUIRE=", 20) != 0 &&
          strncmp(environ[i], "DISPLAY=", 8) != 0)
        child_env[k++] = environ[i];
    }
    if (askpass_var)
      child_env[k++] = askpass_var;
    child_env[k++] = "SSH_ASKPASS_REQUIRE=force";
    child_env[k++] = "DISPLAY=dummy:0";
    child_env[k] = NULL;
  }

  /*
   * argv, built before fork. Keepalives make ssh exit when the server stops
   * answering (about 45 s), so a dead tunnel is noticed and restarted.
   */
  const char *argv[24];
  int argc = 0;
  argv[argc++] = "ssh";
  argv[argc++] = "-N";
  argv[argc++] = "-L";
  argv[argc++] = forward_spec;
  argv[argc++] = "-o";
  argv[argc++] = "ExitOnForwardFailure=yes";
  argv[argc++] = "-o";
  argv[argc++] = "StrictHostKeyChecking=accept-new";
  argv[argc++] = "-o";
  argv[argc++] = "ServerAliveInterval=15";
  argv[argc++] = "-o";
  argv[argc++] = "ServerAliveCountMax=3";
  argv[argc++] = "-o";
  argv[argc++] = "ConnectTimeout=15";
  /* Only pass -p / -l if given: otherwise ~/.ssh/config decides */
  if (tunnel->ssh_port > 0) {
    argv[argc++] = "-p";
    argv[argc++] = ssh_port_str;
  }
  if (tunnel->ssh_user && *tunnel->ssh_user) {
    argv[argc++] = "-l";
    argv[argc++] = tunnel->ssh_user;
  }
  argv[argc++] = tunnel->ssh_host;
  argv[argc] = NULL;

  long max_fd = sysconf(_SC_OPEN_MAX); /* For the child's fd cleanup */
  if (max_fd < 0 || max_fd > 65536)
    max_fd = 65536;

  pid_t pid = fork();
  if (pid < 0) {
    err_setf(err, "fork() failed: %s", strerror(errno));
    free(child_env);
    free(askpass_var);
    cleanup_askpass_files(tunnel);
    return false;
  }

  if (pid == 0) {
    /* ---- Child process ---- */

    /* Detach from controlling terminal so SSH uses SSH_ASKPASS */
    setsid();

    /* Redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO)
        close(devnull);
    }

    /* Drop every inherited daemon fd (listening socket, client sockets,
     * database sockets, notify pipe). Otherwise ssh keeps them open: clients
     * the daemon closed never see EOF, and a crashed daemon's socket stays
     * connectable so clients never respawn it. */
    for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++)
      close(fd);

    if (child_env) {
      environ = child_env;
    }
    execvp("ssh", (char *const *)argv);
    _exit(127);
  }
  /* Parent: the child has its own copy of the environment */
  free(child_env);
  free(askpass_var);
  tunnel->pid = pid;

  LOG_DEBUG("SSH tunnel process started: pid=%d, local_port=%d", pid, lport);

  if (!wait_for_tunnel_ready(pid, lport, TUNNEL_READY_TIMEOUT_MS)) {
    int status;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w > 0 && WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if (exit_code == 127) {
        err_set(err, "SSH command not found. Is openssh-client installed?");
      } else if (exit_code == 255) {
        err_set(err, "SSH connection failed (authentication, host error, or "
                     "the local port is taken)");
      } else {
        err_setf(err, "SSH tunnel process exited with code %d", exit_code);
      }
    } else {
      err_set(err, g_abort ? "SSH tunnel setup stopped: the daemon is "
                             "shutting down"
                           : "SSH tunnel failed to become ready (timeout)");
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
    }
    tunnel->pid = 0;
    cleanup_askpass_files(tunnel);
    return false;
  }

  /* Tunnel is ready -- clean up temp files (SSH has read the password) */
  cleanup_askpass_files(tunnel);
  LOG_INFO("SSH tunnel ready: pid=%d, 127.0.0.1:%d -> %s:%d", pid, lport,
           tunnel->remote_host, tunnel->remote_port);
  return true;
}

SshTunnel *ssh_tunnel_create(const ConnString *cs, int *local_port, char **err) {
  if (!cs || !cs->ssh || !cs->ssh_host) {
    err_set(err, "Invalid SSH connection parameters");
    return NULL;
  }

  /* Determine the remote host:port to forward to */
  int remote_port = cs->port > 0 ? cs->port : connstr_get_port(cs);
  if (remote_port <= 0) {
    err_set(err, "Cannot determine remote database port for SSH tunnel");
    return NULL;
  }

  int lport = find_free_port();
  if (lport < 0) {
    err_set(err, "Failed to allocate local port for SSH tunnel");
    return NULL;
  }

  SshTunnel *tunnel = safe_calloc(1, sizeof(SshTunnel));
  tunnel->local_port = lport;
  tunnel->ssh_host = str_dup(cs->ssh_host);
  tunnel->ssh_user = cs->ssh_user ? str_dup(cs->ssh_user) : NULL;
  tunnel->ssh_password = cs->ssh_password ? str_dup(cs->ssh_password) : NULL;
  tunnel->ssh_port = cs->ssh_port;
  tunnel->remote_host = str_dup(cs->host ? cs->host : "localhost");
  tunnel->remote_port = remote_port;
  if (!tunnel->ssh_host || !tunnel->remote_host) {
    err_set(err, "Memory allocation failed");
    tunnel_free(tunnel);
    return NULL;
  }

  if (!tunnel_spawn(tunnel, err)) {
    tunnel_free(tunnel);
    return NULL;
  }
  *local_port = lport;
  return tunnel;
}

bool ssh_tunnel_restart(SshTunnel *tunnel, char **err) {
  if (!tunnel) {
    err_set(err, "No SSH tunnel");
    return false;
  }
  if (ssh_tunnel_is_alive(tunnel)) {
    return true;
  }
  LOG_WARN("SSH tunnel on port %d died; restarting", tunnel->local_port);
  return tunnel_spawn(tunnel, err);
}

void ssh_tunnel_destroy(SshTunnel *tunnel) {
  if (!tunnel)
    return;

  if (tunnel->pid > 0) {
    LOG_DEBUG("Destroying SSH tunnel: pid=%d, port=%d",
              tunnel->pid, tunnel->local_port);

    kill(tunnel->pid, SIGTERM);

    /* Wait for graceful exit */
    int status;
    bool reaped = false;
    for (int i = 0; i < TUNNEL_SHUTDOWN_RETRIES && !reaped; i++) {
      reaped = waitpid(tunnel->pid, &status, WNOHANG) != 0;
      if (!reaped)
        usleep(TUNNEL_SHUTDOWN_INTERVAL_US);
    }

    /* Force kill only if it was never reaped: once reaped, the pid may
     * already belong to another process */
    if (!reaped) {
      LOG_WARN("SSH tunnel pid=%d did not exit gracefully, sending SIGKILL",
               tunnel->pid);
      kill(tunnel->pid, SIGKILL);
      waitpid(tunnel->pid, &status, 0);
    }
  }

  tunnel_free(tunnel);
}

bool ssh_tunnel_is_alive(SshTunnel *tunnel) {
  if (!tunnel || tunnel->pid <= 0)
    return false;

  /* Reap if it exited (kill(pid, 0) succeeds on a zombie) */
  int status;
  pid_t w = waitpid(tunnel->pid, &status, WNOHANG);
  if (w == tunnel->pid || (w < 0 && errno == ECHILD)) {
    tunnel->pid = 0;
    return false;
  }
  return true;
}
