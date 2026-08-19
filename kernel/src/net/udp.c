#include <net/checksum.h>
#include <net/ipv4.h>
#include <net/udp.h>

typedef struct __attribute__((packed)) {
    u16 source_port;
    u16 destination_port;
    u16 length;
    u16 checksum;
} udp_wire_header_t;

_Static_assert(sizeof(udp_wire_header_t) == UDP_HEADER_SIZE,
               "UDP header must be eight bytes");

typedef struct { u16 port; udp_handler_t handler; } udp_endpoint_t;
static udp_endpoint_t endpoints[8];

static u16 read_be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static void write_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static void ip_copy(u8 out[4], net_ipv4_t ip)
{
    for (u32 i = 0; i < 4; i++) out[i] = ip.octet[i];
}

u16 udp_checksum(net_ipv4_t source, net_ipv4_t destination,
                 const void *udp_packet, usize length)
{
    u8 data[12 + ETHERNET_PAYLOAD_MAX];
    u8 *p = data;
    if (!udp_packet || length < UDP_HEADER_SIZE || length > ETHERNET_PAYLOAD_MAX)
        return 0;
    ip_copy(p, source); p += 4;
    ip_copy(p, destination); p += 4;
    *p++ = 0; *p++ = UDP_PROTOCOL;
    write_be16(p, (u16)length); p += 2;
    for (usize i = 0; i < length; i++) p[i] = ((const u8 *)udp_packet)[i];
    p[6] = 0; p[7] = 0;
    return net_checksum(data, 12 + length);
}

void udp_init(void)
{
    for (u32 i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
        endpoints[i].port = 0;
        endpoints[i].handler = 0;
    }
}

bool udp_register_handler(u16 port, udp_handler_t handler)
{
    if (!port || !handler) return false;
    for (u32 i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
        if (endpoints[i].handler && endpoints[i].port == port) {
            endpoints[i].handler = handler;
            return true;
        }
    }
    for (u32 i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
        if (!endpoints[i].handler) {
            endpoints[i].port = port;
            endpoints[i].handler = handler;
            return true;
        }
    }
    return false;
}

bool udp_unregister_handler(u16 port, udp_handler_t handler)
{
    if (!port || !handler) return false;
    for (u32 i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
        if (endpoints[i].port == port && endpoints[i].handler == handler) {
            endpoints[i].port = 0;
            endpoints[i].handler = 0;
            return true;
        }
    }
    return false;
}

void udp_receive(net_device_t *device, net_ipv4_t source,
                 net_ipv4_t destination, const u8 *packet, usize length)
{
    const udp_wire_header_t *header;
    u16 packet_length, checksum;
    u16 destination_port;
    if (!device || !packet || length < UDP_HEADER_SIZE) return;
    header = (const udp_wire_header_t *)packet;
    packet_length = read_be16((const u8 *)&header->length);
    if (packet_length < UDP_HEADER_SIZE || packet_length > length) return;
    checksum = read_be16((const u8 *)&header->checksum);
    if (checksum) {
        u8 copy[ETHERNET_PAYLOAD_MAX];
        if (packet_length > sizeof(copy)) return;
        for (u16 i = 0; i < packet_length; i++) copy[i] = packet[i];
        copy[6] = 0; copy[7] = 0;
        if (udp_checksum(source, destination, copy, packet_length) != checksum)
            return;
    }
    destination_port = read_be16((const u8 *)&header->destination_port);
    for (u32 i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
        if (endpoints[i].handler && endpoints[i].port == destination_port) {
            endpoints[i].handler(device, source,
                read_be16((const u8 *)&header->source_port), destination,
                destination_port, packet + UDP_HEADER_SIZE,
                packet_length - UDP_HEADER_SIZE);
            return;
        }
    }
}

bool udp_transmit(net_device_t *device, net_ipv4_t source,
                  net_ipv4_t destination, u16 source_port, u16 destination_port,
                  const void *payload, usize length)
{
    u8 packet[ETHERNET_PAYLOAD_MAX];
    udp_wire_header_t *header = (udp_wire_header_t *)packet;
    usize total = UDP_HEADER_SIZE + length;
    u16 sum;
    if (!device || !payload || !source_port || !destination_port ||
        length > sizeof(packet) - UDP_HEADER_SIZE) return false;
    write_be16((u8 *)&header->source_port, source_port);
    write_be16((u8 *)&header->destination_port, destination_port);
    write_be16((u8 *)&header->length, (u16)total);
    header->checksum = 0;
    for (usize i = 0; i < length; i++) packet[UDP_HEADER_SIZE + i] = ((const u8 *)payload)[i];
    sum = udp_checksum(source, destination, packet, total);
    if (!sum) sum = 0xffff;
    write_be16((u8 *)&header->checksum, sum);
    return ipv4_transmit_from(device, source, destination, UDP_PROTOCOL,
                              packet, total);
}
