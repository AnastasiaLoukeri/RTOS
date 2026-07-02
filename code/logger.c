/*
 * logger.c - Thread 3: the synchronous, strictly-periodic monitor.
 *
 * Wakes exactly once per second and appends one CSV line to metrics_log.txt:
 *
 *   Seconds,Nanoseconds,Commit_Count,Identity_Count,Account_Count,
 *   Info_Count,Buffer_Occupancy_Pct,CPU_Pct
 *
 * Real-time scheduling
 * --------------------
 * Drift is eliminated by sleeping to ABSOLUTE deadlines on CLOCK_MONOTONIC
 * (clock_nanosleep + TIMER_ABSTIME), advancing the deadline by exactly
 * 1.000000000 s each tick. Because every deadline is computed from a fixed
 * base (deadline_n = base + n*1s) rather than "sleep 1s from now", scheduling
 * errors do NOT accumulate: a late wake-up is absorbed by the next interval,
 * so over 24 h the program never "loses" seconds.
 *
 * CLOCK_MONOTONIC is used for the schedule (immune to NTP steps/slew), while
 * the value written to the file is CLOCK_REALTIME as the spec requires. The
 * first deadline is aligned to the next whole real-time second so the log
 * lines fall on second boundaries.
 *
 * Jitter (|actual - ideal| per tick) is measured against the monotonic
 * deadline and a min/mean/max summary is printed at shutdown. The per-second
 * jitter itself is recoverable in post-processing from the logged timestamps.
 */
#include "app.h"
#include "config.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define NS_PER_SEC 1000000000L

static void ts_add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= NS_PER_SEC) {
        t->tv_nsec -= NS_PER_SEC;
        t->tv_sec  += 1;
    }
}

/* Sleep until an absolute CLOCK_MONOTONIC deadline, retrying across EINTR. */
static int sleep_until(const struct timespec *deadline)
{
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (rc == EINTR);
    return rc;   /* 0 on success, else errno-style code */
}

void *logger_thread(void *arg)
{
    app_t *app = (app_t *)arg;

    FILE *fp = fopen(LOG_FILE_NAME, "a");
    if (!fp) {
        fprintf(stderr, "FATAL: cannot open %s: %s\n",
                LOG_FILE_NAME, strerror(errno));
        app->stop = 1;
        rb_wake_all(&app->ring);
        return NULL;
    }
    int logfd = fileno(fp);

    /* Write a CSV header only if the file is empty (fresh run). */
    if (ftell(fp) == 0) {
        fprintf(fp, "Seconds,Nanoseconds,Commit_Count,Identity_Count,"
                    "Account_Count,Info_Count,Buffer_Occupancy_Pct,CPU_Pct\n");
        fflush(fp);
    }

    cpu_sampler_t cpu;
    if (cpu_sampler_init(&cpu) != 0)
        fprintf(stderr, "WARN: cannot open %s, CPU%% will be -1\n", PROC_STAT_PATH);
    cpu_sampler_busy_pct(&cpu);   /* prime the first delta */

    /* ---- Align the first deadline to the next whole real-time second ---- */
    struct timespec rt_now, mono_now;
    clock_gettime(CLOCK_REALTIME,  &rt_now);
    clock_gettime(CLOCK_MONOTONIC, &mono_now);

    long ns_to_next = NS_PER_SEC - rt_now.tv_nsec;   /* time to next .000... */
    if (ns_to_next == NS_PER_SEC) ns_to_next = 0;

    struct timespec deadline = mono_now;
    ts_add_ns(&deadline, ns_to_next);

    /* ---- Jitter accounting (against the monotonic ideal) ---------------- */
    double jit_min = 1e18, jit_max = -1e18, jit_sum = 0.0;
    uint64_t ticks = 0;
    int fsync_ctr = 0;

    while (!app->stop) {
        if (sleep_until(&deadline) != 0 && app->stop)
            break;

        /* Measure how far the actual wake-up is from the ideal deadline. */
        struct timespec woke;
        clock_gettime(CLOCK_MONOTONIC, &woke);
        double jitter_ms =
            ((double)(woke.tv_sec  - deadline.tv_sec)  * 1000.0) +
            ((double)(woke.tv_nsec - deadline.tv_nsec) / 1.0e6);
        if (jitter_ms < jit_min) jit_min = jitter_ms;
        if (jitter_ms > jit_max) jit_max = jitter_ms;
        jit_sum += jitter_ms;
        ticks++;

        /* Wall-clock timestamp to log (spec: CLOCK_REALTIME). */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        /* Snapshot + reset the per-second counters under the stats mutex. */
        uint64_t commit, identity, account, info;
        counters_snapshot_reset(&app->counters,
                                &commit, &identity, &account, &info);

        /* Instantaneous metrics. */
        double occ = rb_occupancy_pct(&app->ring);
        double cpu_pct = cpu_sampler_busy_pct(&cpu);

        /* Format + I/O OUTSIDE every lock (locks held only for snapshots). */
        fprintf(fp, "%lld,%ld,%llu,%llu,%llu,%llu,%.2f,%.2f\n",
                (long long)ts.tv_sec, ts.tv_nsec,
                (unsigned long long)commit,
                (unsigned long long)identity,
                (unsigned long long)account,
                (unsigned long long)info,
                occ, cpu_pct);
        fflush(fp);                       /* push to kernel page cache       */

        if (++fsync_ctr >= LOG_FSYNC_EVERY) {   /* durability vs SD wear     */
            fsync(logfd);
            fsync_ctr = 0;
        }

        ts_add_ns(&deadline, LOG_PERIOD_NS);   /* fixed step => no drift */
    }

    /* ---- Clean shutdown: flush everything to stable storage ------------- */
    fflush(fp);
    fsync(logfd);
    fclose(fp);
    cpu_sampler_close(&cpu);

    if (ticks) {
        fprintf(stderr,
            "logger: %llu ticks | jitter ms  min=%.3f mean=%.3f max=%.3f\n",
            (unsigned long long)ticks, jit_min, jit_sum / (double)ticks, jit_max);
    }
    return NULL;
}
