#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define BIND_PORT 9000
#define BUF_SIZE  2048

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(BIND_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("listening on UDP port %d\n", BIND_PORT);

    char buf[BUF_SIZE];
    uint64_t pkt_count = 0;
    uint64_t last_pkt_count = 0;

    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (1)
    {
        ssize_t n = recvfrom(fd, buf, BUF_SIZE, 0, NULL, NULL);
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

    close(fd);
    return 0;
}
