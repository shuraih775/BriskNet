#include "../include/af_xdp_socket.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <net/if.h>

int xsk_socket_create(struct xsk_socket_info **xsk_ptr,
                      struct umem_info *umem,
                      const char *ifname,
                      uint32_t queue_id)
{
    struct xsk_socket_config cfg;
    struct xsk_socket_info *xsk;
    int ifindex;
    int ret;

    xsk = calloc(1, sizeof(*xsk));
    if (!xsk)
        return -1;

    xsk->umem = umem;

    ifindex = if_nametoindex(ifname);
    if (!ifindex)
    {
        perror("if_nametoindex failed");
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_size = RX_RING_SIZE;
    cfg.tx_size = TX_RING_SIZE;
    cfg.libbpf_flags = 0;
    cfg.xdp_flags = (1U << 1);
    cfg.bind_flags = XDP_USE_NEED_WAKEUP;

    ret = xsk_socket__create(&xsk->xsk,
                             ifname,
                             queue_id,
                             umem->umem,
                             &xsk->rx,
                             &xsk->tx,
                             &cfg);

    if (ret)
    {
        fprintf(stderr, "xsk_socket__create failed: %s\n",
                strerror(-ret));
        free(xsk);
        return ret;
    }

    xsk->outstanding_tx = 0;

    *xsk_ptr = xsk;

    int fd = xsk_socket__fd(xsk->xsk);

    int map_fd = bpf_obj_get("/sys/fs/bpf/xsks_map"); // or from skeleton later

    int key = queue_id;

    if (bpf_map_update_elem(map_fd, &key, &fd, 0) < 0)
    {
        perror("bpf_map_update_elem failed");
    }

    return 0;
}

void xsk_socket_delete(struct xsk_socket_info *xsk)
{
    if (!xsk)
        return;

    if (xsk->xsk)
        xsk_socket__delete(xsk->xsk);

    free(xsk);
}