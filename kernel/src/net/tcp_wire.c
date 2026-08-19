#include <net/checksum.h>
#include <net/ethernet.h>
#include <net/tcp.h>

static u16 tcp_read16(const u8 *p)
{
    return ((u16)p[0] << 8) | p[1];
}

static u32 tcp_read32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

bool tcp_seq_before(u32 left, u32 right)
{
    return (i32)(left - right) < 0;
}

bool tcp_seq_between(u32 value, u32 first, u32 last)
{
    return !tcp_seq_before(value, first) && !tcp_seq_before(last, value);
}

u16 tcp_checksum(net_ipv4_t source, net_ipv4_t destination,
                 const void *segment, usize length)
{
    u8 pseudo[12 + ETHERNET_PAYLOAD_MAX];
    const u8 *input = (const u8 *)segment;
    if (!segment || length < TCP_HEADER_MIN_SIZE || length > ETHERNET_PAYLOAD_MAX)
        return 0;
    for (u32 i = 0; i < 4; i++) pseudo[i] = source.octet[i];
    for (u32 i = 0; i < 4; i++) pseudo[4 + i] = destination.octet[i];
    pseudo[8] = 0;
    pseudo[9] = TCP_PROTOCOL;
    pseudo[10] = (u8)(length >> 8);
    pseudo[11] = (u8)length;
    for (usize i = 0; i < length; i++) pseudo[12 + i] = input[i];
    pseudo[12 + 16] = 0;
    pseudo[12 + 17] = 0;
    return net_checksum(pseudo, 12 + length);
}

bool tcp_parse_segment(const u8 *packet, usize length, tcp_segment_t *segment)
{
    usize header_length;
    if (!packet || !segment || length < TCP_HEADER_MIN_SIZE) return false;
    header_length = (usize)(packet[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_MIN_SIZE || header_length > length) return false;
    segment->source_port = tcp_read16(packet + 0);
    segment->destination_port = tcp_read16(packet + 2);
    segment->sequence = tcp_read32(packet + 4);
    segment->acknowledgment = tcp_read32(packet + 8);
    segment->flags = packet[13];
    segment->window = tcp_read16(packet + 14);
    segment->payload = packet + header_length;
    segment->payload_length = length - header_length;
    return true;
}
