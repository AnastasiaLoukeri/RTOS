/*
 * main.c - wires the three threads together and handles graceful shutdown.
 *
 * Shutdown design (no async-signal-safety pitfalls):
 *   - SIGINT/SIGTERM are BLOCKED in every thread.
 *   - The main thread waits for them with sigwait() (synchronously).
 *   - On signal: set app.stop, wake the consumer, join all threads, then
 *     free resources. This guarantees the log file is flushed and closed
 *     and there are no leaks, so the program can be stopped at any time and
 *     run again, or run unattended for days under a supervisor.
 */
#include "app.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <inttypes.h>

int main(void)
{
    app_t app;
    memset(&app, 0, sizeof(app));

    if (rb_init(&app.ring, RB_CAPACITY, RB_SLOT_SIZE) != 0) {
        fprintf(stderr, "FATAL: ring buffer allocation failed\n");
        return EXIT_FAILURE;
    }
    counters_init(&app.counters);

    /* Don't die if the network peer disappears mid-write. */
    signal(SIGPIPE, SIG_IGN);

    /* Block the termination signals in this thread; the threads we create
     * inherit the mask, so only the main thread (via sigwait) handles them. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t t_prod, t_cons, t_log;
    int e1 = pthread_create(&t_cons, NULL, consumer_thread, &app);
    int e2 = pthread_create(&t_log,  NULL, logger_thread,   &app);
    int e3 = pthread_create(&t_prod, NULL, producer_thread, &app);
    if (e1 || e2 || e3) {
        fprintf(stderr, "FATAL: pthread_create failed\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "telemetry collector running. Ctrl-C (SIGINT) to stop.\n");

    /* Wait for a termination signal. */
    int sig = 0;
    sigwait(&set, &sig);
    fprintf(stderr, "\nsignal %d received, shutting down...\n", sig);

    /* Orchestrate a clean stop. */
    app.stop = 1;
    rb_wake_all(&app.ring);     /* unblock the consumer from rb_peek()       */

    pthread_join(t_prod, NULL); /* stops the network, frees lws context      */
    rb_wake_all(&app.ring);     /* in case the consumer is still waiting     */
    pthread_join(t_cons, NULL);
    pthread_join(t_log,  NULL); /* wakes at its next deadline (<= 1 s)        */

    /* Diagnostics that document long-run health (data-loss accounting). */
    uint64_t dropped = 0, oversized = 0, pushed = 0;
    unsigned hwm = 0;
    rb_stats(&app.ring, &dropped, &oversized, &pushed, &hwm);
    fprintf(stderr,
        "summary: enqueued=%" PRIu64 " dropped(full)=%" PRIu64
        " oversized=%" PRIu64 " unknown_kind=%" PRIu64
        " buffer_high_watermark=%u/%u\n",
        pushed, dropped, oversized, counters_other(&app.counters),
        hwm, RB_CAPACITY);

    counters_destroy(&app.counters);
    rb_destroy(&app.ring);
    return EXIT_SUCCESS;
}
