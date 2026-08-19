#include <net/arp.h>
#include <net/checksum.h>
#include <net/ethernet.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/ipv4.h>



typedef struct __attribute__((packed)) {
    u8 version_ihl;
    u8 dscp_ecn;
    u16 total_length;
    u16 identification;
    u16 flags_fragment;
    u8 ttl;
    u8 protocol;
    u16 checksum;
    u8 source[4];
    u8 destination[4];
} ipv4_wire_header_t;

_Static_assert(sizeof(ipv4_wire_header_t) == 20,
               "IPv4 header must be 20 bytes");

static u16 ipv4_read_be16(const u8 *bytes)
{
    return ((u16)bytes[0] << 8) | bytes[1];
}


static void ipv4_write_be16(u8 *bytes, u16 value)
{
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

static net_ipv4_t ipv4_from_wire(const u8 bytes[4])
{
    net_ipv4_t result = {{bytes[0], bytes[1], bytes[2], bytes[3]}};
    return result;
}

static void ipv4_to_wire(u8 bytes[4], net_ipv4_t address)
{
    for (u32 i = 0; i < 4; i++) bytes[i] = address.octet[i];
}

static bool ipv4_same_subnet(net_ipv4_t left, net_ipv4_t right)
{
    const net_ipv4_t *mask = net_netmask_ipv4();
    if (!mask) return false;
    for (u32 i = 0; i < 4; i++) {
        if ((left.octet[i] & mask->octet[i]) !=
            (right.octet[i] & mask->octet[i])) return false;
    }
    return true;
}

static bool ipv4_is_broadcast(net_ipv4_t address)
{
    return address.octet[0] == 255 && address.octet[1] == 255 &&
           address.octet[2] == 255 && address.octet[3] == 255;
}

static bool ipv4_next_hop(net_ipv4_t destination, net_ipv4_t *next_hop)
{
    const net_ipv4_t *gateway = net_gateway_ipv4();
    const net_ipv4_t *local = net_local_ipv4();
    if (!next_hop || !local || !gateway) return false;
    *next_hop = ipv4_same_subnet(destination, *local) ? destination : *gateway;
    return true;
}

void ipv4_init(void)
{
}

bool ipv4_transmit_from(net_device_t *device, net_ipv4_t source,
                        net_ipv4_t destination, u8 protocol,
                        const void *payload, usize length)
{
    u8 packet[NET_ETHERNET_MAX_FRAME - NET_ETHERNET_HEADER_SIZE];
    ipv4_wire_header_t *header = (ipv4_wire_header_t *)packet;
    net_ipv4_t next_hop;
    u8 destination_mac[6];
    static u16 identification;
    usize total_length = sizeof(*header) + length;

    if (!device || !payload || device->mtu < sizeof(*header) ||
        length > device->mtu - sizeof(*header) || total_length > 0xFFFFU) {
        return false;
    }
    if (ipv4_is_broadcast(destination)) {
        for (u32 i = 0; i < 6; i++) destination_mac[i] = 0xff;
    } else {
        if (!net_network_configured() || !ipv4_next_hop(destination, &next_hop)) return false;
        if (!arp_lookup(&next_hop, destination_mac)) {
            (void)arp_request(device, next_hop);
            return false;
        }
    }
    header->version_ihl = 0x45;
    header->dscp_ecn = 0;
    ipv4_write_be16((u8 *)&header->total_length, (u16)total_length);
    ipv4_write_be16((u8 *)&header->identification, identification++);
    ipv4_write_be16((u8 *)&header->flags_fragment, 0x4000U);
    header->ttl = IPV4_TTL_DEFAULT;
    header->protocol = protocol;
    header->checksum = 0;
    ipv4_to_wire(header->source, source);
    ipv4_to_wire(header->destination, destination);
    for (usize i = 0; i < length; i++) packet[sizeof(*header) + i] = ((const u8 *)payload)[i];
    ipv4_write_be16((u8 *)&header->checksum, net_checksum(packet, sizeof(*header)));
    if (!ethernet_transmit(device, destination_mac, ETHERNET_ETHERTYPE_IPV4,
                           packet, total_length)) {
        return false;
    }
    return true;
}

bool ipv4_transmit(net_device_t *device, net_ipv4_t destination,
                   u8 protocol, const void *payload, usize length)
{
    return ipv4_transmit_from(device, *net_local_ipv4(), destination, protocol,
                              payload, length);
}

void ipv4_receive(net_device_t *device, const u8 *packet, usize length)
{
    const ipv4_wire_header_t *header;
    const net_ipv4_t *local = net_local_ipv4();
    net_ipv4_t source;
    net_ipv4_t destination;
    usize header_length;
    usize total_length;

    if (!device || !packet || length < sizeof(ipv4_wire_header_t) || !local) return;
    header = (const ipv4_wire_header_t *)packet;
    if ((header->version_ihl >> 4) != 4 || (header->version_ihl & 0x0F) < 5) return;
    header_length = (usize)(header->version_ihl & 0x0F) * 4U;
    if (header_length > length || !net_checksum_valid(packet, header_length)) return;
    total_length = ipv4_read_be16((const u8 *)&header->total_length);
    if (total_length < header_length || total_length > length) return;
    source = ipv4_from_wire(header->source);
    destination = ipv4_from_wire(header->destination);
    if (!net_ipv4_equal(destination, *local) && !ipv4_is_broadcast(destination)) return;
    if (header->protocol == IPV4_PROTOCOL_ICMP) {
        icmp_receive(device, source, destination, packet + header_length,
                     total_length - header_length);
    }
    else if (header->protocol == IPV4_PROTOCOL_UDP) {
        udp_receive(device, source, destination, packet + header_length,
                    total_length - header_length);
    }
    else if (header->protocol == TCP_PROTOCOL) {
        tcp_receive(device, source, destination, packet + header_length,
                    total_length - header_length);
    }
}
