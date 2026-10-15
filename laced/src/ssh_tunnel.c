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
#include "constants.h"
#include "log.h"
#include "server.h"
#include <util/mem.h>
#include <util/str.h>

#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TUNNEL_POLL_INTERVAL_MS 50

/* Max time to wait for graceful shutdown */
#define TUNNEL_SHUTDOWN_RETRIES 20
#define TUNNEL_SHUTDOWN_INTERVAL_US 50000 /* 50ms */

struct SshTunnel {
  pid_t pid;         /* SSH child process PID (0 once it exited) */
  /* ssh forwards to a Unix socket in a private (0700) directory, never a
   * TCP port: another local user could take a port (in the gap before ssh
   * binds it, or after ssh died) and receive the database login */
  char *socket_dir;  /* Created for this tunnel, removed with it */
  char *socket_path; /* Kept across restarts */
  char *askpass_path; /* Temp askpass script (NULL if key auth) */
  char *pass_path;    /* Temp password file (NULL if key auth) */
  /* What ssh_tunnel_restart() needs to start ssh again */
  char *ssh_host;
  char *ssh_user;
  char *ssh_key;      /* Identity file for -i (NULL: ssh's own choice) */
  char *ssh_password; /* Secure-freed */
  int ssh_port;       /* 0: from ~/.ssh/config */
  char *remote_host;
  int remote_port;
  int err_fd; /* Read end of ssh's stderr (-1: none): why it failed */
};

static void close_ssh_stderr(SshTunnel *tunnel);

/* Set when the daemon shuts down: tunnel setups stop waiting */
static _Atomic bool g_abort = false;

void ssh_tunnel_abort_pending(void) { g_abort = true; }

const char *ssh_tunnel_host(const SshTunnel *tunnel) {
  return tunnel ? tunnel->ssh_host : NULL;
}

const char *ssh_tunnel_socket_path(const SshTunnel *tunnel) {
  return tunnel ? tunnel->socket_path : NULL;
}

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

#define TUNNEL_OWNER_FILE "owner"
#define TUNNEL_PASS_FILE "pass"
#define TUNNEL_ASKPASS_FILE "askpass"

/* Remove a tunnel directory and what it holds (its own files and socket;
 * it has no subdirectories) */
static void remove_tunnel_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
        unlinkat(dirfd(d), e->d_name, 0);
    }
    closedir(d);
  }
  rmdir(dir);
}

/* A new private (0700) directory for the tunnel's socket, next to the
 * daemon's own socket; NULL on failure */
static char *make_socket_dir(void) {
  char *sock = laced_get_default_socket_path(); /* Creates its directory */
  char *slash = sock ? strrchr(sock, '/') : NULL;
  if (!slash) {
    free(sock);
    return NULL;
  }
  *slash = '\0';
  char *dir = str_printf("%s/lace-tun-XXXXXX", sock);
  free(sock);
  if (dir && !mkdtemp(dir)) {
    free(dir);
    return NULL;
  }
  /* Who made it: a daemon that crashed leaves the directory behind, and
   * the next daemon removes it only once this pid is gone */
  char *owner = str_printf("%s/" TUNNEL_OWNER_FILE, dir);
  int fd = open(owner, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  free(owner);
  if (fd < 0) {
    rmdir(dir);
    free(dir);
    return NULL;
  }
  char pid[24];
  int n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
  bool ok = write(fd, pid, (size_t)n) == n;
  close(fd);
  if (!ok) {
    remove_tunnel_dir(dir);
    free(dir);
    return NULL;
  }
  return dir;
}

/* Write content to dir/name, a new file with the given mode; the path, or
 * NULL on failure. dir is the tunnel's private directory. */
static char *write_private_file(const char *dir, const char *name,
                                const char *content, mode_t mode) {
  char *path = str_printf("%s/%s", dir, name);
  unlink(path); /* Left by a failed earlier start of this tunnel */
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd < 0) {
    str_secure_free(path);
    return NULL;
  }
  size_t len = strlen(content);
  bool ok = write(fd, content, len) == (ssize_t)len;
  close(fd);
  if (!ok) {
    unlink(path);
    str_secure_free(path);
    return NULL;
  }
  return path;
}

/*
 * SSH_ASKPASS password authentication: a password file and a script that
 * prints it, both in the tunnel's private (0700) directory, so a daemon
 * that dies during setup leaves them only where the next daemon's sweep
 * removes them. They live until ssh has read the password.
 */
static bool create_askpass_files(const char *dir, const char *password,
                                 char **out_pass_path, char **out_script_path) {
  char *line = str_printf("%s\n", password);
  char *pass = write_private_file(dir, TUNNEL_PASS_FILE, line, 0600);
  str_secure_free(line);
  if (!pass) {
    return false;
  }
  char *script_content = str_printf("#!/bin/sh\ncat '%s'\n", pass);
  char *script = write_private_file(dir, TUNNEL_ASKPASS_FILE, script_content, 0700);
  free(script_content);
  if (!script) {
    unlink(pass);
    str_secure_free(pass);
    return false;
  }
  *out_pass_path = pass;
  *out_script_path = script;
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
 * Wait until ssh listens on the tunnel's socket. Only this user can create
 * it (private directory), so whatever accepts there is our ssh. Returns 1
 * when ready, 0 on timeout or abort, -1 if ssh exited (*status: its wait
 * status; it is reaped, so its pid must not be used again).
 */
static int wait_for_tunnel_ready(pid_t pid, const char *path, int timeout_ms,
                                 int *status) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
  for (int elapsed = 0; elapsed < timeout_ms && !g_abort;
       elapsed += TUNNEL_POLL_INTERVAL_MS) {
    if (waitpid(pid, status, WNOHANG) == pid)
      return -1;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
      return 0;
    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    if (ret == 0)
      return 1;
    usleep(TUNNEL_POLL_INTERVAL_MS * 1000);
  }
  return 0;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

static void tunnel_free(SshTunnel *tunnel) {
  cleanup_askpass_files(tunnel);
  close_ssh_stderr(tunnel);
  if (tunnel->socket_dir)
    remove_tunnel_dir(tunnel->socket_dir); /* Socket, owner file */
  free(tunnel->socket_path);
  free(tunnel->socket_dir);
  free(tunnel->ssh_host);
  free(tunnel->ssh_user);
  free(tunnel->ssh_key);
  str_secure_free(tunnel->ssh_password);
  free(tunnel->remote_host);
  free(tunnel);
}

/* Read what ssh wrote to stderr so far (non-blocking); keeps the last
 * complete line in last (NUL-terminated, control characters dropped) */
static void drain_ssh_stderr(SshTunnel *tunnel, char *last, size_t last_size) {
  if (!tunnel || tunnel->err_fd < 0)
    return;
  char buf[1024];
  char line[512];
  size_t len = 0;
  if (last && last_size)
    last[0] = '\0';
  for (;;) {
    ssize_t n = read(tunnel->err_fd, buf, sizeof(buf));
    if (n <= 0)
      break; /* EOF, EAGAIN or error */
    for (ssize_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || c == '\r') {
        if (len > 0 && last && last_size) {
          line[len] = '\0';
          snprintf(last, last_size, "%s", line);
        }
        len = 0;
      } else if ((unsigned char)c >= ' ' && len + 1 < sizeof(line)) {
        line[len++] = c;
      }
    }
  }
  if (len > 0 && last && last_size) {
    line[len] = '\0';
    snprintf(last, last_size, "%s", line);
  }
}

static void close_ssh_stderr(SshTunnel *tunnel) {
  if (tunnel && tunnel->err_fd >= 0) {
    close(tunnel->err_fd);
    tunnel->err_fd = -1;
  }
}

/* Start ssh for tunnel->socket_path and wait until it forwards */
static bool tunnel_spawn(SshTunnel *tunnel, char **err) {

  /* Set up password authentication if needed */
  if (tunnel->ssh_password && *tunnel->ssh_password) {
    if (!create_askpass_files(tunnel->socket_dir, tunnel->ssh_password,
                              &tunnel->pass_path, &tunnel->askpass_path)) {
      err_set(err, "Failed to create SSH askpass helper");
      return false;
    }
  }

  /* ssh -L forward spec: "socketpath:remotehost:remoteport" */
  /* An IPv6 database host goes in brackets (ssh splits the spec at ':');
   * the whole spec is built before fork, never cut short */
  bool v6 = strchr(tunnel->remote_host, ':') != NULL;
  char *forward_spec = str_printf(v6 ? "%s:[%s]:%d" : "%s:%s:%d", tunnel->socket_path,
                                  tunnel->remote_host, tunnel->remote_port);

  char ssh_port_str[16];
  snprintf(ssh_port_str, sizeof(ssh_port_str), "%d",
           tunnel->ssh_port > 0 ? tunnel->ssh_port : CONNSTR_PORT_SSH);

  LOG_INFO("Starting SSH tunnel: %s -> %s -> %s:%d", tunnel->socket_path,
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
  const char *ssh_config = getenv("LACED_SSH_CONFIG");
  const char *argv[40]; /* At most 37 used: keep room when adding options */
  int argc = 0;
  argv[argc++] = "ssh";
  argv[argc++] = "-N";
  argv[argc++] = "-L";
  argv[argc++] = forward_spec;
  argv[argc++] = "-o";
  argv[argc++] = "ExitOnForwardFailure=yes";
  /* A socket left by an ssh that died is replaced on restart; only this
   * user may connect to the new one */
  argv[argc++] = "-o";
  argv[argc++] = "StreamLocalBindUnlink=yes";
  argv[argc++] = "-o";
  argv[argc++] = "StreamLocalBindMask=0177";
  /* This ssh must own the forward: a ControlMaster from the user's
   * ssh_config would hand it to another process (liveness checks, restart
   * and cleanup all go by this pid) */
  argv[argc++] = "-o";
  argv[argc++] = "ControlMaster=no";
  argv[argc++] = "-o";
  argv[argc++] = "ControlPath=none";
  argv[argc++] = "-o";
  argv[argc++] = "StrictHostKeyChecking=accept-new";
  argv[argc++] = "-o";
  argv[argc++] = "ServerAliveInterval=15";
  argv[argc++] = "-o";
  argv[argc++] = "ServerAliveCountMax=3";
  argv[argc++] = "-o";
  argv[argc++] = "ConnectTimeout=15";
  /* $LACED_SSH_CONFIG: an ssh_config file used instead of ~/.ssh/config
   * (ssh -F), e.g. for its own known_hosts; read before fork */
  if (ssh_config && *ssh_config) {
    argv[argc++] = "-F";
    argv[argc++] = ssh_config;
  }
  /* Only pass -p / -l if given: otherwise ~/.ssh/config decides */
  if (tunnel->ssh_port > 0) {
    argv[argc++] = "-p";
    argv[argc++] = ssh_port_str;
  }
  if (tunnel->ssh_key && *tunnel->ssh_key) {
    /* That key, not every key the agent holds first (servers stop after a
     * few attempts) */
    argv[argc++] = "-i";
    argv[argc++] = tunnel->ssh_key;
    argv[argc++] = "-o";
    argv[argc++] = "IdentitiesOnly=yes";
  }
  if (tunnel->ssh_user && *tunnel->ssh_user) {
    argv[argc++] = "-l";
    argv[argc++] = tunnel->ssh_user;
  }
  argv[argc++] = "--"; /* The host is never an option, whatever it says */
  argv[argc++] = tunnel->ssh_host;
  argv[argc] = NULL;
  _Static_assert(sizeof(argv) / sizeof(argv[0]) >= 37, "ssh argv too small");

  long max_fd = sysconf(_SC_OPEN_MAX); /* For the child's fd cleanup */
  if (max_fd < 0 || max_fd > 65536)
    max_fd = 65536;

  /* ssh's stderr into a pipe: "Permission denied", "Could not resolve
   * hostname" say more than its exit code. Read end non-blocking, and
   * drained while ssh runs so it never blocks on a full pipe. */
  close_ssh_stderr(tunnel);
  int errp[2] = {-1, -1};
  if (pipe(errp) == 0) {
    fcntl(errp[0], F_SETFD, FD_CLOEXEC);
    fcntl(errp[0], F_SETFL, fcntl(errp[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(errp[1], F_SETFD, FD_CLOEXEC);
  }

  pid_t pid = fork();
  if (pid != 0) {
    free(forward_spec); /* The child has its own copy until exec */
    if (errp[1] >= 0)
      close(errp[1]);
    tunnel->err_fd = pid > 0 ? errp[0] : -1;
    if (pid < 0 && errp[0] >= 0)
      close(errp[0]);
  }
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
      dup2(errp[1] >= 0 ? errp[1] : devnull, STDERR_FILENO);
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

  LOG_DEBUG("SSH tunnel process started: pid=%d, socket=%s", pid,
            tunnel->socket_path);

  int status = 0;
  int ready = wait_for_tunnel_ready(pid, tunnel->socket_path,
                                    LACED_TUNNEL_READY_TIMEOUT_MS, &status);
  if (ready != 1) {
    if (ready < 0 && WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if (exit_code == 127) {
        err_set(err, "SSH command not found. Is openssh-client installed?");
      } else if (exit_code == 255) {
        char why[256];
        drain_ssh_stderr(tunnel, why, sizeof(why));
        if (why[0])
          err_setf(err, "SSH connection failed: %s", why);
        else
          err_set(err, "SSH connection failed (authentication or host error)");
      } else {
        err_setf(err, "SSH tunnel process exited with code %d", exit_code);
      }
    } else if (ready < 0) {
      err_set(err, "SSH tunnel process was killed");
    } else {
      /* Still running: stop it (not reaped yet, so the pid is still ours) */
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
  LOG_INFO("SSH tunnel ready: pid=%d, %s -> %s:%d", pid, tunnel->socket_path,
           tunnel->remote_host, tunnel->remote_port);
  return true;
}

SshTunnel *ssh_tunnel_create(const ConnString *cs, char **err) {
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

  SshTunnel *tunnel = safe_calloc(1, sizeof(SshTunnel));
  tunnel->err_fd = -1;
  tunnel->socket_dir = make_socket_dir();
  if (!tunnel->socket_dir) {
    err_set(err, "Cannot create a private directory for the SSH tunnel");
    tunnel_free(tunnel);
    return NULL;
  }
  /* PostgreSQL clients look for <dir>/.s.PGSQL.<port>; MySQL takes a path */
  bool pg = cs->driver && (str_eq(cs->driver, "postgres") ||
                           str_eq(cs->driver, "postgresql") ||
                           str_eq(cs->driver, "pg"));
  tunnel->socket_path =
      pg ? str_printf("%s/.s.PGSQL.%d", tunnel->socket_dir, remote_port)
         : str_printf("%s/db.sock", tunnel->socket_dir);
  /* ssh and the drivers cannot use a longer path (truncating it would
   * name another file); say so instead of a misleading ssh failure */
  struct sockaddr_un probe;
  if (strlen(tunnel->socket_path) >= sizeof(probe.sun_path)) {
    err_setf(err, "The SSH tunnel's socket path is too long (%zu bytes; the "
                  "limit is %zu): %s. Start laced with a shorter socket "
                  "directory (e.g. XDG_RUNTIME_DIR)",
             strlen(tunnel->socket_path), sizeof(probe.sun_path) - 1,
             tunnel->socket_path);
    tunnel_free(tunnel);
    return NULL;
  }
  tunnel->ssh_host = str_dup(cs->ssh_host);
  tunnel->ssh_user = cs->ssh_user ? str_dup(cs->ssh_user) : NULL;
  /* ~/ means the daemon user's home, as in ssh_config */
  const char *key = connstr_get_option(cs, "ssh_key");
  if (key && key[0]) {
    const char *home = getenv("HOME");
    tunnel->ssh_key = key[0] == '~' && key[1] == '/' && home
                          ? str_printf("%s%s", home, key + 1)
                          : str_dup(key);
    if (access(tunnel->ssh_key, R_OK) != 0) {
      err_setf(err, "SSH key %s: %s", tunnel->ssh_key, strerror(errno));
      tunnel_free(tunnel);
      return NULL;
    }
  }
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
  LOG_WARN("SSH tunnel at %s died; restarting", tunnel->socket_path);
  return tunnel_spawn(tunnel, err);
}

void ssh_tunnel_destroy(SshTunnel *tunnel) {
  if (!tunnel)
    return;

  if (tunnel->pid > 0) {
    LOG_DEBUG("Destroying SSH tunnel: pid=%d, socket=%s", tunnel->pid,
              tunnel->socket_path);

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
  drain_ssh_stderr(tunnel, NULL, 0); /* Keep ssh from blocking on it */

  /* Reap if it exited (kill(pid, 0) succeeds on a zombie) */
  int status;
  pid_t w = waitpid(tunnel->pid, &status, WNOHANG);
  if (w == tunnel->pid || (w < 0 && errno == ECHILD)) {
    tunnel->pid = 0;
    return false;
  }
  return true;
}

void ssh_tunnel_signal_exit(SshTunnel *tunnel) {
  if (tunnel && tunnel->pid > 0)
    kill(tunnel->pid, SIGTERM);
}

void ssh_tunnel_sweep_stale(void) {
  char *sock = laced_get_default_socket_path();
  char *slash = sock ? strrchr(sock, '/') : NULL;
  if (!slash) {
    free(sock);
    return;
  }
  *slash = '\0';
  DIR *d = opendir(sock);
  if (!d) {
    free(sock);
    return;
  }
  time_t now = time(NULL);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    char *path = str_printf("%s/%s", sock, e->d_name);
    struct stat st;
    if (strncmp(e->d_name, "lace-tun-", 9) == 0 && lstat(path, &st) == 0 &&
        S_ISDIR(st.st_mode) && st.st_uid == geteuid()) {
      /* Its daemon's pid: remove the directory once that process is gone
       * (no owner file: not ours to judge; a reused pid: kept, safely) */
      char *owner = str_printf("%s/" TUNNEL_OWNER_FILE, path);
      FILE *f = fopen(owner, "r");
      long pid = 0;
      if (f) {
        if (fscanf(f, "%ld", &pid) != 1)
          pid = 0;
        fclose(f);
      }
      free(owner);
      if (pid > 0 && kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
        LOG_INFO("Removing %s: its daemon (pid %ld) is gone", path, pid);
        remove_tunnel_dir(path);
      }
    } else if ((strncmp(e->d_name, "lace-sshpass-", 13) == 0 ||
                strncmp(e->d_name, "lace-askpass-", 13) == 0) &&
               lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
               st.st_uid == geteuid() && now - st.st_mtime > 600) {
      /* Older daemons kept these beside the tunnel directories, for the
       * at most 30 s of a tunnel's setup: ten minutes old means stale */
      LOG_INFO("Removing stale %s", path);
      unlink(path);
    }
    free(path);
  }
  closedir(d);
  free(sock);
}
