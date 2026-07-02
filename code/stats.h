/*
 * stats.h - Global message counters (Consumer writes, Logger reads+resets)
 *           and the /proc/stat CPU-usage sampler (Logger only).
 *
 * The 4 counters required by the assignment correspond to the Jetstream
 * "kind" field: commit, identity, account, info. A 5th "other" counter
 * catches anything unexpected (unknown kind or unparseable JSON) so nothing
 * is silently lost; it is not written to the CSV but is reported at exit.
 */
#ifndef STATS_H
#define STATS_H

#include <pthread.h>
#include <stdint.h>

typedef struct {
    uint64_t commit;
    uint64_t identity;
    uint64_t account;
    uint64_t info;
    uint64_t other;     /* unknown kind / parse failure (diagnostics only)  */
    pthread_mutex_t lock;
} counters_t;

void counters_init(counters_t *c);
void counters_destroy(counters_t *c);

/* Consumer: classify one NUL-terminated JSON message and bump a counter. */
void counters_classify(counters_t *c, const char *json, size_t len);

/* Logger: atomically read the 4 CSV counters AND reset them to 0 for the
 * next 1-second window. `other` is read separately for diagnostics. */
void counters_snapshot_reset(counters_t *c,
                             uint64_t *commit, uint64_t *identity,
                             uint64_t *account, uint64_t *info);

uint64_t counters_other(counters_t *c);

/* ---- CPU usage from /proc/stat ----------------------------------------- *
 * Keeps the previous (total,idle) jiffie reading and returns the busy %
 * over the interval since the last call. Returns -1.0 on read error.       */
typedef struct {
    int      fd;            /* kept open to avoid per-second open() cost     */
    uint64_t prev_total;
    uint64_t prev_idle;
    int      primed;
} cpu_sampler_t;

int    cpu_sampler_init(cpu_sampler_t *s);   /* 0 ok, -1 cannot open /proc  */
void   cpu_sampler_close(cpu_sampler_t *s);
double cpu_sampler_busy_pct(cpu_sampler_t *s); /* busy% since last call     */

#endif /* STATS_H */
