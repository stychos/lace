/*
 * laced - Lace Database Daemon
 * Crash protection and signal handling
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACED_CRASH_H
#define LACED_CRASH_H

#include <stdbool.h>

/* ==========================================================================
 * Crash Handler Types
 * ========================================================================== */

/*
 * Cleanup callback type.
 * Called before process exits after a crash.
 * Must be signal-safe (no malloc, no stdio, no locks).
 */
typedef void (*CrashCleanupCallback)(void);

/* ==========================================================================
 * Crash Handler API
 * ========================================================================== */

/*
 * Install crash handlers for fatal signals.
 * Installs handlers for: SIGSEGV, SIGBUS, SIGFPE, SIGABRT, SIGILL
 *
 * @param cleanup  Optional cleanup callback (called before exit)
 * @return         true on success
 */
bool crash_handler_install(CrashCleanupCallback cleanup);

/*
 * Uninstall crash handlers and restore defaults.
 */
void crash_handler_uninstall(void);

/*
 * Write backtrace to log (signal-safe implementation).
 * Can be called from signal handlers.
 */
void crash_write_backtrace(void);

/*
 * Set additional context information for crash reports.
 * This is written to the log on crash.
 * Not signal-safe - call before crash handlers trigger.
 *
 * @param context  Static string (must remain valid until crash or clear)
 */
void crash_set_context(const char *context);

/*
 * Clear crash context.
 */
void crash_clear_context(void);

#endif /* LACED_CRASH_H */
