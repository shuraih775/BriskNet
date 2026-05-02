#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#define PKT_TCP_FLAG_FIN 0x01
#define PKT_TCP_FLAG_SYN 0x02
#define PKT_TCP_FLAG_RST 0x04
#define PKT_TCP_FLAG_PSH 0x08
#define PKT_TCP_FLAG_ACK 0x10
#define PKT_TCP_FLAG_URG 0x20

#include <stdint.h>

/* TCP flag bits */

enum pkt_protocol
{
    PKT_PROTO_UDP,
    PKT_PROTO_TCP,
    PKT_PROTO_OTHER,
};

struct parsed_packet
{
    enum pkt_protocol protocol;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t tcp_flags;    /* raw TCP flags byte (only valid for TCP) */
    uint32_t tcp_seq;     /* TCP sequence number (host byte order) */
    uint32_t tcp_ack_seq; /* TCP acknowledgment number (host byte order) */
    uint8_t *payload;
    uint32_t payload_len;
};

/*
 * Parse Ethernet/IPv4/UDP or TCP headers from raw packet data.
 * Returns 0 on success (UDP or TCP parsed), -1 otherwise.
 * On success, fields in `out` are populated.
 */
int parse_packet(const uint8_t *pkt, uint32_t len, struct parsed_packet *out);

#endif
