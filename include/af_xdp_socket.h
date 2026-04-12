#ifndef AF_XDP_SOCKET_H
#define AF_XDP_SOCKET_H

#include <xdp/xsk.h>
#include "umem.h"

struct xsk_socket_info
{
    struct xsk_socket *xsk;

    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;

    struct umem_info *umem;

    uint32_t outstanding_tx;
};

int xsk_socket_create(struct xsk_socket_info **xsk_ptr,
                      struct umem_info *umem,
                      const char *ifname,
                      uint32_t queue_id);

void xsk_socket_delete(struct xsk_socket_info *xsk);

#endif