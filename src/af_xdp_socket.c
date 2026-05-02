#include "../include/af_xdp_socket.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <linux/if_link.h>
#include <net/if.h>

#ifndef DEBUG
#define DEBUG 0
#endif

#define DBG_PRINT(fmt, ...)                               \
    do                                                    \
    {                                                     \
        if (DEBUG)                                        \
            fprintf(stderr, "[DBG] " fmt, ##__VA_ARGS__); \
    } while (0)

int xsk_socket_create(struct xsk_socket_info **xsk_ptr,
                      struct umem_info *umem,
                      const char *ifname,
                      uint32_t queue_id)
{
    struct xsk_socket_config cfg;
    struct xsk_socket_info *xsk;
    int ret;

    xsk = calloc(1, sizeof(*xsk));
    if (!xsk)
        return -1;

    xsk->umem = umem;

    if (!if_nametoindex(ifname))
    {
        perror("if_nametoindex failed");
        free(xsk);
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_size = RX_RING_SIZE;
    cfg.tx_size = TX_RING_SIZE;
    cfg.libbpf_flags = 0;
    cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
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
    xsk->queue_id = queue_id;

    *xsk_ptr = xsk;

    int fd = xsk_socket__fd(xsk->xsk);
    int map_fd = bpf_obj_get("/sys/fs/bpf/xsks_map");

    if (map_fd < 0)
    {
        fprintf(stderr, "setting the map failed: %s\n", strerror(errno));
    }

    DBG_PRINT("queue_id: %u\n", queue_id);
    DBG_PRINT("socket fd: %d\n", fd);
    DBG_PRINT("map_fd: %d\n", map_fd);

    if (bpf_map_update_elem(map_fd, &queue_id, &fd, 0) < 0)
    {
        fprintf(stderr, "bpf_map_update_elem failed: %s\n", strerror(errno));
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