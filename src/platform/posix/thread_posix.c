/*
 * Lace
 * POSIX thread implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../thread.h"

#ifdef LACE_PLATFORM_POSIX

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Thread Functions
 * ============================================================================
 */

void lace_thread_attr_init(lace_thread_attr_t *attr) {
  if (!attr)
    return;
  attr->stack_size = 0; /* Use system default */
  attr->detached = false;
}

bool lace_thread_create(lace_thread_t *thread, const lace_thread_attr_t *attr,
                        lace_thread_func_t func, void *arg) {
  if (!thread || !func)
    return false;

  pthread_attr_t pattr;
  pthread_attr_init(&pattr);

  if (attr) {
    if (attr->stack_size > 0) {
      pthread_attr_setstacksize(&pattr, attr->stack_size);
    }
    if (attr->detached) {
      pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);
    }
  }

  int result = pthread_create(thread, &pattr, func, arg);
  pthread_attr_destroy(&pattr);

  return result == 0;
}

bool lace_thread_join(lace_thread_t thread, void **retval) {
  return pthread_join(thread, retval) == 0;
}

bool lace_thread_detach(lace_thread_t thread) {
  return pthread_detach(thread) == 0;
}

/* ============================================================================
 * Mutex Functions
 * ============================================================================
 */

bool lace_mutex_init(lace_mutex_t *mutex) {
  if (!mutex)
    return false;
  return pthread_mutex_init(mutex, NULL) == 0;
}

void lace_mutex_destroy(lace_mutex_t *mutex) {
  if (mutex) {
    pthread_mutex_destroy(mutex);
  }
}

void lace_mutex_lock(lace_mutex_t *mutex) {
  if (mutex) {
    pthread_mutex_lock(mutex);
  }
}

void lace_mutex_unlock(lace_mutex_t *mutex) {
  if (mutex) {
    pthread_mutex_unlock(mutex);
  }
}

/* ============================================================================
 * Condition Variable Functions
 * ============================================================================
 */

bool lace_cond_init(lace_cond_t *cond) {
  if (!cond)
    return false;
  return pthread_cond_init(cond, NULL) == 0;
}

void lace_cond_destroy(lace_cond_t *cond) {
  if (cond) {
    pthread_cond_destroy(cond);
  }
}

void lace_cond_signal(lace_cond_t *cond) {
  if (cond) {
    pthread_cond_signal(cond);
  }
}

void lace_cond_broadcast(lace_cond_t *cond) {
  if (cond) {
    pthread_cond_broadcast(cond);
  }
}

void lace_cond_wait(lace_cond_t *cond, lace_mutex_t *mutex) {
  if (cond && mutex) {
    pthread_cond_wait(cond, mutex);
  }
}

bool lace_cond_timedwait(lace_cond_t *cond, lace_mutex_t *mutex,
                         int timeout_ms) {
  if (!cond || !mutex)
    return false;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }

  return pthread_cond_timedwait(cond, mutex, &ts) != ETIMEDOUT;
}

/* ============================================================================
 * Time Functions
 * ============================================================================
 */

uint64_t lace_time_ms(void) {
  struct timespec ts;
#ifdef CLOCK_MONOTONIC
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void lace_sleep_ms(int ms) {
  if (ms > 0) {
    usleep((useconds_t)ms * 1000);
  }
}

#endif /* LACE_PLATFORM_POSIX */
