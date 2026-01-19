/*
 * laced - Lace Database Daemon
 * Logging system
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_LOG_H
#define LACED_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* ==========================================================================
 * Log Levels
 * ========================================================================== */

typedef enum {
  LOG_LEVEL_DEBUG = 0,
  LOG_LEVEL_INFO = 1,
  LOG_LEVEL_WARN = 2,
  LOG_LEVEL_ERROR = 3,
  LOG_LEVEL_FATAL = 4
} LogLevel;

/* ==========================================================================
 * Log Configuration
 * ========================================================================== */

typedef struct {
  LogLevel min_level;     /* Minimum level to log */
  size_t max_file_size;   /* Max log file size before rotation (0 = no limit) */
  int max_rotated_files;  /* Number of rotated files to keep */
  bool use_stderr;        /* Log to stderr instead of file */
  char *log_path;         /* Path to log file (NULL = default) */
} LogConfig;

/* Default configuration values */
#define LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024) /* 10 MB */
#define LOG_DEFAULT_ROTATE_COUNT 5

/* ==========================================================================
 * Logging API
 * ========================================================================== */

/*
 * Initialize the logging system.
 * Must be called before any logging functions.
 *
 * @param config  Configuration (NULL for defaults)
 * @return        true on success
 */
bool log_init(const LogConfig *config);

/*
 * Shutdown the logging system.
 * Flushes and closes the log file.
 */
void log_shutdown(void);

/*
 * Write a log message.
 * Thread-safe.
 *
 * @param level   Log level
 * @param file    Source file name (__FILE__)
 * @param line    Source line number (__LINE__)
 * @param fmt     Printf-style format string
 * @param ...     Format arguments
 */
void log_write(LogLevel level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/*
 * Write a log message with va_list.
 * Thread-safe.
 */
void log_writev(LogLevel level, const char *file, int line, const char *fmt,
                va_list args);

/*
 * Emergency logging - signal-safe, no malloc.
 * Use this in signal handlers or after crashes.
 * Writes directly to log file descriptor without formatting.
 *
 * @param message  Static message to write
 */
void log_emergency(const char *message);

/*
 * Get the log file descriptor for emergency writes.
 * Returns -1 if logging is not initialized or using stderr.
 */
int log_get_fd(void);

/*
 * Flush the log file.
 * Thread-safe.
 */
void log_flush(void);

/*
 * Get the current log file path.
 * Returns NULL if logging to stderr.
 */
const char *log_get_path(void);

/*
 * Get the default log file path for the platform.
 * Caller must free the returned string.
 *
 * @return  Allocated path string, or NULL on error
 */
char *log_get_default_path(void);

/* ==========================================================================
 * Convenience Macros
 * ========================================================================== */

#define LOG_DEBUG(fmt, ...) \
  log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
  log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
  log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
  log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...) \
  log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* LACED_LOG_H */
