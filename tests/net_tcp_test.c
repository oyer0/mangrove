#include <assert.h>
#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long usize;
typedef _Bool bool;
typedef signed int i32;
typedef struct { u8 octet[4]; } net_ipv4_t;
typedef struct {
    u16 source_port, destination_port;
    u32 sequence, acknowledgment;
    u8 flags;
    u16 window;
    const u8 *payload;
    usize payload_length;
} tcp_segment_t;

bool tcp_seq_before(u32, u32);
bool tcp_seq_between(u32, u32, u32);
u16 tcp_checksum(net_ipv4_t, net_ipv4_t, const void *, usize);
bool tcp_parse_segment(const u8 *, usize, tcp_segment_t *);

static void be16(u8 *p, u16 value) { p[0] = (u8)(value >> 8); p[1] = (u8)value; }
static void be32(u8 *p, u32 value) {
    p[0] = (u8)(value >> 24); p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8); p[3] = (u8)value;
}

int main(void)
{
    u8 packet[25] = {0};
    net_ipv4_t source = {{10, 0, 2, 15}};
    net_ipv4_t destination = {{10, 0, 2, 2}};
    tcp_segment_t segment;
    u16 sum;

    be16(packet, 49152); be16(packet + 2, 12345);
    be32(packet + 4, 0xfffffff0U); be32(packet + 8, 23);
    packet[12] = 5U << 4; packet[13] = 0x18; be16(packet + 14, 4096);
    packet[20] = 'o'; packet[21] = 'd'; packet[22] = 'd'; packet[23] = '!'; packet[24] = '\n';
    sum = tcp_checksum(source, destination, packet, sizeof(packet));
    assert(sum != 0); be16(packet + 16, sum);
    assert(tcp_checksum(source, destination, packet, sizeof(packet)) == sum);
    assert(tcp_parse_segment(packet, sizeof(packet), &segment));
    assert(segment.source_port == 49152 && segment.destination_port == 12345);
    assert(segment.payload_length == 5 && segment.payload[0] == 'o');
    assert(tcp_seq_before(0xfffffff0U, 0x10U));
    assert(tcp_seq_between(0U, 0xfffffff0U, 0x10U));
    packet[12] = 4U << 4;
    assert(!tcp_parse_segment(packet, sizeof(packet), &segment));
    puts("TCP wire tests passed");
    return 0;
}
