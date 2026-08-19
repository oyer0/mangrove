#include <net/arp.h>
#include <net/ethernet.h>
#include <net/ipv4.h>

static u16 ethernet_read_be16(const u8 *bytes)
{
    return ((u16)bytes[0] << 8) | bytes[1];
}

static void ethernet_write_be16(u8 *bytes, u16 value)
{
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

bool net_mac_equal(const u8 left[6], const u8 right[6])
{
    u8 difference = 0;
    for (u32 i = 0; i < 6; i++) difference |= left[i] ^ right[i];
    return difference == 0;
}

void net_mac_copy(u8 destination[6], const u8 source[6])
{
    for (u32 i = 0; i < 6; i++) destination[i] = source[i];
}

bool net_mac_is_broadcast(const u8 mac[6])
{
    static const u8 broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return net_mac_equal(mac, broadcast);
}

void ethernet_init(void)
{
    net_set_receive_handler(ethernet_receive);
}

bool ethernet_transmit(net_device_t *device, const u8 destination[6],
                       u16 ethertype, const void *payload, usize length)
{
    u8 frame[NET_ETHERNET_MAX_FRAME];
    const u8 *source = (const u8 *)payload;

    if (!device || !destination || !payload ||
        length > ETHERNET_PAYLOAD_MAX || length > device->mtu) {
        return false;
    }

    net_mac_copy(frame, destination);
    net_mac_copy(frame + 6, device->mac);
    ethernet_write_be16(frame + 12, ethertype);
    for (usize i = 0; i < length; i++) frame[NET_ETHERNET_HEADER_SIZE + i] = source[i];

    return netdev_transmit(device, frame,
                           NET_ETHERNET_HEADER_SIZE + length);
}

void ethernet_receive(net_device_t *device, const u8 *frame, usize length)
{
    u16 ethertype;

    if (!device || !frame || length < NET_ETHERNET_HEADER_SIZE) return;
    if (!net_mac_equal(frame, device->mac) && !net_mac_is_broadcast(frame)) return;

    ethertype = ethernet_read_be16(frame + 12);
    switch (ethertype) {
    case ETHERNET_ETHERTYPE_ARP:
        arp_receive(device, frame + NET_ETHERNET_HEADER_SIZE,
                    length - NET_ETHERNET_HEADER_SIZE);
        break;
    case ETHERNET_ETHERTYPE_IPV4:
        ipv4_receive(device, frame + NET_ETHERNET_HEADER_SIZE,
                     length - NET_ETHERNET_HEADER_SIZE);
        break;
    default:
        break;
    }
}
