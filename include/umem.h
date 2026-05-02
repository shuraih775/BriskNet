#ifndef UMEM_H
#define UMEM_H

#include <xdp/xsk.h>
#include <stdint.h>

/* Frame size must be a power of 2 and >= 2048 for XDP alignment requirements */
#define FRAME_SIZE 2048
#define FRAME_COUNT 4096
#define UMEM_SIZE (FRAME_SIZE * FRAME_COUNT)

_Static_assert((FRAME_SIZE & (FRAME_SIZE - 1)) == 0,
               "FRAME_SIZE must be a power of 2");
_Static_assert(FRAME_SIZE >= 2048,
               "FRAME_SIZE must be at least 2048 bytes");

#define RX_RING_SIZE 4096
#define TX_RING_SIZE 4096

struct umem_info
{
    void *buffer;
    struct xsk_umem *umem;

    struct xsk_ring_prod fill_q;
    struct xsk_ring_cons comp_q;
};

int umem_create(struct umem_info *umem);
void umem_free(struct umem_info *umem);

#endif