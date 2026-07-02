/*
 * consumer.c - Thread 2: event-driven consumer.
 *
 * Sleeps on the ring's condition variable until the producer enqueues data,
 * then dequeues each JSON string, parses it with cJSON, reads the "kind"
 * field and increments the matching mutex-protected counter.
 *
 * HARD RULE from the spec: this thread performs NO printf() and NO file I/O.
 * It does the absolute minimum so it can keep up with bursty traffic and
 * thus keep the buffer occupancy low.
 */
#include "app.h"

void *consumer_thread(void *arg)
{
    app_t *app = (app_t *)arg;

    for (;;) {
        const char *msg;
        size_t      len;

        /* Blocks until a message is available, or returns 0 once the ring is
         * empty AND shutdown has been requested -> drains cleanly on exit. */
        if (!rb_peek(&app->ring, &msg, &len, &app->stop))
            break;

        counters_classify(&app->counters, msg, len);

        rb_consume(&app->ring);   /* release the slot for the producer */
    }
    return NULL;
}
