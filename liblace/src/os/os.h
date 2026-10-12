/*
 * liblace - Lace Client Library
 * Operating-system layer: everything liblace needs from the OS, so the rest
 * of the library is plain C. os_posix.c implements it for macOS, Linux and
 * the BSDs; os_win32.c for Windows (TCP and stdio links; no Unix sockets
 * or file-permission checks there).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LIBLACE_OS_H
#define LIBLACE_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A socket or pipe end: an fd on POSIX, a SOCKET or HANDLE on Windows */
typedef intptr_t LaceOsHandle;
#define LACE_OS_INVALID ((LaceOsHandle)-1)

/* A child process: a pid on POSIX, a process HANDLE on Windows; 0 = none */
typedef intptr_t LaceOsProcess;

/* ---- Time ---------------------------------------------------------------- */

int64_t lace_os_now_ms(void); /* Monotonic */
void lace_os_sleep_ms(int ms);

/* ---- Files and paths ----------------------------------------------------- */

/* Where a per-user daemon listens by default (NULL if there is no default,
 * e.g. on Windows, which has no Unix sockets here) */
char *lace_os_default_socket_path(void);

/* Directory of the running executable, or NULL */
char *lace_os_exe_dir(void);

/* Is path an executable file? */
bool lace_os_is_executable(const char *path);

/* Absolute, resolved form of an existing path (NULL if it does not exist) */
char *lace_os_real_path(const char *path);

/* The working directory (allocated), or NULL */
char *lace_os_current_dir(void);

/* True if path is absolute */
bool lace_os_path_is_absolute(const char *path);

/* The daemon's file name ("laced", "laced.exe") and the PATH separator */
const char *lace_os_daemon_name(void);
char lace_os_path_list_separator(void);

/* Contents of a small file only this user can read (a token file);
 * NULL if missing, not ours, readable by others, or empty */
char *lace_os_read_private_file(const char *path, size_t max_len);

/* Fill buf with random bytes; false if the OS has no source */
bool lace_os_random(void *buf, size_t len);

/* A number identifying this process (seeding, never security) */
uint64_t lace_os_process_id(void);

/* ---- Sockets ------------------------------------------------------------- */

/* Connect, waiting at most timeout_ms. verify_owner: the socket file and the
 * process behind it must belong to this user (the default path).
 * LACE_OS_INVALID on failure (unsupported on Windows). */
LaceOsHandle lace_os_connect_unix(const char *path, int timeout_ms,
                                  bool verify_owner);
LaceOsHandle lace_os_connect_tcp(const char *host, int port, int timeout_ms);

/* Is there a socket file at path that a quick connect reaches? */
bool lace_os_unix_listening(const char *path);

bool lace_os_socket_owned_by_me(const char *path);
bool lace_os_peer_is_me(LaceOsHandle h);

/* ---- Streams (a link's socket or pipe ends) ------------------------------ */

/* Wait until h is readable (or writable): 1 ready, 0 timeout, -1 error or
 * hang-up (for writing). timeout_ms < 0 waits without limit. */
int lace_os_wait(LaceOsHandle h, bool is_socket, bool for_write, int timeout_ms);

/* > 0 bytes moved, 0 end of stream (read), -1 error, -2 try again */
int64_t lace_os_read(LaceOsHandle h, bool is_socket, void *buf, size_t len);
int64_t lace_os_write(LaceOsHandle h, bool is_socket, const void *buf, size_t len);

/* Never blocks: true if the other end of an idle stream has closed (data
 * waiting to be read is not a hang-up) */
bool lace_os_hung_up(LaceOsHandle h, bool is_socket);

void lace_os_close(LaceOsHandle h, bool is_socket);

/* ---- Processes ----------------------------------------------------------- */

/* Start exe with argv (argv[0] is exe; NULL-terminated), detached from the
 * terminal, standard streams on the null device, inheriting no other
 * handles, and not our child (POSIX: adopted by init, so it never lingers
 * as a zombie of ours). Its pid, or 0 on failure. */
LaceOsProcess lace_os_spawn_detached(const char *exe, const char *const *argv);

/* Start exe with its stdin and stdout connected to pipes (stderr on the
 * null device): *to_child writes its stdin, *from_child reads its stdout.
 * 0 on failure. */
LaceOsProcess lace_os_spawn_piped(const char *exe, const char *const *argv,
                                  LaceOsHandle *to_child,
                                  LaceOsHandle *from_child);

/* Non-blocking: true if the process has exited (a child of ours is reaped;
 * a detached one is checked for existence) */
bool lace_os_process_exited(LaceOsProcess p);

/* Ask it to stop (force: kill outright), then wait until it has (a
 * detached process is only signalled: it is not ours to wait for) */
void lace_os_process_stop(LaceOsProcess p, bool force);

#endif /* LIBLACE_OS_H */
