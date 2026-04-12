#include <stdio.h>
#include "../include/umem.h"
#include "../include/af_xdp_socket.h"

int main()
{
    struct umem_info umem;
    struct xsk_socket_info *xsk;

    const char *ifname = "wlo1";
    uint32_t queue_id = 0;

    if (umem_create(&umem))
    {
        printf("UMEM initialization failed\n");
        return 1;
    }

    printf("UMEM initialized\n");
    printf("frame size: %d\n", FRAME_SIZE);
    printf("frame count: %d\n", FRAME_COUNT);

    printf("fill ring populated\n");

    if (xsk_socket_create(&xsk, &umem, ifname, queue_id))
    {
        printf("Socket creation failed\n");
        return 1;
    }

    printf("AF_XDP socket created\n");

    while (1)
    {
        uint32_t idx;
        int rcvd = xsk_ring_cons__peek(&xsk->rx, 64, &idx);

        if (!rcvd)
            continue;

        for (int i = 0; i < rcvd; i++)
        {
            const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&xsk->rx, idx + i);

            printf("packet len: %u\n", desc->len);

            // recycle buffer
            uint32_t fidx;
            xsk_ring_prod__reserve(&umem.fill_q, 1, &fidx);
            *xsk_ring_prod__fill_addr(&umem.fill_q, fidx) = desc->addr;
            xsk_ring_prod__submit(&umem.fill_q, 1);
        }

        xsk_ring_cons__release(&xsk->rx, rcvd);
    }

    xsk_socket_delete(xsk);
    umem_free(&umem);

    return 0;
}