#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include "../include/umem.h"
#include "../include/af_xdp_socket.h"
#include "../include/packet_parser.h"
#include "../include/tcp_flow.h"
#include "../include/lfqueue_wrapper.h"

#define POLLER_CPU_CORE 2
#define WORKER_CPU_CORE 3
#define BATCH_SIZE 64
#define LATENCY_RING_SIZE (1 << 20) /* 1M entries */
#define LATENCY_RING_MASK (LATENCY_RING_SIZE - 1)
#define QUEUE_SIZE 8192
#define RESULTS_FILE "benchmark/results.txt"

/* Pack addr (upper 32) and len (lower 32) into a single uint64_t */
#define PKT_ENCODE(addr, len) (((uint64_t)(addr) << 32) | (uint32_t)(len))
#define PKT_ADDR(val) ((val) >> 32)
#define PKT_LEN(val) ((uint32_t)((val) & 0xFFFFFFFF))

#define NUM_QUEUES 1

static volatile int running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

/* Shared state between poller and worker */
struct pipeline_ctx
{
    struct umem_info *umem;
    struct xsk_socket_info *xsk;
    lfqueue_t *queue;
    _Alignas(64) atomic_uint_least64_t packets_received;
    _Alignas(64) atomic_uint_least64_t packets_enqueued;
    _Alignas(64) atomic_uint_least64_t packets_dropped;
    _Alignas(64) atomic_uint_least64_t packets_processed;
};

static int pin_thread(int cpu)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static void *poller_thread(void *arg)
{
    struct pipeline_ctx *ctx = (struct pipeline_ctx *)arg;
    struct xsk_socket_info *xsk = ctx->xsk;

    if (pin_thread(POLLER_CPU_CORE) == 0)
        printf("[poller] pinned to CPU %d\n", POLLER_CPU_CORE);
    else
        perror("[poller] sched_setaffinity failed");

    while (running)
    {
        uint32_t rx_idx;
        int rcvd = xsk_ring_cons__peek(&xsk->rx, BATCH_SIZE, &rx_idx);

        if (!rcvd)
            continue;

        for (int i = 0; i < rcvd; i++)
        {
            const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&xsk->rx, rx_idx + i);

            /* prefetch next descriptor + its UMEM packet data */
            if (i + 1 < rcvd)
            {
                const struct xdp_desc *next = xsk_ring_cons__rx_desc(&xsk->rx, rx_idx + i + 1);
                __builtin_prefetch(next, 0, 1);
                __builtin_prefetch((uint8_t *)ctx->umem->buffer + next->addr, 0, 3);
            }

            /* Enqueue addr+len to worker; drop if full (poller must never block) */
            if (lfqueue_enqueue(ctx->queue, PKT_ENCODE(desc->addr, desc->len)) != 0)
                atomic_fetch_add_explicit(&ctx->packets_dropped, 1, memory_order_relaxed);
            else
                atomic_fetch_add_explicit(&ctx->packets_enqueued, 1, memory_order_relaxed);
        }

        /* Flush to ensure worker sees enqueued items promptly */
        lfqueue_flush(ctx->queue);

        xsk_ring_cons__release(&xsk->rx, rcvd);
        atomic_fetch_add_explicit(&ctx->packets_received, rcvd, memory_order_relaxed);
    }

    /* Final flush on exit */
    lfqueue_flush(ctx->queue);
    return NULL;
}

static uint64_t latency_ring[LATENCY_RING_SIZE];
static uint64_t latency_scratch[LATENCY_RING_SIZE];

static inline void swap64(uint64_t *a, uint64_t *b)
{
    uint64_t t = *a;
    *a = *b;
    *b = t;
}

static size_t partition(uint64_t *arr, size_t lo, size_t hi)
{
    uint64_t pivot = arr[hi];
    size_t i = lo;
    for (size_t j = lo; j < hi; j++)
    {
        if (arr[j] <= pivot)
        {
            swap64(&arr[i], &arr[j]);
            i++;
        }
    }
    swap64(&arr[i], &arr[hi]);
    return i;
}

static uint64_t quickselect(uint64_t *arr, size_t lo, size_t hi, size_t k)
{
    while (lo < hi)
    {
        size_t p = partition(arr, lo, hi);
        if (p == k)
            return arr[p];
        else if (k < p)
            hi = p - 1;
        else
            lo = p + 1;
    }
    return arr[lo];
}

static uint64_t latency_percentile(size_t count, double pct)
{
    if (count == 0)
        return 0;
    size_t n = count < LATENCY_RING_SIZE ? count : LATENCY_RING_SIZE;
    memcpy(latency_scratch, latency_ring, n * sizeof(uint64_t));
    size_t k = (size_t)(pct * (n - 1));
    if (k >= n)
        k = n - 1;
    return quickselect(latency_scratch, 0, n - 1, k);
}

static void *worker_thread(void *arg)
{
    struct pipeline_ctx *ctx = (struct pipeline_ctx *)arg;
    struct umem_info *umem = ctx->umem;

    if (pin_thread(WORKER_CPU_CORE) == 0)
        printf("[worker] pinned to CPU %d\n", WORKER_CPU_CORE);
    else
        perror("[worker] sched_setaffinity failed");

    uint64_t pkt_count = 0;
    uint64_t udp_count = 0;
    uint64_t last_pkt_count = 0;

    uint64_t latency_sum_ns = 0;
    uint64_t latency_max_ns = 0;
    uint64_t latency_samples = 0;
    size_t latency_ring_idx = 0;
    uint64_t elapsed_sec = 0;

    struct tcp_flow_table tcp_flows;
    tcp_flow_table_init(&tcp_flows);

    FILE *results_fp = fopen(RESULTS_FILE, "w");
    if (results_fp)
    {
        fprintf(results_fp, "sec,pps,total,udp,latency_avg_ns,latency_max_ns,p50_ns,p99_ns,p999_ns\n");
        fflush(results_fp);
    }

    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (running)
    {
        uint64_t batch[BATCH_SIZE];
        size_t n = lfqueue_dequeue_bulk(ctx->queue, batch, BATCH_SIZE);
        if (!n)
            continue;

        uint64_t recycle_addrs[BATCH_SIZE];
        size_t recycle_count = 0;

        for (size_t bi = 0; bi < n; bi++)
        {
            uint64_t val = batch[bi];
            uint64_t addr = PKT_ADDR(val);
            uint32_t len = PKT_LEN(val);
            uint8_t *pkt = (uint8_t *)umem->buffer + addr;

            /* prefetch next packet's UMEM buffer 1 step ahead */
            if (bi + 1 < n)
                __builtin_prefetch((uint8_t *)umem->buffer + PKT_ADDR(batch[bi + 1]), 0, 3);

            struct parsed_packet parsed;
            if (parse_packet(pkt, len, &parsed) != 0)
                goto recycle;

            /* Latency measurement (UDP only) */
            if (parsed.protocol == PKT_PROTO_UDP &&
                parsed.payload_len >= sizeof(uint64_t))
            {
                uint64_t send_ns;
                memcpy(&send_ns, parsed.payload, sizeof(send_ns));

                struct timespec ts_rx;
                clock_gettime(CLOCK_MONOTONIC, &ts_rx);
                uint64_t rx_ns = (uint64_t)ts_rx.tv_sec * 1000000000ULL + ts_rx.tv_nsec;

                if (send_ns == 0 || send_ns > rx_ns)
                {
                    goto recycle; // skip invalid sample
                }
                uint64_t latency = rx_ns - send_ns;

                latency_ring[latency_ring_idx & LATENCY_RING_MASK] = latency;
                latency_ring_idx++;
                latency_sum_ns += latency;
                if (latency > latency_max_ns)
                    latency_max_ns = latency;
                latency_samples++;
            }

            if (parsed.protocol == PKT_PROTO_UDP)
                udp_count++;

            if (parsed.protocol == PKT_PROTO_TCP)
                tcp_flow_process(&tcp_flows, &parsed);

        recycle:
            recycle_addrs[recycle_count++] = addr;
            pkt_count++;
        }

        /* Batch-recycle all buffers back to fill ring */
        {
            uint32_t fill_idx;
            if (xsk_ring_prod__reserve(&umem->fill_q, recycle_count, &fill_idx) == recycle_count)
            {
                for (size_t ri = 0; ri < recycle_count; ri++)
                    *xsk_ring_prod__fill_addr(&umem->fill_q, fill_idx + ri) = recycle_addrs[ri];
                xsk_ring_prod__submit(&umem->fill_q, recycle_count);
            }
        }

        atomic_fetch_add_explicit(&ctx->packets_processed, recycle_count, memory_order_relaxed);

        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        double elapsed = (ts_now.tv_sec - ts_start.tv_sec) +
                         (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
        if (elapsed >= 1.0)
        {
            elapsed_sec++;
            uint64_t delta = pkt_count - last_pkt_count;
            uint64_t avg_ns = latency_samples ? latency_sum_ns / latency_samples : 0;
            uint64_t p50 = latency_percentile(latency_samples, 0.50);
            uint64_t p99 = latency_percentile(latency_samples, 0.99);
            uint64_t p999 = latency_percentile(latency_samples, 0.999);
            uint64_t rcvd = atomic_load_explicit(&ctx->packets_received, memory_order_relaxed);
            uint64_t enqd = atomic_load_explicit(&ctx->packets_enqueued, memory_order_relaxed);
            uint64_t drops = atomic_load_explicit(&ctx->packets_dropped, memory_order_relaxed);
            size_t queue_depth = lfqueue_size(ctx->queue);
            printf("PPS: %.0f | rcvd: %lu enqd: %lu proc: %lu | drops: %lu | queue_depth: %zu\n",
                   delta / elapsed, rcvd, enqd, pkt_count, drops, queue_depth);
            printf("  udp: %lu | latency avg: %lu ns, p50: %lu ns, p99: %lu ns, p999: %lu ns, max: %lu ns\n",
                   udp_count, avg_ns, p50, p99, p999, latency_max_ns);
            if (results_fp)
            {
                fprintf(results_fp, "%lu,%.0f,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                        elapsed_sec, delta / elapsed, pkt_count, udp_count,
                        avg_ns, latency_max_ns, p50, p99, p999);
                fflush(results_fp);
            }
            last_pkt_count = pkt_count;
            ts_start = ts_now;
            latency_sum_ns = 0;
            latency_max_ns = 0;
            latency_samples = 0;
            latency_ring_idx = 0;
        }
    }

    printf("\nshutting down...\n"
           "  packets_received:  %lu\n"
           "  packets_enqueued:  %lu\n"
           "  packets_dropped:   %lu\n"
           "  packets_processed: %lu\n"
           "  udp: %lu | tcp flows: %u (syn: %lu, fin: %lu, rst: %lu)\n",
           atomic_load_explicit(&ctx->packets_received, memory_order_relaxed),
           atomic_load_explicit(&ctx->packets_enqueued, memory_order_relaxed),
           atomic_load_explicit(&ctx->packets_dropped, memory_order_relaxed),
           atomic_load_explicit(&ctx->packets_processed, memory_order_relaxed),
           udp_count, tcp_flows.count,
           tcp_flows.syn_count, tcp_flows.fin_count, tcp_flows.rst_count);

    if (results_fp)
    {
        fprintf(results_fp, "# summary: total=%lu udp=%lu duration=%lu s\n",
                pkt_count, udp_count, elapsed_sec);
        fclose(results_fp);
    }
    printf("results saved to %s\n", RESULTS_FILE);

    return NULL;
}

int main()
{
    struct umem_info umem;
    struct xsk_socket_info *xsk;

    const char *ifname = "lo";
    uint32_t queue_id = 0;

    if (umem_create(&umem))
    {
        fprintf(stderr, "UMEM initialization failed\n");
        return 1;
    }

    printf("UMEM initialized\n");
    printf("frame size: %d\n", FRAME_SIZE);
    printf("frame count: %d\n", FRAME_COUNT);
    printf("fill ring populated\n");

    if (xsk_socket_create(&xsk, &umem, ifname, queue_id))
    {
        fprintf(stderr, "Socket creation failed\n");
        return 1;
    }

    printf("AF_XDP socket created\n");

    signal(SIGINT, handle_sigint);

    struct pipeline_ctx ctx = {
        .umem = &umem,
        .xsk = xsk,
        .queue = lfqueue_create(QUEUE_SIZE),
    };
    if (!ctx.queue)
    {
        fprintf(stderr, "lfqueue_create failed\n");
        return 1;
    }
    atomic_store_explicit(&ctx.packets_received, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx.packets_enqueued, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx.packets_dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx.packets_processed, 0, memory_order_relaxed);

    pthread_t poller_tid, worker_tid;

    if (pthread_create(&worker_tid, NULL, worker_thread, &ctx) != 0)
    {
        perror("pthread_create worker");
        return 1;
    }

    if (pthread_create(&poller_tid, NULL, poller_thread, &ctx) != 0)
    {
        perror("pthread_create poller");
        return 1;
    }

    pthread_join(poller_tid, NULL);
    pthread_join(worker_tid, NULL);

    lfqueue_destroy(ctx.queue);
    xsk_socket_delete(xsk);
    umem_free(&umem);

    return 0;
}