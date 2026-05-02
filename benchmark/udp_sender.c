#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PKT_SIZE 64
#define DEFAULT_PORT 9000
#define DEFAULT_TARGET "127.0.0.1"

#define RATE_LIMIT_PPS 0

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [target_ip] [port] [pkt_size]\n", prog);
    fprintf(stderr, "  target_ip  default: %s\n", DEFAULT_TARGET);
    fprintf(stderr, "  port       default: %d\n", DEFAULT_PORT);
    fprintf(stderr, "  pkt_size   default: %d bytes\n", DEFAULT_PKT_SIZE);
}

int main(int argc, char *argv[])
{
    const char *target = (argc > 1) ? argv[1] : DEFAULT_TARGET;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
    int pkt_size = (argc > 3) ? atoi(argv[3]) : DEFAULT_PKT_SIZE;

    if (pkt_size < 8 || pkt_size > 65507)
    {
        fprintf(stderr, "packet size must be between 8 and 65507\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };

    if (inet_pton(AF_INET, target, &dest.sin_addr) != 1)
    {
        fprintf(stderr, "invalid target IP: %s\n", target);
        close(fd);
        return 1;
    }

    char *buf = calloc(1, pkt_size);
    if (!buf)
    {
        perror("calloc");
        close(fd);
        return 1;
    }

    printf("sending %d-byte UDP packets to %s:%d\n", pkt_size, target, port);
    if (RATE_LIMIT_PPS > 0)
        printf("rate limit: %d PPS\n", RATE_LIMIT_PPS);
    else
        printf("rate limit: none (max throughput)\n");

    uint64_t pkt_count = 0;
    uint64_t last_pkt_count = 0;

    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

#if RATE_LIMIT_PPS > 0
    const long interval_ns = 1000000000L / RATE_LIMIT_PPS;
    struct timespec ts_last_send;
    clock_gettime(CLOCK_MONOTONIC, &ts_last_send);
#endif

    while (1)
    {
#if RATE_LIMIT_PPS > 0
        struct timespec ts_cur;
        clock_gettime(CLOCK_MONOTONIC, &ts_cur);
        long diff_ns = (ts_cur.tv_sec - ts_last_send.tv_sec) * 1000000000L +
                       (ts_cur.tv_nsec - ts_last_send.tv_nsec);
        if (diff_ns < interval_ns)
            continue;
        ts_last_send = ts_cur;
#endif

        /* Embed send timestamp (ns) in first 8 bytes of payload */
        struct timespec ts_send;
        clock_gettime(CLOCK_MONOTONIC, &ts_send);
        uint64_t send_ns = (uint64_t)ts_send.tv_sec * 1000000000ULL + ts_send.tv_nsec;
        memcpy(buf, &send_ns, sizeof(send_ns));

        ssize_t n = sendto(fd, buf, pkt_size, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
        if (n < 0)
            continue;

        pkt_count++;

        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        double elapsed = (ts_now.tv_sec - ts_start.tv_sec) +
                         (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
        if (elapsed >= 1.0)
        {
            uint64_t delta = pkt_count - last_pkt_count;
            printf("PPS: %.0f | total: %lu\n", delta / elapsed, pkt_count);
            last_pkt_count = pkt_count;
            ts_start = ts_now;
        }
    }

    free(buf);
    close(fd);
    return 0;
}
