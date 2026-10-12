/*
 * liblace - Lace Client Library
 * Operating-system layer for Windows: TCP and stdio (pipe) links to the
 * daemon. Unix sockets and the file-permission checks have no counterpart
 * here yet: the default-socket functions report "not available", so
 * clients use LACE_CONN_TCP or a stdio daemon.
 *
 * Compiled with MinGW-w64 (winpthreads provides the pthread API the rest of
 * liblace uses); link with -lws2_32.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifdef _WIN32

#define _CRT_RAND_S
#include "os.h"
#include "../../include/util/str.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCK(h) ((SOCKET)(h))
#define HND(h) ((HANDLE)(h))

/* ---- Time ---------------------------------------------------------------- */

int64_t lace_os_now_ms(void) { return (int64_t)GetTickCount64(); }

void lace_os_sleep_ms(int ms) { Sleep(ms > 0 ? (DWORD)ms : 0); }

/* ---- Files and paths ----------------------------------------------------- */

char *lace_os_default_socket_path(void) { return NULL; }

char *lace_os_exe_dir(void) {
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
  if (n == 0 || n >= sizeof(path))
    return NULL;
  char *slash = strrchr(path, '\\');
  char *fwd = strrchr(path, '/');
  if (fwd && (!slash || fwd > slash))
    slash = fwd;
  if (!slash)
    return NULL;
  *slash = '\0';
  return str_dup(path);
}

bool lace_os_is_executable(const char *path) {
  if (!path)
    return false;
  DWORD attr = GetFileAttributesA(path);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

char *lace_os_real_path(const char *path) {
  if (!path || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    return NULL;
  char full[MAX_PATH];
  return _fullpath(full, path, sizeof(full)) ? str_dup(full) : NULL;
}

char *lace_os_current_dir(void) {
  char cwd[MAX_PATH];
  return _getcwd(cwd, sizeof(cwd)) ? str_dup(cwd) : NULL;
}

bool lace_os_path_is_absolute(const char *path) {
  if (!path || !path[0])
    return false;
  if (path[0] == '\\' || path[0] == '/')
    return true;
  return path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

const char *lace_os_daemon_name(void) { return "laced.exe"; }

char lace_os_path_list_separator(void) { return ';'; }

char *lace_os_read_private_file(const char *path, size_t max_len) {
  /* No owner/mode check: files under the user's profile are private to
   * the user by default ACLs */
  if (!path || max_len == 0)
    return NULL;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *buf = malloc(max_len + 1);
  size_t n = buf ? fread(buf, 1, max_len, f) : 0;
  fclose(f);
  if (n == 0) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  return buf;
}

bool lace_os_random(void *buf, size_t len) {
  unsigned char *p = buf;
  for (size_t i = 0; i < len; i += sizeof(unsigned int)) {
    unsigned int r;
    if (rand_s(&r) != 0)
      return false;
    size_t take = len - i < sizeof(r) ? len - i : sizeof(r);
    memcpy(p + i, &r, take);
  }
  return true;
}

uint64_t lace_os_process_id(void) { return (uint64_t)GetCurrentProcessId(); }

/* ---- Sockets ------------------------------------------------------------- */

static bool winsock_ready(void) {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  static BOOL ok = FALSE;
  BOOL pending;
  if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
    WSADATA wsa;
    ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    InitOnceComplete(&once, 0, NULL);
  }
  return ok;
}

LaceOsHandle lace_os_connect_unix(const char *path, int timeout_ms,
                                  bool verify_owner) {
  (void)path;
  (void)timeout_ms;
  (void)verify_owner;
  return LACE_OS_INVALID;
}

LaceOsHandle lace_os_connect_tcp(const char *host, int port, int timeout_ms) {
  if (!winsock_ready())
    return LACE_OS_INVALID;
  char service[16];
  snprintf(service, sizeof(service), "%d", port);
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host && host[0] ? host : "127.0.0.1", service, &hints, &res) != 0)
    return LACE_OS_INVALID;
  SOCKET s = INVALID_SOCKET;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    s = WSASocketW(ai->ai_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                   WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
      continue;
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
    bool ok = r == 0;
    if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
      fd_set w, e;
      FD_ZERO(&w);
      FD_ZERO(&e);
      FD_SET(s, &w);
      FD_SET(s, &e);
      struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
      ok = select(0, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w);
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    if (ok)
      break;
    closesocket(s);
    s = INVALID_SOCKET;
  }
  freeaddrinfo(res);
  return s == INVALID_SOCKET ? LACE_OS_INVALID : (LaceOsHandle)s;
}

bool lace_os_unix_listening(const char *path) {
  (void)path;
  return false;
}

bool lace_os_socket_owned_by_me(const char *path) {
  (void)path;
  return false;
}

bool lace_os_peer_is_me(LaceOsHandle h) {
  (void)h;
  return false;
}

/* ---- Streams ------------------------------------------------------------- */

int lace_os_wait(LaceOsHandle h, bool is_socket, bool for_write, int timeout_ms) {
  if (is_socket) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(SOCK(h), &set);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = select(0, for_write ? NULL : &set, for_write ? &set : NULL, NULL,
                   timeout_ms < 0 ? NULL : &tv);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
  }
  if (for_write)
    return 1; /* Pipe writes block until taken */
  /* Anonymous pipes cannot be waited on: poll for data (or a broken pipe,
   * which the next read reports as the end) */
  int64_t deadline = timeout_ms < 0 ? -1 : lace_os_now_ms() + timeout_ms;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(HND(h), NULL, 0, NULL, &avail, NULL) || avail > 0)
      return 1;
    if (deadline >= 0 && lace_os_now_ms() >= deadline)
      return 0;
    Sleep(5);
  }
}

int64_t lace_os_read(LaceOsHandle h, bool is_socket, void *buf, size_t len) {
  int cap = len > 0x40000000 ? 0x40000000 : (int)len;
  if (is_socket) {
    int n = recv(SOCK(h), buf, cap, 0);
    return n == SOCKET_ERROR ? (WSAGetLastError() == WSAEINTR ? -2 : -1) : n;
  }
  DWORD got = 0;
  if (!ReadFile(HND(h), buf, (DWORD)cap, &got, NULL))
    return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
  return got;
}

int64_t lace_os_write(LaceOsHandle h, bool is_socket, const void *buf, size_t len) {
  int cap = len > 0x40000000 ? 0x40000000 : (int)len;
  if (is_socket) {
    int n = send(SOCK(h), buf, cap, 0);
    return n == SOCKET_ERROR ? (WSAGetLastError() == WSAEINTR ? -2 : -1)
                             : (n == 0 ? -1 : n);
  }
  DWORD put = 0;
  if (!WriteFile(HND(h), buf, (DWORD)cap, &put, NULL) || put == 0)
    return -1;
  return put;
}

bool lace_os_hung_up(LaceOsHandle h, bool is_socket) {
  if (is_socket) {
    if (lace_os_wait(h, true, false, 0) <= 0)
      return false;
    char c;
    int n = recv(SOCK(h), &c, 1, MSG_PEEK);
    return n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK);
  }
  DWORD avail = 0;
  return !PeekNamedPipe(HND(h), NULL, 0, NULL, &avail, NULL) &&
         GetLastError() == ERROR_BROKEN_PIPE;
}

void lace_os_close(LaceOsHandle h, bool is_socket) {
  if (h == LACE_OS_INVALID)
    return;
  if (is_socket)
    closesocket(SOCK(h));
  else
    CloseHandle(HND(h));
}

/* ---- Processes ----------------------------------------------------------- */

/* One argument for a Windows command line (the C runtime's parsing rules:
 * backslashes are literal except before a quote) */
static void append_arg(StringBuilder *sb, const char *arg) {
  if (arg[0] && !strpbrk(arg, " \t\"")) {
    sb_append(sb, arg);
    return;
  }
  sb_append(sb, "\"");
  for (const char *p = arg;; p++) {
    size_t slashes = 0;
    while (*p == '\\') {
      slashes++;
      p++;
    }
    if (!*p) {
      for (size_t i = 0; i < slashes * 2; i++)
        sb_append(sb, "\\");
      break;
    }
    for (size_t i = 0; i < (*p == '"' ? slashes * 2 + 1 : slashes); i++)
      sb_append(sb, "\\");
    char c[2] = {*p, 0};
    sb_append(sb, c);
  }
  sb_append(sb, "\"");
}

static char *command_line(const char *const *argv) {
  StringBuilder *sb = sb_new(256);
  for (size_t i = 0; argv[i]; i++) {
    if (i)
      sb_append(sb, " ");
    append_arg(sb, argv[i]);
  }
  return sb_finish(sb);
}

static HANDLE null_device(void) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  return CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                     NULL);
}

static LaceOsProcess start(const char *exe, const char *const *argv,
                           HANDLE in, HANDLE out, HANDLE err, DWORD flags) {
  char *cmd = command_line(argv);
  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out;
  si.hStdError = err;
  PROCESS_INFORMATION pi;
  BOOL ok = cmd && CreateProcessA(exe, cmd, NULL, NULL, TRUE,
                                  flags | CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  free(cmd);
  if (!ok)
    return 0;
  CloseHandle(pi.hThread);
  return (LaceOsProcess)pi.hProcess;
}

LaceOsProcess lace_os_spawn_detached(const char *exe, const char *const *argv) {
  HANDLE nul = null_device();
  LaceOsProcess p = start(exe, argv, nul, nul, nul, DETACHED_PROCESS);
  if (nul != INVALID_HANDLE_VALUE)
    CloseHandle(nul);
  return p;
}

LaceOsProcess lace_os_spawn_piped(const char *exe, const char *const *argv,
                                  LaceOsHandle *to_child,
                                  LaceOsHandle *from_child) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE in_r, in_w, out_r, out_w;
  if (!CreatePipe(&in_r, &in_w, &sa, 0))
    return 0;
  if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
    CloseHandle(in_r);
    CloseHandle(in_w);
    return 0;
  }
  /* Only the child's ends are inherited */
  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = null_device();
  LaceOsProcess p = start(exe, argv, in_r, out_w, nul, 0);
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (nul != INVALID_HANDLE_VALUE)
    CloseHandle(nul);
  if (!p) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    return 0;
  }
  *to_child = (LaceOsHandle)in_w;
  *from_child = (LaceOsHandle)out_r;
  return p;
}

bool lace_os_process_exited(LaceOsProcess p) {
  return !p || WaitForSingleObject(HND(p), 0) == WAIT_OBJECT_0;
}

void lace_os_process_stop(LaceOsProcess p, bool force) {
  (void)force; /* No polite stop for a console-less process: terminate */
  if (!p)
    return;
  TerminateProcess(HND(p), 1);
  WaitForSingleObject(HND(p), 5000);
  CloseHandle(HND(p));
}

#endif /* _WIN32 */
