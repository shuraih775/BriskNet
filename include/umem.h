#ifndef UMEM_H
#define UMEM_H

#include <xdp/xsk.h>
#include <stdint.h>

#define FRAME_SIZE 2048
#define FRAME_COUNT 4096
#define UMEM_SIZE (FRAME_SIZE * FRAME_COUNT)

#define RX_RING_SIZE 2048
#define TX_RING_SIZE 2048

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