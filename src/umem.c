#include "../include/umem.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int umem_create(struct umem_info *u)
{
    int ret;

    void *buffer = mmap(NULL,
                        UMEM_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                        -1,
                        0);

    if (buffer == MAP_FAILED)
    {
        perror("mmap failed");
        return -1;
    }

    struct xsk_umem_config cfg = {
        .fill_size = RX_RING_SIZE,
        .comp_size = TX_RING_SIZE,
        .frame_size = FRAME_SIZE,
        .frame_headroom = 0,
        .flags = 0};

    ret = xsk_umem__create(&u->umem,
                           buffer,
                           UMEM_SIZE,
                           &u->fill_q,
                           &u->comp_q,
                           &cfg);

    if (ret)
    {
        fprintf(stderr, "xsk_umem__create failed\n");
        return ret;
    }

    u->buffer = buffer;

    uint32_t idx;
    ret = xsk_ring_prod__reserve(&u->fill_q, FRAME_COUNT, &idx);

    if (ret != FRAME_COUNT)
    {
        fprintf(stderr, "fill ring reserve failed\n");
        return -1;
    }

    for (int i = 0; i < FRAME_COUNT; i++)
    {
        *xsk_ring_prod__fill_addr(&u->fill_q, idx++) = i * FRAME_SIZE;
    }

    xsk_ring_prod__submit(&u->fill_q, FRAME_COUNT);

    return 0;
}

void umem_free(struct umem_info *u)
{
    if (!u)
        return;

    if (u->umem)
        xsk_umem__delete(u->umem);

    if (u->buffer)
        munmap(u->buffer, UMEM_SIZE);
}