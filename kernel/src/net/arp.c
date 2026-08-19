#include <net/arp.h>
#include <net/ethernet.h>

#define ARP_HARDWARE_ETHERNET 1U
#define ARP_PROTOCOL_IPV4     0x0800U
#define ARP_OPERATION_REQUEST 1U
#define ARP_OPERATION_REPLY   2U

typedef struct __attribute__((packed)) {
    u16 hardware_type;
    u16 protocol_type;
    u8 hardware_length;
    u8 protocol_length;
    u16 operation;
    u8 sender_mac[6];
    u8 sender_ipv4[4];
    u8 target_mac[6];
    u8 target_ipv4[4];
} arp_wire_packet_t;

_Static_assert(sizeof(arp_wire_packet_t) == 28,
               "ARP Ethernet/IPv4 packet must be 28 bytes");

typedef struct {
    bool valid;
    net_ipv4_t address;
    u8 mac[6];
} arp_cache_entry_t;

static arp_cache_entry_t cache[ARP_CACHE_CAPACITY];

static u16 arp_read_be16(const u8 *bytes)
{
    return ((u16)bytes[0] << 8) | bytes[1];
}

static void arp_write_be16(u8 *bytes, u16 value)
{
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

static bool arp_ip_is_zero(net_ipv4_t address)
{
    return address.octet[0] == 0 && address.octet[1] == 0 &&
           address.octet[2] == 0 && address.octet[3] == 0;
}

static net_ipv4_t arp_ip_from_wire(const u8 bytes[4])
{
    net_ipv4_t address = {{bytes[0], bytes[1], bytes[2], bytes[3]}};
    return address;
}

static void arp_ip_to_wire(u8 bytes[4], net_ipv4_t address)
{
    for (u32 i = 0; i < 4; i++) bytes[i] = address.octet[i];
}

static bool arp_mac_is_zero(const u8 mac[6])
{
    u8 value = 0;
    for (u32 i = 0; i < 6; i++) value |= mac[i];
    return value == 0;
}

static bool arp_mac_is_valid_sender(const u8 mac[6])
{
    /* ARP sender hardware addresses must be nonzero unicast addresses. */
    return !arp_mac_is_zero(mac) && !(mac[0] & 1U);
}

static void arp_cache_update(net_ipv4_t address, const u8 mac[6])
{
    u32 free_index = ARP_CACHE_CAPACITY;

    if (arp_ip_is_zero(address) || !arp_mac_is_valid_sender(mac)) return;
    for (u32 i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (cache[i].valid && net_ipv4_equal(cache[i].address, address)) {
            net_mac_copy(cache[i].mac, mac);
            return;
        }
        if (!cache[i].valid && free_index == ARP_CACHE_CAPACITY) free_index = i;
    }
    if (free_index < ARP_CACHE_CAPACITY) {
        cache[free_index].valid = true;
        cache[free_index].address = address;
        net_mac_copy(cache[free_index].mac, mac);
    }
}

static bool arp_ip_equal(net_ipv4_t left, net_ipv4_t right)
{
    for (u32 i = 0; i < 4; i++) {
        if (left.octet[i] != right.octet[i]) return false;
    }
    return true;
}

void arp_init(void)
{
    for (u32 i = 0; i < ARP_CACHE_CAPACITY; i++) cache[i].valid = false;
}

bool arp_lookup(const net_ipv4_t *address, u8 mac_out[6])
{
    if (!address || !mac_out) return false;
    for (u32 i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (cache[i].valid && arp_ip_equal(cache[i].address, *address)) {
            net_mac_copy(mac_out, cache[i].mac);
            return true;
        }
    }
    return false;
}

bool arp_request(net_device_t *device, net_ipv4_t address)
{
    arp_wire_packet_t packet = {0};
    static const u8 broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const net_ipv4_t *local = net_local_ipv4();

    if (!device || !local || arp_ip_is_zero(address)) return false;
    arp_write_be16((u8 *)&packet.hardware_type, ARP_HARDWARE_ETHERNET);
    arp_write_be16((u8 *)&packet.protocol_type, ARP_PROTOCOL_IPV4);
    packet.hardware_length = 6;
    packet.protocol_length = 4;
    arp_write_be16((u8 *)&packet.operation, ARP_OPERATION_REQUEST);
    net_mac_copy(packet.sender_mac, device->mac);
    arp_ip_to_wire(packet.sender_ipv4, *local);
    arp_ip_to_wire(packet.target_ipv4, address);
    return ethernet_transmit(device, broadcast, ETHERNET_ETHERTYPE_ARP,
                             &packet, sizeof(packet));
}

usize arp_snapshot(arp_snapshot_entry_t *entries, usize capacity)
{
    usize count = 0;
    for (u32 i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (!cache[i].valid) continue;
        if (entries && count < capacity) entries[count] = (arp_snapshot_entry_t){ cache[i].address, {0}, true };
        if (entries && count < capacity) net_mac_copy(entries[count].mac, cache[i].mac);
        count++;
    }
    return count;
}

static bool arp_send_reply(net_device_t *device, const arp_wire_packet_t *request)
{
    arp_wire_packet_t reply = {0};
    const net_ipv4_t *local = net_local_ipv4();
    net_ipv4_t requester = arp_ip_from_wire(request->sender_ipv4);

    arp_write_be16((u8 *)&reply.hardware_type, ARP_HARDWARE_ETHERNET);
    arp_write_be16((u8 *)&reply.protocol_type, ARP_PROTOCOL_IPV4);
    reply.hardware_length = 6;
    reply.protocol_length = 4;
    arp_write_be16((u8 *)&reply.operation, ARP_OPERATION_REPLY);
    net_mac_copy(reply.sender_mac, device->mac);
    arp_ip_to_wire(reply.sender_ipv4, *local);
    net_mac_copy(reply.target_mac, request->sender_mac);
    arp_ip_to_wire(reply.target_ipv4, requester);
    return ethernet_transmit(device, request->sender_mac,
                             ETHERNET_ETHERTYPE_ARP, &reply, sizeof(reply));
}

void arp_receive(net_device_t *device, const u8 *packet, usize length)
{
    const arp_wire_packet_t *arp;
    net_ipv4_t sender;
    net_ipv4_t target;
    const net_ipv4_t *local = net_local_ipv4();
    u16 operation;

    /* Ethernet frames are padded to the minimum frame size.  The ARP wire
     * packet occupies the first 28 bytes; trailing padding is not part of
     * the ARP message and is intentionally ignored. */
    if (!device || !packet || length < sizeof(arp_wire_packet_t) || !local) return;
    arp = (const arp_wire_packet_t *)packet;
    if (arp_read_be16((const u8 *)&arp->hardware_type) != ARP_HARDWARE_ETHERNET ||
        arp_read_be16((const u8 *)&arp->protocol_type) != ARP_PROTOCOL_IPV4 ||
        arp->hardware_length != 6 || arp->protocol_length != 4) return;
    operation = arp_read_be16((const u8 *)&arp->operation);
    if (operation != ARP_OPERATION_REQUEST && operation != ARP_OPERATION_REPLY) return;
    if (!arp_mac_is_valid_sender(arp->sender_mac)) return;

    sender = arp_ip_from_wire(arp->sender_ipv4);
    target = arp_ip_from_wire(arp->target_ipv4);
    if (arp_ip_is_zero(sender)) return;
    arp_cache_update(sender, arp->sender_mac);

    if (!net_ipv4_equal(target, *local)) return;
    if (operation == ARP_OPERATION_REQUEST) {
        (void)arp_send_reply(device, arp);
    }
}
