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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Timeout waiting for tunnel readiness */
#define TUNNEL_READY_TIMEOUT_MS 30000
#define TUNNEL_POLL_INTERVAL_MS 50

/* Max time to wait for graceful shutdown */
#define TUNNEL_SHUTDOWN_RETRIES 20
#define TUNNEL_SHUTDOWN_INTERVAL_US 50000 /* 50ms */

struct SshTunnel {
  pid_t pid;         /* SSH child process PID */
  int local_port;    /* Local forwarded port */
  char *askpass_path; /* Temp askpass script (NULL if key auth) */
  char *pass_path;    /* Temp password file (NULL if key auth) */
};

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

  while (elapsed < timeout_ms) {
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

SshTunnel *ssh_tunnel_create(const ConnString *cs, int *local_port, char **err) {
  if (!cs || !cs->ssh || !cs->ssh_host) {
    err_set(err, "Invalid SSH connection parameters");
    return NULL;
  }

  /* Determine the remote host:port to forward to */
  const char *remote_host = cs->host ? cs->host : "localhost";
  int remote_port = cs->port > 0 ? cs->port : connstr_get_port(cs);
  if (remote_port <= 0) {
    err_set(err, "Cannot determine remote database port for SSH tunnel");
    return NULL;
  }

  /* Find a free local port */
  int lport = find_free_port();
  if (lport < 0) {
    err_set(err, "Failed to allocate local port for SSH tunnel");
    return NULL;
  }

  SshTunnel *tunnel = safe_calloc(1, sizeof(SshTunnel));
  tunnel->local_port = lport;

  /* Set up password authentication if needed */
  if (cs->ssh_password && *cs->ssh_password) {
    if (!create_askpass_files(cs->ssh_password,
                              &tunnel->pass_path,
                              &tunnel->askpass_path)) {
      err_set(err, "Failed to create SSH askpass helper");
      free(tunnel);
      return NULL;
    }
  }

  /* Build ssh -L forward spec: "localport:remotehost:remoteport" */
  char forward_spec[256];
  snprintf(forward_spec, sizeof(forward_spec), "%d:%s:%d",
           lport, remote_host, remote_port);

  /* Build ssh port argument */
  char ssh_port_str[16];
  int ssh_port = cs->ssh_port > 0 ? cs->ssh_port : CONNSTR_PORT_SSH;
  snprintf(ssh_port_str, sizeof(ssh_port_str), "%d", ssh_port);

  LOG_INFO("Creating SSH tunnel: %s:%d -> %s -> %s:%d",
           "127.0.0.1", lport, cs->ssh_host, remote_host, remote_port);

  /* Fork the SSH process */
  pid_t pid = fork();
  if (pid < 0) {
    err_setf(err, "fork() failed: %s", strerror(errno));
    cleanup_askpass_files(tunnel);
    free(tunnel);
    return NULL;
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

    /* Set up SSH_ASKPASS for password auth */
    if (tunnel->askpass_path) {
      setenv("SSH_ASKPASS", tunnel->askpass_path, 1);
      setenv("SSH_ASKPASS_REQUIRE", "force", 1);
      setenv("DISPLAY", "dummy:0", 1);
    }

    /*
     * Build argv dynamically since -l and -p are conditional.
     * Max args: ssh -N -L spec -o ... -o ... -o ... -p port -l user host NULL
     */
    const char *argv[20];
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
    argv[argc++] = "ServerAliveInterval=60";

    /* Only pass -p if SSH port was explicitly specified */
    if (cs->ssh_port > 0) {
      argv[argc++] = "-p";
      argv[argc++] = ssh_port_str;
    }

    /* Only pass -l if SSH user was explicitly specified */
    if (cs->ssh_user && *cs->ssh_user) {
      argv[argc++] = "-l";
      argv[argc++] = cs->ssh_user;
    }

    argv[argc++] = cs->ssh_host;
    argv[argc] = NULL;

    execvp("ssh", (char *const *)argv);

    /* exec failed */
    _exit(127);
  }

  /* ---- Parent process ---- */
  tunnel->pid = pid;

  LOG_DEBUG("SSH tunnel process started: pid=%d, local_port=%d", pid, lport);

  /* Wait for the tunnel to become ready */
  if (!wait_for_tunnel_ready(pid, lport, TUNNEL_READY_TIMEOUT_MS)) {
    /* Check if SSH process exited */
    int status;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w > 0 && WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if (exit_code == 127) {
        err_set(err, "SSH command not found. Is openssh-client installed?");
      } else if (exit_code == 255) {
        err_set(err, "SSH connection failed (authentication or host error)");
      } else {
        err_setf(err, "SSH tunnel process exited with code %d", exit_code);
      }
      tunnel->pid = 0; /* Already dead */
    } else {
      err_set(err, "SSH tunnel failed to become ready (timeout)");
      /* Kill the hung process */
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      tunnel->pid = 0;
    }

    cleanup_askpass_files(tunnel);
    free(tunnel);
    return NULL;
  }

  /* Tunnel is ready -- clean up temp files (SSH has read the password) */
  cleanup_askpass_files(tunnel);

  LOG_INFO("SSH tunnel ready: pid=%d, 127.0.0.1:%d -> %s:%d",
           pid, lport, remote_host, remote_port);

  *local_port = lport;
  return tunnel;
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
    int retries = TUNNEL_SHUTDOWN_RETRIES;
    while (retries-- > 0) {
      if (waitpid(tunnel->pid, &status, WNOHANG) != 0)
        break;
      usleep(TUNNEL_SHUTDOWN_INTERVAL_US);
    }

    /* Force kill if still running */
    if (retries <= 0) {
      LOG_WARN("SSH tunnel pid=%d did not exit gracefully, sending SIGKILL",
               tunnel->pid);
      kill(tunnel->pid, SIGKILL);
      waitpid(tunnel->pid, &status, 0);
    }
  }

  cleanup_askpass_files(tunnel);
  free(tunnel);
}

bool ssh_tunnel_is_alive(const SshTunnel *tunnel) {
  if (!tunnel || tunnel->pid <= 0)
    return false;

  /* Check without reaping */
  return kill(tunnel->pid, 0) == 0;
}
