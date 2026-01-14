/*
 * Lace
 * Platform-independent thread abstraction
 *
 * Provides a thin wrapper over platform-specific threading primitives
 * (pthreads on POSIX, Win32 threads on Windows).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_THREAD_H
#define LACE_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Platform Detection
 * ============================================================================
 */

#if defined(_WIN32) || defined(_WIN64)
#define LACE_PLATFORM_WINDOWS 1
#else
#define LACE_PLATFORM_POSIX 1
#endif

/* ============================================================================
 * Type Definitions (platform-specific in implementation)
 * ============================================================================
 */

#ifdef LACE_PLATFORM_WINDOWS
/* Windows types - opaque pointers */
typedef void *lace_thread_t;
typedef void *lace_mutex_t;
typedef void *lace_cond_t;
#else
/* POSIX types - use pthread directly for efficiency */
#include <pthread.h>
typedef pthread_t lace_thread_t;
typedef pthread_mutex_t lace_mutex_t;
typedef pthread_cond_t lace_cond_t;
#endif

/* Thread function signature */
typedef void *(*lace_thread_func_t)(void *arg);

/* Thread attributes */
typedef struct {
  size_t stack_size; /* Stack size in bytes (0 = default) */
  bool detached;     /* Create as detached thread */
} lace_thread_attr_t;

/* ============================================================================
 * Thread Functions
 * ============================================================================
 */

/* Initialize thread attributes with defaults */
void lace_thread_attr_init(lace_thread_attr_t *attr);

/* Create a new thread */
bool lace_thread_create(lace_thread_t *thread, const lace_thread_attr_t *attr,
                        lace_thread_func_t func, void *arg);

/* Wait for thread to complete (join) */
bool lace_thread_join(lace_thread_t thread, void **retval);

/* Detach a thread (thread will clean up itself) */
bool lace_thread_detach(lace_thread_t thread);

/* ============================================================================
 * Mutex Functions
 * ============================================================================
 */

/* Initialize a mutex */
bool lace_mutex_init(lace_mutex_t *mutex);

/* Destroy a mutex */
void lace_mutex_destroy(lace_mutex_t *mutex);

/* Lock a mutex */
void lace_mutex_lock(lace_mutex_t *mutex);

/* Unlock a mutex */
void lace_mutex_unlock(lace_mutex_t *mutex);

/* ============================================================================
 * Condition Variable Functions
 * ============================================================================
 */

/* Initialize a condition variable */
bool lace_cond_init(lace_cond_t *cond);

/* Destroy a condition variable */
void lace_cond_destroy(lace_cond_t *cond);

/* Signal one waiting thread */
void lace_cond_signal(lace_cond_t *cond);

/* Broadcast to all waiting threads */
void lace_cond_broadcast(lace_cond_t *cond);

/* Wait on condition variable (mutex must be locked) */
void lace_cond_wait(lace_cond_t *cond, lace_mutex_t *mutex);

/* Wait on condition variable with timeout (returns false on timeout) */
bool lace_cond_timedwait(lace_cond_t *cond, lace_mutex_t *mutex,
                         int timeout_ms);

/* ============================================================================
 * Time Functions
 * ============================================================================
 */

/* Get current time in milliseconds (monotonic if available) */
uint64_t lace_time_ms(void);

/* Sleep for specified milliseconds */
void lace_sleep_ms(int ms);

#endif /* LACE_THREAD_H */
