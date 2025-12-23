/*
 * Lace
 * Win32 thread implementation (stub for future Windows support)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "thread.h"

#ifdef LACE_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Internal structures for opaque types */
typedef struct {
  CRITICAL_SECTION cs;
} lace_mutex_internal_t;

typedef struct {
  CONDITION_VARIABLE cv;
} lace_cond_internal_t;

/* ============================================================================
 * Thread Functions
 * ============================================================================
 */

void lace_thread_attr_init(lace_thread_attr_t *attr) {
  if (!attr)
    return;
  attr->stack_size = 0;
  attr->detached = false;
}

/* Wrapper structure for thread function */
typedef struct {
  lace_thread_func_t func;
  void *arg;
} thread_wrapper_t;

static DWORD WINAPI thread_wrapper_func(LPVOID param) {
  thread_wrapper_t *wrapper = (thread_wrapper_t *)param;
  lace_thread_func_t func = wrapper->func;
  void *arg = wrapper->arg;
  free(wrapper);
  func(arg);
  return 0;
}

bool lace_thread_create(lace_thread_t *thread, const lace_thread_attr_t *attr,
                        lace_thread_func_t func, void *arg) {
  if (!thread || !func)
    return false;

  thread_wrapper_t *wrapper = malloc(sizeof(thread_wrapper_t));
  if (!wrapper)
    return false;

  wrapper->func = func;
  wrapper->arg = arg;

  SIZE_T stack_size = (attr && attr->stack_size > 0) ? attr->stack_size : 0;
  HANDLE h =
      CreateThread(NULL, stack_size, thread_wrapper_func, wrapper, 0, NULL);
  if (!h) {
    free(wrapper);
    return false;
  }

  if (attr && attr->detached) {
    CloseHandle(h);
    *thread = NULL;
  } else {
    *thread = h;
  }
  return true;
}

bool lace_thread_join(lace_thread_t thread, void **retval) {
  if (!thread)
    return false;
  DWORD result = WaitForSingleObject((HANDLE)thread, INFINITE);
  if (result == WAIT_OBJECT_0) {
    CloseHandle((HANDLE)thread);
    if (retval)
      *retval = NULL;
    return true;
  }
  return false;
}

bool lace_thread_detach(lace_thread_t thread) {
  if (thread) {
    CloseHandle((HANDLE)thread);
  }
  return true;
}

/* ============================================================================
 * Mutex Functions
 * ============================================================================
 */

bool lace_mutex_init(lace_mutex_t *mutex) {
  if (!mutex)
    return false;
  lace_mutex_internal_t *m = malloc(sizeof(lace_mutex_internal_t));
  if (!m)
    return false;
  InitializeCriticalSection(&m->cs);
  *mutex = m;
  return true;
}

void lace_mutex_destroy(lace_mutex_t *mutex) {
  if (mutex && *mutex) {
    lace_mutex_internal_t *m = (lace_mutex_internal_t *)*mutex;
    DeleteCriticalSection(&m->cs);
    free(m);
    *mutex = NULL;
  }
}

void lace_mutex_lock(lace_mutex_t *mutex) {
  if (mutex && *mutex) {
    lace_mutex_internal_t *m = (lace_mutex_internal_t *)*mutex;
    EnterCriticalSection(&m->cs);
  }
}

void lace_mutex_unlock(lace_mutex_t *mutex) {
  if (mutex && *mutex) {
    lace_mutex_internal_t *m = (lace_mutex_internal_t *)*mutex;
    LeaveCriticalSection(&m->cs);
  }
}

/* ============================================================================
 * Condition Variable Functions
 * ============================================================================
 */

bool lace_cond_init(lace_cond_t *cond) {
  if (!cond)
    return false;
  lace_cond_internal_t *c = malloc(sizeof(lace_cond_internal_t));
  if (!c)
    return false;
  InitializeConditionVariable(&c->cv);
  *cond = c;
  return true;
}

void lace_cond_destroy(lace_cond_t *cond) {
  if (cond && *cond) {
    /* Condition variables don't need explicit destruction on Windows */
    free(*cond);
    *cond = NULL;
  }
}

void lace_cond_signal(lace_cond_t *cond) {
  if (cond && *cond) {
    lace_cond_internal_t *c = (lace_cond_internal_t *)*cond;
    WakeConditionVariable(&c->cv);
  }
}

void lace_cond_broadcast(lace_cond_t *cond) {
  if (cond && *cond) {
    lace_cond_internal_t *c = (lace_cond_internal_t *)*cond;
    WakeAllConditionVariable(&c->cv);
  }
}

void lace_cond_wait(lace_cond_t *cond, lace_mutex_t *mutex) {
  if (cond && *cond && mutex && *mutex) {
    lace_cond_internal_t *c = (lace_cond_internal_t *)*cond;
    lace_mutex_internal_t *m = (lace_mutex_internal_t *)*mutex;
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
  }
}

bool lace_cond_timedwait(lace_cond_t *cond, lace_mutex_t *mutex,
                         int timeout_ms) {
  if (!cond || !*cond || !mutex || !*mutex)
    return false;
  lace_cond_internal_t *c = (lace_cond_internal_t *)*cond;
  lace_mutex_internal_t *m = (lace_mutex_internal_t *)*mutex;
  return SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)timeout_ms) != 0;
}

/* ============================================================================
 * Time Functions
 * ============================================================================
 */

uint64_t lace_time_ms(void) {
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
}

void lace_sleep_ms(int ms) {
  if (ms > 0) {
    Sleep((DWORD)ms);
  }
}

#endif /* LACE_PLATFORM_WINDOWS */
