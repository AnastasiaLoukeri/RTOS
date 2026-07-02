/*
 * app.h - The single shared context handed to every thread.
 *
 * Only three pieces of state are shared between threads, each with a clear
 * ownership/synchronisation rule:
 *   - ring      : the bounded queue (own mutex + cond inside)
 *   - counters  : message-type counters (own mutex inside)
 *   - stop      : cooperative shutdown flag (sig_atomic_t, set once)
 */
#ifndef APP_H
#define APP_H

#include <signal.h>
#include "ring_buffer.h"
#include "stats.h"

typedef struct {
    ring_buffer_t           ring;
    counters_t              counters;
    volatile sig_atomic_t   stop;     /* 0 = run, 1 = shut down              */
} app_t;

void *producer_thread(void *arg);   /* arg = app_t*  (network, libwebsockets)*/
void *consumer_thread(void *arg);   /* arg = app_t*  (parse + classify)      */
void *logger_thread  (void *arg);   /* arg = app_t*  (periodic CSV monitor)  */

#endif /* APP_H */
