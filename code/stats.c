/* stats.c - see stats.h */
#include "stats.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <cjson/cJSON.h>

void counters_init(counters_t *c)
{
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);
}

void counters_destroy(counters_t *c)
{
    pthread_mutex_destroy(&c->lock);
}

void counters_classify(counters_t *c, const char *json, size_t len)
{
    /* Parse with an explicit length: robust even if a NUL slipped in. */
    cJSON *root = cJSON_ParseWithLength(json, len);

    /* Decide the bucket first, THEN take the lock for the shortest time. */
    enum { K_COMMIT, K_IDENTITY, K_ACCOUNT, K_INFO, K_OTHER } bucket = K_OTHER;

    if (root) {
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
        if (cJSON_IsString(kind) && kind->valuestring) {
            const char *k = kind->valuestring;
            if      (strcmp(k, "commit")   == 0) bucket = K_COMMIT;
            else if (strcmp(k, "identity") == 0) bucket = K_IDENTITY;
            else if (strcmp(k, "account")  == 0) bucket = K_ACCOUNT;
            else if (strcmp(k, "info")     == 0) bucket = K_INFO;
        }
        cJSON_Delete(root);     /* free immediately -> no memory growth */
    }

    pthread_mutex_lock(&c->lock);
    switch (bucket) {
        case K_COMMIT:   c->commit++;   break;
        case K_IDENTITY: c->identity++; break;
        case K_ACCOUNT:  c->account++;  break;
        case K_INFO:     c->info++;     break;
        default:         c->other++;    break;
    }
    pthread_mutex_unlock(&c->lock);
}

void counters_snapshot_reset(counters_t *c,
                             uint64_t *commit, uint64_t *identity,
                             uint64_t *account, uint64_t *info)
{
    pthread_mutex_lock(&c->lock);
    *commit   = c->commit;   c->commit   = 0;
    *identity = c->identity; c->identity = 0;
    *account  = c->account;  c->account  = 0;
    *info     = c->info;     c->info     = 0;
    pthread_mutex_unlock(&c->lock);
}

uint64_t counters_other(counters_t *c)
{
    pthread_mutex_lock(&c->lock);
    uint64_t v = c->other;
    pthread_mutex_unlock(&c->lock);
    return v;
}

/* ----------------------------------------------------------------------- */

int cpu_sampler_init(cpu_sampler_t *s)
{
    memset(s, 0, sizeof(*s));
    s->fd = open(PROC_STAT_PATH, O_RDONLY);
    return (s->fd < 0) ? -1 : 0;
}

void cpu_sampler_close(cpu_sampler_t *s)
{
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
}

/*
 * /proc/stat first line:
 *   cpu  user nice system idle iowait irq softirq steal guest guest_nice
 * idle time   = idle + iowait
 * busy time   = everything else
 * We compute the busy fraction over the delta between two reads.
 */
double cpu_sampler_busy_pct(cpu_sampler_t *s)
{
    if (s->fd < 0) return -1.0;

    char buf[512];
    if (lseek(s->fd, 0, SEEK_SET) < 0) return -1.0;
    ssize_t n = read(s->fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1.0;
    buf[n] = '\0';

    /* First line begins with "cpu " (aggregate over all cores). */
    unsigned long long v[10] = {0};
    int got = sscanf(buf,
        "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (got < 4) return -1.0;

    uint64_t idle  = v[3] + v[4];                 /* idle + iowait          */
    uint64_t total = 0;
    for (int i = 0; i < 10; i++) total += v[i];

    double pct = 0.0;
    if (s->primed) {
        uint64_t dt = total - s->prev_total;
        uint64_t di = idle  - s->prev_idle;
        if (dt > 0)
            pct = 100.0 * (double)(dt - di) / (double)dt;
    }
    s->prev_total = total;
    s->prev_idle  = idle;
    s->primed     = 1;
    return pct;
}
