#ifndef LACE_NC_MAINQ_H
#define LACE_NC_MAINQ_H
#include <stddef.h>

/* Completions posted by worker threads, run on the main thread. The main loop
 * polls mainq_fd() for POLLIN and calls mainq_drain(). */
typedef struct MainQueue MainQueue;
typedef void (*MainQueueFn)(void *data);

MainQueue *mainq_new(void);
void mainq_free(MainQueue *q);              /* runs what is still queued */
int mainq_fd(const MainQueue *q);
void mainq_post(MainQueue *q, MainQueueFn fn, void *data); /* any thread */
size_t mainq_drain(MainQueue *q);           /* main thread; runs every fn */
#endif
