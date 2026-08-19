#include <net/checksum.h>
#include <net/icmp.h>
#include <timer.h>

typedef struct __attribute__((packed)) {
    u8 type;
    u8 code;
    u16 checksum;
    u16 identifier;
    u16 sequence;
} icmp_echo_wire_t;

_Static_assert(sizeof(icmp_echo_wire_t) == 8,
               "ICMP echo header must be 8 bytes");

static u16 icmp_read_be16(const u8 *bytes)
{
    return ((u16)bytes[0] << 8) | bytes[1];
}

static void icmp_write_be16(u8 *bytes, u16 value)
{
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

static struct {
    bool pending;
    bool received;
    u16 identifier;
    u16 sequence;
    net_ipv4_t source;
    usize reply_length;
    u64 received_at_ms;
} ping_state;

void icmp_init(void)
{
    ping_state.pending = false;
    ping_state.received = false;
}

bool icmp_echo_request(net_device_t *device, net_ipv4_t destination,
                       u16 identifier, u16 sequence,
                       const void *payload, usize length)
{
    u8 packet[NET_ETHERNET_MAX_FRAME - NET_ETHERNET_HEADER_SIZE - 20U];
    icmp_echo_wire_t *echo = (icmp_echo_wire_t *)packet;

    if (!device || !payload || length > sizeof(packet) - sizeof(*echo)) return false;
    echo->type = ICMP_TYPE_ECHO_REQUEST;
    echo->code = 0;
    echo->checksum = 0;
    icmp_write_be16((u8 *)&echo->identifier, identifier);
    icmp_write_be16((u8 *)&echo->sequence, sequence);
    for (usize i = 0; i < length; i++) packet[sizeof(*echo) + i] = ((const u8 *)payload)[i];
    icmp_write_be16((u8 *)&echo->checksum,
                    net_checksum(packet, sizeof(*echo) + length));
    ping_state.pending = true;
    ping_state.received = false;
    ping_state.identifier = identifier;
    ping_state.sequence = sequence;
    if (!ipv4_transmit(device, destination, IPV4_PROTOCOL_ICMP, packet,
                       sizeof(*echo) + length)) {
        ping_state.pending = false;
        return false;
    }
    return true;
}

bool icmp_echo_reply_received(u16 identifier, u16 sequence)
{
    return ping_state.received && ping_state.identifier == identifier &&
           ping_state.sequence == sequence;
}

bool icmp_echo_reply_info(u16 identifier, u16 sequence, net_ipv4_t *source,
                          usize *reply_length, u64 *received_at_ms)
{
    if (!icmp_echo_reply_received(identifier, sequence)) return false;
    if (source) *source = ping_state.source;
    if (reply_length) *reply_length = ping_state.reply_length;
    if (received_at_ms) *received_at_ms = ping_state.received_at_ms;
    return true;
}

static void icmp_send_reply(net_device_t *device, net_ipv4_t destination,
                            const u8 *packet, usize length)
{
    u8 reply[NET_ETHERNET_MAX_FRAME - NET_ETHERNET_HEADER_SIZE - 20U];
    if (length > sizeof(reply)) return;
    for (usize i = 0; i < length; i++) reply[i] = packet[i];
    reply[0] = ICMP_TYPE_ECHO_REPLY;
    reply[2] = 0;
    reply[3] = 0;
    icmp_write_be16(reply + 2, net_checksum(reply, length));
    (void)ipv4_transmit(device, destination, IPV4_PROTOCOL_ICMP, reply, length);
}

void icmp_receive(net_device_t *device, net_ipv4_t source,
                  net_ipv4_t destination, const u8 *packet, usize length)
{
    u8 type;
    u8 code;
    u16 identifier;
    u16 sequence;

    (void)destination;
    if (!device || !packet || length < sizeof(icmp_echo_wire_t) ||
        !net_checksum_valid(packet, length)) return;
    type = packet[0];
    code = packet[1];
    if (code != 0 || (type != ICMP_TYPE_ECHO_REPLY && type != ICMP_TYPE_ECHO_REQUEST)) return;
    identifier = icmp_read_be16(packet + 4);
    sequence = icmp_read_be16(packet + 6);
    if (type == ICMP_TYPE_ECHO_REPLY) {
        if (ping_state.pending && ping_state.identifier == identifier &&
            ping_state.sequence == sequence) {
            ping_state.received = true;
            ping_state.pending = false;
            ping_state.source = source;
            ping_state.reply_length = length - sizeof(icmp_echo_wire_t);
            ping_state.received_at_ms = timer_uptime_ms();
        }
    } else {
        icmp_send_reply(device, source, packet, length);
    }
}
