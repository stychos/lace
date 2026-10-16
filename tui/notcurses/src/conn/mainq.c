#include "conn/mainq.h"
#include "util/mem.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

struct Item {
  MainQueueFn fn;
  void *data;
  struct Item *next;
};

struct MainQueue {
  pthread_mutex_t lock;
  struct Item *head, *tail;
  int fds[2];
};

/* Close-on-exec and non-blocking (pipe2() is Linux-only) */
static bool pipe_end_setup(int fd) {
  int fl = fcntl(fd, F_GETFL);
  return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fl != -1 &&
         fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

MainQueue *mainq_new(void) {
  MainQueue *q = safe_calloc(1, sizeof *q);
  if (pipe(q->fds) != 0) {
    free(q);
    return NULL;
  }
  if (!pipe_end_setup(q->fds[0]) || !pipe_end_setup(q->fds[1])) {
    close(q->fds[0]);
    close(q->fds[1]);
    free(q);
    return NULL;
  }
  pthread_mutex_init(&q->lock, NULL);
  return q;
}

int mainq_fd(const MainQueue *q) { return q->fds[0]; }

void mainq_post(MainQueue *q, MainQueueFn fn, void *data) {
  struct Item *it = safe_calloc(1, sizeof *it);
  it->fn = fn;
  it->data = data;
  pthread_mutex_lock(&q->lock);
  if (q->tail) q->tail->next = it;
  else q->head = it;
  q->tail = it;
  pthread_mutex_unlock(&q->lock);
  char b = 1;
  ssize_t r = write(q->fds[1], &b, 1);
  (void)r; /* full pipe: a byte is already pending */
}

size_t mainq_drain(MainQueue *q) {
  char buf[64];
  while (read(q->fds[0], buf, sizeof buf) > 0) {}
  pthread_mutex_lock(&q->lock);
  struct Item *it = q->head;
  q->head = q->tail = NULL;
  pthread_mutex_unlock(&q->lock);
  size_t n = 0;
  /* Items posted by the callbacks make the pipe readable for the next pass */
  while (it) {
    struct Item *next = it->next;
    it->fn(it->data);
    free(it);
    it = next;
    n++;
  }
  return n;
}

void mainq_free(MainQueue *q) {
  if (!q) return;
  mainq_drain(q);
  close(q->fds[0]);
  close(q->fds[1]);
  pthread_mutex_destroy(&q->lock);
  free(q);
}
