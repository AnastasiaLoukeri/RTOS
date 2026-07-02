/*
 * ring_buffer.h - Bounded circular queue for the Producer->Consumer hand-off.
 *
 * Design notes
 * ------------
 *  - Fixed array of fixed-size slots (no per-item heap allocation).
 *  - Protected by ONE mutex + ONE condition variable (rb_not_empty).
 *  - The Producer NEVER blocks: if the ring is full it drops the newest
 *    message and increments `dropped` (back-pressure is measured, not hidden,
 *    via the buffer-occupancy metric). This guarantees the libwebsockets
 *    network callback returns immediately and no socket packets are lost.
 *  - Single Consumer uses a zero-copy peek/consume pattern: it parses the
 *    head slot in place (safe, because the Producer only ever writes the
 *    `tail` slot and the head slot stays "occupied" until rb_consume()).
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char    *slots;         /* RB_CAPACITY * RB_SLOT_SIZE contiguous bytes  */
    size_t  *lens;          /* payload length stored in each slot           */
    unsigned capacity;
    unsigned slot_size;

    unsigned head;          /* next slot to read  (consumer)                */
    unsigned tail;          /* next slot to write (producer)                */
    unsigned count;         /* number of occupied slots                     */

    uint64_t dropped;       /* msgs dropped because the ring was full       */
    uint64_t oversized;     /* msgs dropped because they exceeded slot_size */
    uint64_t pushed;        /* total successfully enqueued                  */

    unsigned high_watermark;/* max `count` ever observed (diagnostics)      */

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} ring_buffer_t;

/* Returns 0 on success, -1 on allocation failure. */
int  rb_init(ring_buffer_t *rb, unsigned capacity, unsigned slot_size);
void rb_destroy(ring_buffer_t *rb);

/* PRODUCER side: non-blocking enqueue. Returns:
 *    1  enqueued, 0  dropped (full), -1 dropped (oversized). Never blocks. */
int  rb_push(ring_buffer_t *rb, const char *data, size_t len);

/* CONSUMER side: block until an item is available OR stop is signalled.
 *  - On success: returns 1 and sets *out_ptr / *out_len to the head slot
 *    (valid until the next rb_consume()).
 *  - Returns 0 if the ring is empty AND *stop_flag became non-zero. */
int  rb_peek(ring_buffer_t *rb, const char **out_ptr, size_t *out_len,
             volatile int *stop_flag);

/* CONSUMER side: release the slot returned by the last rb_peek(). */
void rb_consume(ring_buffer_t *rb);

/* Wake any thread blocked in rb_peek() (used at shutdown). */
void rb_wake_all(ring_buffer_t *rb);

/* Instantaneous occupancy as a percentage [0..100]. Thread-safe. */
double rb_occupancy_pct(ring_buffer_t *rb);

/* Snapshot of diagnostics counters. Thread-safe. */
void rb_stats(ring_buffer_t *rb, uint64_t *dropped, uint64_t *oversized,
              uint64_t *pushed, unsigned *high_watermark);

#endif /* RING_BUFFER_H */
