#include "../include/packet_parser.h"

#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <arpa/inet.h>
#include <stddef.h>

int parse_packet(const uint8_t *pkt, uint32_t len, struct parsed_packet *out)
{
    if (len < sizeof(struct ethhdr))
        return -1;

    const struct ethhdr *eth = (const struct ethhdr *)pkt;

    if (ntohs(eth->h_proto) != ETH_P_IP)
        return -1;

    if (len < sizeof(struct ethhdr) + sizeof(struct iphdr))
        return -1;

    const struct iphdr *ip = (const struct iphdr *)(pkt + sizeof(struct ethhdr));
    uint32_t ip_hdr_len = ip->ihl * 4;

    out->src_ip = ip->saddr;
    out->dst_ip = ip->daddr;

    if (ip->protocol == IPPROTO_UDP)
    {
        if (len < sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct udphdr))
            return -1;

        const struct udphdr *udp = (const struct udphdr *)(pkt + sizeof(struct ethhdr) + ip_hdr_len);

        out->protocol = PKT_PROTO_UDP;
        out->src_port = ntohs(udp->source);
        out->dst_port = ntohs(udp->dest);
        out->tcp_flags = 0;

        uint32_t hdr_total = sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct udphdr);
        out->payload = (uint8_t *)(pkt + hdr_total);
        out->payload_len = len - hdr_total;

        return 0;
    }

    if (ip->protocol == IPPROTO_TCP)
    {
        if (len < sizeof(struct ethhdr) + ip_hdr_len + sizeof(struct tcphdr))
            return -1;

        const struct tcphdr *tcp = (const struct tcphdr *)(pkt + sizeof(struct ethhdr) + ip_hdr_len);
        uint32_t tcp_hdr_len = tcp->doff * 4;

        out->protocol = PKT_PROTO_TCP;
        out->src_port = ntohs(tcp->source);
        out->dst_port = ntohs(tcp->dest);
        out->tcp_flags = ((const uint8_t *)tcp)[13];
        out->tcp_seq = ntohl(tcp->seq);
        out->tcp_ack_seq = ntohl(tcp->ack_seq);

        uint32_t hdr_total = sizeof(struct ethhdr) + ip_hdr_len + tcp_hdr_len;
        if (len >= hdr_total)
        {
            out->payload = (uint8_t *)(pkt + hdr_total);
            out->payload_len = len - hdr_total;
        }
        else
        {
            out->payload = NULL;
            out->payload_len = 0;
        }

        return 0;
    }

    out->protocol = PKT_PROTO_OTHER;
    out->src_port = 0;
    out->dst_port = 0;
    out->tcp_flags = 0;
    out->payload = NULL;
    out->payload_len = 0;

    return 0;
}
