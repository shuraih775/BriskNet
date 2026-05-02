#ifndef LFQUEUE_WRAPPER_H
#define LFQUEUE_WRAPPER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lfqueue lfqueue_t;

/* Create a new lock-free SPSC queue. Size must be power of 2. */
lfqueue_t *lfqueue_create(size_t size);

/* Destroy the queue and free resources. */
void lfqueue_destroy(lfqueue_t *q);

/* Enqueue a single uint64_t value. Returns 0 on success, -1 if full. */
int lfqueue_enqueue(lfqueue_t *q, uint64_t value);

/* Dequeue a single uint64_t value. Returns 0 on success, -1 if empty. */
int lfqueue_dequeue(lfqueue_t *q, uint64_t *value);

/* Enqueue multiple values. Returns number enqueued (0 if full). */
size_t lfqueue_enqueue_bulk(lfqueue_t *q, const uint64_t *values, size_t n);

/* Dequeue multiple values. Returns number dequeued (0 if empty). */
size_t lfqueue_dequeue_bulk(lfqueue_t *q, uint64_t *values, size_t n);

/* Flush pending published items (call after enqueue batch). */
void lfqueue_flush(lfqueue_t *q);

/* Approximate number of items in the queue (lock-free snapshot). */
size_t lfqueue_size(lfqueue_t *q);

#ifdef __cplusplus
}
#endif

#endif
