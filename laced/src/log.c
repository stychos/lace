/*
 * laced - Lace Database Daemon
 * Logging system implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ==========================================================================
 * Internal State
 * ========================================================================== */

static struct {
  bool initialized;
  LogLevel min_level;
  size_t max_file_size;
  int max_rotated_files;
  bool use_stderr;
  char *log_path;
  FILE *log_file;
  int log_fd;
  pthread_mutex_t mutex;
  size_t current_size;
} g_log = {
    .initialized = false,
    .min_level = LOG_LEVEL_INFO,
    .max_file_size = LOG_DEFAULT_MAX_SIZE,
    .max_rotated_files = LOG_DEFAULT_ROTATE_COUNT,
    .use_stderr = false,
    .log_path = NULL,
    .log_file = NULL,
    .log_fd = -1,
    .current_size = 0,
};

/* Level names for log output */
static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

/* ==========================================================================
 * Internal Helpers
 * ========================================================================== */

/* Get base filename from path */
static const char *basename_safe(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* Create directory path recursively */
static bool ensure_directory(const char *path) {
  char *path_copy = strdup(path);
  if (!path_copy) {
    return false;
  }

  /* Find last slash to get directory */
  char *last_slash = strrchr(path_copy, '/');
  if (!last_slash) {
    free(path_copy);
    return true; /* No directory component */
  }
  *last_slash = '\0';

  /* Check if directory exists */
  struct stat st;
  if (stat(path_copy, &st) == 0) {
    free(path_copy);
    return S_ISDIR(st.st_mode);
  }

  /* Create parent directories */
  for (char *p = path_copy + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
        free(path_copy);
        return false;
      }
      *p = '/';
    }
  }

  /* Create final directory */
  if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
    free(path_copy);
    return false;
  }

  free(path_copy);
  return true;
}

/* Get current file size */
static size_t get_file_size(FILE *f) {
  if (!f) {
    return 0;
  }
  long pos = ftell(f);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, pos, SEEK_SET);
  return size > 0 ? (size_t)size : 0;
}

/* Rotate log files */
static void rotate_logs(void) {
  if (!g_log.log_path || g_log.max_rotated_files <= 0) {
    return;
  }

  /* Close current file */
  if (g_log.log_file) {
    fclose(g_log.log_file);
    g_log.log_file = NULL;
    g_log.log_fd = -1;
  }

  /* Remove oldest rotated file */
  char old_path[1024];
  snprintf(old_path, sizeof(old_path), "%s.%d", g_log.log_path,
           g_log.max_rotated_files);
  unlink(old_path);

  /* Rotate existing files */
  for (int i = g_log.max_rotated_files - 1; i >= 1; i--) {
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s.%d", g_log.log_path, i);
    snprintf(dst, sizeof(dst), "%s.%d", g_log.log_path, i + 1);
    rename(src, dst);
  }

  /* Rotate current log */
  char rotated_path[1024];
  snprintf(rotated_path, sizeof(rotated_path), "%s.1", g_log.log_path);
  rename(g_log.log_path, rotated_path);

  /* Reopen log file */
  g_log.log_file = fopen(g_log.log_path, "a");
  if (g_log.log_file) {
    g_log.log_fd = fileno(g_log.log_file);
    g_log.current_size = 0;
    setvbuf(g_log.log_file, NULL, _IOLBF, 0);
  }
}

/* Check if rotation is needed */
static void check_rotation(size_t bytes_written) {
  if (g_log.max_file_size == 0 || g_log.use_stderr) {
    return;
  }

  g_log.current_size += bytes_written;
  if (g_log.current_size >= g_log.max_file_size) {
    rotate_logs();
  }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

char *log_get_default_path(void) {
  char *path = NULL;

#ifdef __APPLE__
  /* macOS: ~/Library/Logs/laced.log */
  const char *home = getenv("HOME");
  if (home) {
    size_t len = strlen(home) + 32;
    path = malloc(len);
    if (path) {
      snprintf(path, len, "%s/Library/Logs/laced.log", home);
    }
  }
#else
  /* Linux: ~/.local/share/lace/laced.log */
  const char *xdg_data = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");

  if (xdg_data) {
    size_t len = strlen(xdg_data) + 32;
    path = malloc(len);
    if (path) {
      snprintf(path, len, "%s/lace/laced.log", xdg_data);
    }
  } else if (home) {
    size_t len = strlen(home) + 48;
    path = malloc(len);
    if (path) {
      snprintf(path, len, "%s/.local/share/lace/laced.log", home);
    }
  }
#endif

  return path;
}

bool log_init(const LogConfig *config) {
  if (g_log.initialized) {
    return true; /* Already initialized */
  }

  /* Initialize mutex */
  if (pthread_mutex_init(&g_log.mutex, NULL) != 0) {
    return false;
  }

  /* Apply configuration */
  if (config) {
    g_log.min_level = config->min_level;
    g_log.max_file_size = config->max_file_size;
    g_log.max_rotated_files = config->max_rotated_files;
    g_log.use_stderr = config->use_stderr;
    if (config->log_path) {
      g_log.log_path = strdup(config->log_path);
    }
  } else {
    /* Defaults */
#ifdef DEBUG
    g_log.min_level = LOG_LEVEL_DEBUG;
#else
    g_log.min_level = LOG_LEVEL_INFO;
#endif
    g_log.max_file_size = LOG_DEFAULT_MAX_SIZE;
    g_log.max_rotated_files = LOG_DEFAULT_ROTATE_COUNT;
    g_log.use_stderr = false;
  }

  /* Open log file */
  if (g_log.use_stderr) {
    g_log.log_file = stderr;
    g_log.log_fd = STDERR_FILENO;
  } else {
    if (!g_log.log_path) {
      g_log.log_path = log_get_default_path();
    }

    if (g_log.log_path) {
      if (!ensure_directory(g_log.log_path)) {
        /* Fall back to stderr */
        g_log.use_stderr = true;
        g_log.log_file = stderr;
        g_log.log_fd = STDERR_FILENO;
      } else {
        g_log.log_file = fopen(g_log.log_path, "a");
        if (g_log.log_file) {
          g_log.log_fd = fileno(g_log.log_file);
          g_log.current_size = get_file_size(g_log.log_file);
          setvbuf(g_log.log_file, NULL, _IOLBF, 0);
        } else {
          /* Fall back to stderr */
          g_log.use_stderr = true;
          g_log.log_file = stderr;
          g_log.log_fd = STDERR_FILENO;
        }
      }
    } else {
      /* No path available, use stderr */
      g_log.use_stderr = true;
      g_log.log_file = stderr;
      g_log.log_fd = STDERR_FILENO;
    }
  }

  g_log.initialized = true;
  return true;
}

void log_shutdown(void) {
  if (!g_log.initialized) {
    return;
  }

  pthread_mutex_lock(&g_log.mutex);

  if (g_log.log_file && g_log.log_file != stderr) {
    fflush(g_log.log_file);
    fclose(g_log.log_file);
  }

  free(g_log.log_path);

  g_log.log_file = NULL;
  g_log.log_fd = -1;
  g_log.log_path = NULL;
  g_log.initialized = false;

  pthread_mutex_unlock(&g_log.mutex);
  pthread_mutex_destroy(&g_log.mutex);
}

void log_write(LogLevel level, const char *file, int line, const char *fmt,
               ...) {
  va_list args;
  va_start(args, fmt);
  log_writev(level, file, line, fmt, args);
  va_end(args);
}

void log_writev(LogLevel level, const char *file, int line, const char *fmt,
                va_list args) {
  if (!g_log.initialized || level < g_log.min_level) {
    return;
  }

  pthread_mutex_lock(&g_log.mutex);

  if (!g_log.log_file) {
    pthread_mutex_unlock(&g_log.mutex);
    return;
  }

  /* Get timestamp */
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm tm;
  localtime_r(&tv.tv_sec, &tm);

  /* Get thread ID */
  unsigned long tid_num;
#ifdef __APPLE__
  uint64_t tid64;
  pthread_threadid_np(NULL, &tid64);
  tid_num = (unsigned long)tid64;
#else
  tid_num = (unsigned long)pthread_self();
#endif

  /* Format log line */
  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
           tm.tm_sec, (int)(tv.tv_usec / 1000));

  /* Write header */
  int header_len =
      fprintf(g_log.log_file, "[%s] [%-5s] [%d:%lu] [%s:%d] ", timestamp,
              level_names[level], (int)getpid(), tid_num, basename_safe(file),
              line);

  /* Write message */
  int msg_len = vfprintf(g_log.log_file, fmt, args);

  /* Write newline */
  fprintf(g_log.log_file, "\n");

  /* Track size for rotation */
  if (header_len > 0 && msg_len > 0) {
    check_rotation((size_t)header_len + (size_t)msg_len + 1);
  }

  pthread_mutex_unlock(&g_log.mutex);
}

void log_emergency(const char *message) {
  if (g_log.log_fd < 0) {
    return;
  }

  /* Signal-safe write - no malloc, no mutex, no complex operations */
  /* Write timestamp approximation */
  const char prefix[] = "[EMERGENCY] ";
  (void)write(g_log.log_fd, prefix, sizeof(prefix) - 1);

  /* Write message */
  size_t len = 0;
  const char *p = message;
  while (*p++) len++;
  (void)write(g_log.log_fd, message, len);

  /* Write newline */
  (void)write(g_log.log_fd, "\n", 1);

  /* Try to sync */
  fsync(g_log.log_fd);
}

int log_get_fd(void) {
  return g_log.log_fd;
}

void log_flush(void) {
  if (!g_log.initialized) {
    return;
  }

  pthread_mutex_lock(&g_log.mutex);
  if (g_log.log_file) {
    fflush(g_log.log_file);
  }
  pthread_mutex_unlock(&g_log.mutex);
}

const char *log_get_path(void) {
  return g_log.use_stderr ? NULL : g_log.log_path;
}
