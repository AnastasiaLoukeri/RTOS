/* ring_buffer.c - see ring_buffer.h for the design rationale. */
#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>

int rb_init(ring_buffer_t *rb, unsigned capacity, unsigned slot_size)
{
    memset(rb, 0, sizeof(*rb));
    rb->capacity  = capacity;
    rb->slot_size = slot_size;

    /* One big contiguous block keeps the allocator happy and locality high. */
    rb->slots = malloc((size_t)capacity * slot_size);
    rb->lens  = malloc((size_t)capacity * sizeof(size_t));
    if (!rb->slots || !rb->lens) {
        free(rb->slots);
        free(rb->lens);
        return -1;
    }

    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    return 0;
}

void rb_destroy(ring_buffer_t *rb)
{
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->not_empty);
    free(rb->slots);
    free(rb->lens);
    rb->slots = NULL;
    rb->lens  = NULL;
}

int rb_push(ring_buffer_t *rb, const char *data, size_t len)
{
    /* Reject oversized frames *before* taking the lock to keep it short. */
    if (len + 1 > rb->slot_size) {
        pthread_mutex_lock(&rb->lock);
        rb->oversized++;
        pthread_mutex_unlock(&rb->lock);
        return -1;
    }

    pthread_mutex_lock(&rb->lock);

    if (rb->count == rb->capacity) {        /* full -> drop newest, NO block */
        rb->dropped++;
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }

    char *dst = rb->slots + (size_t)rb->tail * rb->slot_size;
    memcpy(dst, data, len);
    dst[len] = '\0';                        /* NUL-terminate for the parser  */
    rb->lens[rb->tail] = len;

    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count++;
    rb->pushed++;
    if (rb->count > rb->high_watermark)
        rb->high_watermark = rb->count;

    /* Signal while holding the lock: simplest correct ordering. */
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
    return 1;
}

int rb_peek(ring_buffer_t *rb, const char **out_ptr, size_t *out_len,
            volatile int *stop_flag)
{
    pthread_mutex_lock(&rb->lock);
    while (rb->count == 0 && !(stop_flag && *stop_flag))
        pthread_cond_wait(&rb->not_empty, &rb->lock);

    if (rb->count == 0) {                   /* woke up only because of stop  */
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }

    /* The head slot is guaranteed not to be touched by the producer until
     * rb_consume() decrements count, so we can read it without the lock. */
    *out_ptr = rb->slots + (size_t)rb->head * rb->slot_size;
    *out_len = rb->lens[rb->head];
    pthread_mutex_unlock(&rb->lock);
    return 1;
}

void rb_consume(ring_buffer_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    if (rb->count > 0) {
        rb->head = (rb->head + 1) % rb->capacity;
        rb->count--;
    }
    pthread_mutex_unlock(&rb->lock);
}

void rb_wake_all(ring_buffer_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
}

double rb_occupancy_pct(ring_buffer_t *rb)
{
    pthread_mutex_lock(&rb->lock);
    double pct = (double)rb->count * 100.0 / (double)rb->capacity;
    pthread_mutex_unlock(&rb->lock);
    return pct;
}

void rb_stats(ring_buffer_t *rb, uint64_t *dropped, uint64_t *oversized,
              uint64_t *pushed, unsigned *high_watermark)
{
    pthread_mutex_lock(&rb->lock);
    if (dropped)        *dropped        = rb->dropped;
    if (oversized)      *oversized      = rb->oversized;
    if (pushed)         *pushed         = rb->pushed;
    if (high_watermark) *high_watermark = rb->high_watermark;
    pthread_mutex_unlock(&rb->lock);
}
