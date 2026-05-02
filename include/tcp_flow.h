#ifndef TCP_FLOW_H
#define TCP_FLOW_H

#include <stdint.h>
#include "packet_parser.h"

/*
 * Maximum tracked flows. Fixed-size table avoids dynamic allocation
 * in the hot path. Future work: replace with a proper hashmap for
 * scalable flow tracking.
 */
#define MAX_TCP_FLOWS 1024

/* Lightweight connection state (no full state machine) */
enum tcp_conn_state
{
    TCP_STATE_NONE,
    TCP_STATE_SYN_SEEN,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_SEEN,
    TCP_STATE_CLOSED,
};

/* TCP event types returned by flow tracking */
enum tcp_event
{
    TCP_EVENT_NONE,
    TCP_EVENT_NEW_CONN,    /* SYN seen → new flow */
    TCP_EVENT_ESTABLISHED, /* ACK after SYN → connection up */
    TCP_EVENT_DATA,        /* normal ACK traffic */
    TCP_EVENT_FIN,         /* FIN seen → connection closing */
    TCP_EVENT_RST,         /* RST seen → connection reset */
};

struct tcp_flow_key
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
};

struct tcp_flow_entry
{
    struct tcp_flow_key key;
    uint64_t pkt_count;
    uint8_t active;
    enum tcp_conn_state state;
    uint32_t last_seq; /* last sequence number seen */
};

struct tcp_flow_table
{
    struct tcp_flow_entry flows[MAX_TCP_FLOWS];
    uint32_t count;     /* number of active flows */
    uint64_t syn_count; /* total SYN packets seen */
    uint64_t fin_count; /* total FIN packets seen */
    uint64_t rst_count; /* total RST packets seen */
};

static inline void tcp_flow_table_init(struct tcp_flow_table *t)
{
    t->count = 0;
    t->syn_count = 0;
    t->fin_count = 0;
    t->rst_count = 0;
    for (uint32_t i = 0; i < MAX_TCP_FLOWS; i++)
        t->flows[i].active = 0;
}

/*
 * Find an existing flow by 4-tuple. Returns entry or NULL.
 * Uses modulo-indexed start for slight distribution, then linear probe.
 */
static inline struct tcp_flow_entry *tcp_flow_find(
    struct tcp_flow_table *t,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port)
{
    uint32_t start = (src_ip ^ dst_ip ^ ((uint32_t)src_port << 16 | dst_port)) & (MAX_TCP_FLOWS - 1);

    for (uint32_t n = 0; n < MAX_TCP_FLOWS; n++)
    {
        uint32_t i = (start + n) & (MAX_TCP_FLOWS - 1);
        if (!t->flows[i].active)
            continue;

        struct tcp_flow_key *k = &t->flows[i].key;
        if (k->src_ip == src_ip && k->dst_ip == dst_ip &&
            k->src_port == src_port && k->dst_port == dst_port)
            return &t->flows[i];
    }
    return NULL;
}

/*
 * Insert a new flow. Returns entry or NULL if table full.
 */
static inline struct tcp_flow_entry *tcp_flow_insert(
    struct tcp_flow_table *t,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port)
{
    if (t->count >= MAX_TCP_FLOWS)
        return NULL;

    uint32_t start = (src_ip ^ dst_ip ^ ((uint32_t)src_port << 16 | dst_port)) & (MAX_TCP_FLOWS - 1);

    for (uint32_t n = 0; n < MAX_TCP_FLOWS; n++)
    {
        uint32_t i = (start + n) & (MAX_TCP_FLOWS - 1);
        if (!t->flows[i].active)
        {
            t->flows[i].active = 1;
            t->flows[i].key.src_ip = src_ip;
            t->flows[i].key.dst_ip = dst_ip;
            t->flows[i].key.src_port = src_port;
            t->flows[i].key.dst_port = dst_port;
            t->flows[i].pkt_count = 1;
            t->flows[i].state = TCP_STATE_NONE;
            t->flows[i].last_seq = 0;
            t->count++;
            return &t->flows[i];
        }
    }
    return NULL;
}

/*
 * Process a TCP packet and detect events.
 * Performs lightweight state tracking based on flags.
 * No retransmission logic, no full state machine.
 */
static inline enum tcp_event tcp_flow_process(
    struct tcp_flow_table *t,
    const struct parsed_packet *pkt)
{
    uint8_t flags = pkt->tcp_flags;
    uint32_t src_ip = pkt->src_ip;
    uint32_t dst_ip = pkt->dst_ip;
    uint16_t src_port = pkt->src_port;
    uint16_t dst_port = pkt->dst_port;

    /* RST immediately marks closed */
    if (flags & PKT_TCP_FLAG_RST)
    {
        t->rst_count++;
        struct tcp_flow_entry *e = tcp_flow_find(t, src_ip, dst_ip, src_port, dst_port);
        if (e)
        {
            e->state = TCP_STATE_CLOSED;
            e->pkt_count++;
        }
        return TCP_EVENT_RST;
    }

    /* SYN (without ACK) → new connection */
    if ((flags & PKT_TCP_FLAG_SYN) && !(flags & PKT_TCP_FLAG_ACK))
    {
        t->syn_count++;
        struct tcp_flow_entry *e = tcp_flow_find(t, src_ip, dst_ip, src_port, dst_port);
        if (!e)
            e = tcp_flow_insert(t, src_ip, dst_ip, src_port, dst_port);
        if (e)
        {
            e->state = TCP_STATE_SYN_SEEN;
            e->last_seq = pkt->tcp_seq;
        }
        return TCP_EVENT_NEW_CONN;
    }

    /* FIN → connection closing */
    if (flags & PKT_TCP_FLAG_FIN)
    {
        t->fin_count++;
        struct tcp_flow_entry *e = tcp_flow_find(t, src_ip, dst_ip, src_port, dst_port);
        if (e)
        {
            e->state = TCP_STATE_FIN_SEEN;
            e->pkt_count++;
            e->last_seq = pkt->tcp_seq;
        }
        return TCP_EVENT_FIN;
    }

    /* ACK → established or data */
    if (flags & PKT_TCP_FLAG_ACK)
    {
        struct tcp_flow_entry *e = tcp_flow_find(t, src_ip, dst_ip, src_port, dst_port);
        if (!e)
        {
            /* Mid-stream packet, track it anyway */
            e = tcp_flow_insert(t, src_ip, dst_ip, src_port, dst_port);
            if (e)
                e->state = TCP_STATE_ESTABLISHED;
            return TCP_EVENT_DATA;
        }

        e->pkt_count++;
        e->last_seq = pkt->tcp_seq;

        if (e->state == TCP_STATE_SYN_SEEN)
        {
            e->state = TCP_STATE_ESTABLISHED;
            return TCP_EVENT_ESTABLISHED;
        }

        return TCP_EVENT_DATA;
    }

    return TCP_EVENT_NONE;
}

/* Legacy API — simple track without event detection */
static inline struct tcp_flow_entry *tcp_flow_track(
    struct tcp_flow_table *t,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port)
{
    struct tcp_flow_entry *e = tcp_flow_find(t, src_ip, dst_ip, src_port, dst_port);
    if (e)
    {
        e->pkt_count++;
        return e;
    }
    return tcp_flow_insert(t, src_ip, dst_ip, src_port, dst_port);
}

#endif
