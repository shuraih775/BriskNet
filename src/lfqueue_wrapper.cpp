#include "../include/lfqueue_wrapper.h"
#include "../external/lfqueue/include/ring.hpp"

using packet_t = uint64_t;
using RingType = lockfree::Ring<packet_t, true, true>;

struct lfqueue
{
    RingType ring;

    lfqueue(size_t size) : ring(size) {}
};

extern "C" {

lfqueue_t *lfqueue_create(size_t size)
{
    return new (std::nothrow) lfqueue(size);
}

void lfqueue_destroy(lfqueue_t *q)
{
    delete q;
}

int lfqueue_enqueue(lfqueue_t *q, uint64_t value)
{
    return q->ring.enqueue(value) ? 0 : -1;
}

int lfqueue_dequeue(lfqueue_t *q, uint64_t *value)
{
    return q->ring.dequeue(*value) ? 0 : -1;
}

size_t lfqueue_enqueue_bulk(lfqueue_t *q, const uint64_t *values, size_t n)
{
    return q->ring.enqueue_bulk(values, n);
}

size_t lfqueue_dequeue_bulk(lfqueue_t *q, uint64_t *values, size_t n)
{
    return q->ring.dequeue_bulk(values, n);
}

void lfqueue_flush(lfqueue_t *q)
{
    q->ring.flush();
}

size_t lfqueue_size(lfqueue_t *q)
{
    return q->ring.size();
}

} /* extern "C" */
