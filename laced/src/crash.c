/*
 * laced - Lace Database Daemon
 * Crash protection implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "crash.h"
#include "log.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Platform-specific backtrace support */
#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#else
#define HAVE_BACKTRACE 0
#endif

/* Maximum backtrace depth */
#define MAX_BACKTRACE_DEPTH 64

/* ==========================================================================
 * Internal State
 * ========================================================================== */

static struct {
  bool installed;
  CrashCleanupCallback cleanup;
  const char *context;

  /* Saved signal handlers for restoration */
  struct sigaction old_sigsegv;
  struct sigaction old_sigbus;
  struct sigaction old_sigfpe;
  struct sigaction old_sigabrt;
  struct sigaction old_sigill;
} g_crash = {
    .installed = false,
    .cleanup = NULL,
    .context = NULL,
};

/* Signal names for logging */
static const char *signal_name(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV (Segmentation fault)";
  case SIGBUS:
    return "SIGBUS (Bus error)";
  case SIGFPE:
    return "SIGFPE (Floating point exception)";
  case SIGABRT:
    return "SIGABRT (Abort)";
  case SIGILL:
    return "SIGILL (Illegal instruction)";
  default:
    return "Unknown signal";
  }
}

/* ==========================================================================
 * Signal-Safe Utilities
 * ========================================================================== */

/* Write a string to the log fd (signal-safe) */
static void safe_write_str(int fd, const char *str) {
  if (fd < 0 || !str) {
    return;
  }
  size_t len = 0;
  const char *p = str;
  while (*p++) len++;
  (void)write(fd, str, len);
}

/* Write a number as string (signal-safe) */
static void safe_write_num(int fd, unsigned long num) {
  if (fd < 0) {
    return;
  }

  char buf[32];
  char *p = buf + sizeof(buf) - 1;
  *p = '\0';

  if (num == 0) {
    *--p = '0';
  } else {
    while (num > 0) {
      *--p = '0' + (num % 10);
      num /= 10;
    }
  }

  safe_write_str(fd, p);
}

/* Write a pointer as hex string (signal-safe) */
static void safe_write_ptr(int fd, void *ptr) {
  if (fd < 0) {
    return;
  }

  static const char hex[] = "0123456789abcdef";
  unsigned long addr = (unsigned long)ptr;

  char buf[20];
  buf[0] = '0';
  buf[1] = 'x';

  for (int i = 0; i < 16; i++) {
    buf[17 - i] = hex[addr & 0xf];
    addr >>= 4;
  }
  buf[18] = '\0';

  safe_write_str(fd, buf);
}

/* ==========================================================================
 * Backtrace
 * ========================================================================== */

void crash_write_backtrace(void) {
  int fd = log_get_fd();
  if (fd < 0) {
    fd = STDERR_FILENO;
  }

#if HAVE_BACKTRACE
  void *frames[MAX_BACKTRACE_DEPTH];
  int frame_count = backtrace(frames, MAX_BACKTRACE_DEPTH);

  safe_write_str(fd, "\n=== Backtrace (");
  safe_write_num(fd, (unsigned long)frame_count);
  safe_write_str(fd, " frames) ===\n");

  /* backtrace_symbols_fd is async-signal-safe on most platforms */
  backtrace_symbols_fd(frames, frame_count, fd);

  safe_write_str(fd, "=== End Backtrace ===\n");
#else
  safe_write_str(fd, "\n[Backtrace not available on this platform]\n");
#endif
}

/* ==========================================================================
 * Signal Handler
 * ========================================================================== */

static void crash_signal_handler(int sig, siginfo_t *info, void *ucontext) {
  (void)ucontext;

  /* Prevent recursive crashes */
  static volatile sig_atomic_t handling_crash = 0;
  if (handling_crash) {
    _exit(128 + sig);
  }
  handling_crash = 1;

  int fd = log_get_fd();
  if (fd < 0) {
    fd = STDERR_FILENO;
  }

  /* Write crash header */
  safe_write_str(fd, "\n");
  safe_write_str(fd, "========================================\n");
  safe_write_str(fd, "FATAL: laced crashed!\n");
  safe_write_str(fd, "========================================\n");
  safe_write_str(fd, "Signal: ");
  safe_write_str(fd, signal_name(sig));
  safe_write_str(fd, "\n");

  safe_write_str(fd, "PID: ");
  safe_write_num(fd, (unsigned long)getpid());
  safe_write_str(fd, "\n");

  if (info) {
    safe_write_str(fd, "Fault address: ");
    safe_write_ptr(fd, info->si_addr);
    safe_write_str(fd, "\n");

    safe_write_str(fd, "Signal code: ");
    safe_write_num(fd, (unsigned long)info->si_code);
    safe_write_str(fd, "\n");
  }

  /* Write context if set */
  if (g_crash.context) {
    safe_write_str(fd, "Context: ");
    safe_write_str(fd, g_crash.context);
    safe_write_str(fd, "\n");
  }

  /* Write backtrace */
  crash_write_backtrace();

  /* Sync log file */
  fsync(fd);

  /* Call cleanup callback if set */
  if (g_crash.cleanup) {
    g_crash.cleanup();
  }

  /* Restore default signal handler and re-raise */
  signal(sig, SIG_DFL);
  raise(sig);
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

bool crash_handler_install(CrashCleanupCallback cleanup) {
  if (g_crash.installed) {
    return true;
  }

  g_crash.cleanup = cleanup;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_signal_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);

  /* Block other crash signals during handler */
  sigaddset(&sa.sa_mask, SIGSEGV);
  sigaddset(&sa.sa_mask, SIGBUS);
  sigaddset(&sa.sa_mask, SIGFPE);
  sigaddset(&sa.sa_mask, SIGABRT);
  sigaddset(&sa.sa_mask, SIGILL);

  /* Install handlers */
  if (sigaction(SIGSEGV, &sa, &g_crash.old_sigsegv) != 0) {
    return false;
  }
  if (sigaction(SIGBUS, &sa, &g_crash.old_sigbus) != 0) {
    sigaction(SIGSEGV, &g_crash.old_sigsegv, NULL);
    return false;
  }
  if (sigaction(SIGFPE, &sa, &g_crash.old_sigfpe) != 0) {
    sigaction(SIGSEGV, &g_crash.old_sigsegv, NULL);
    sigaction(SIGBUS, &g_crash.old_sigbus, NULL);
    return false;
  }
  if (sigaction(SIGABRT, &sa, &g_crash.old_sigabrt) != 0) {
    sigaction(SIGSEGV, &g_crash.old_sigsegv, NULL);
    sigaction(SIGBUS, &g_crash.old_sigbus, NULL);
    sigaction(SIGFPE, &g_crash.old_sigfpe, NULL);
    return false;
  }
  if (sigaction(SIGILL, &sa, &g_crash.old_sigill) != 0) {
    sigaction(SIGSEGV, &g_crash.old_sigsegv, NULL);
    sigaction(SIGBUS, &g_crash.old_sigbus, NULL);
    sigaction(SIGFPE, &g_crash.old_sigfpe, NULL);
    sigaction(SIGABRT, &g_crash.old_sigabrt, NULL);
    return false;
  }

  g_crash.installed = true;
  return true;
}

void crash_handler_uninstall(void) {
  if (!g_crash.installed) {
    return;
  }

  sigaction(SIGSEGV, &g_crash.old_sigsegv, NULL);
  sigaction(SIGBUS, &g_crash.old_sigbus, NULL);
  sigaction(SIGFPE, &g_crash.old_sigfpe, NULL);
  sigaction(SIGABRT, &g_crash.old_sigabrt, NULL);
  sigaction(SIGILL, &g_crash.old_sigill, NULL);

  g_crash.installed = false;
  g_crash.cleanup = NULL;
  g_crash.context = NULL;
}

void crash_set_context(const char *context) {
  g_crash.context = context;
}

void crash_clear_context(void) {
  g_crash.context = NULL;
}
