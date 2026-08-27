#include <net/dhcp.h>
#include <net/udp.h>
#include <net/config.h>
#include <timer.h>
#include <scheduler.h>
#include <string.h>

#define DHCP_CLIENT_PORT 68U
#define DHCP_SERVER_PORT 67U
#define DHCP_MAGIC_COOKIE 0x63825363U
#define DHCP_DISCOVER 1U
#define DHCP_OFFER 2U
#define DHCP_REQUEST 3U
#define DHCP_DECLINE 4U
#define DHCP_ACK 5U
#define DHCP_NAK 6U
#define DHCP_OPTION_PAD 0U
#define DHCP_OPTION_SUBNET_MASK 1U
#define DHCP_OPTION_ROUTER 3U
#define DHCP_OPTION_DNS 6U
#define DHCP_OPTION_REQUESTED_IP 50U
#define DHCP_OPTION_LEASE_TIME 51U
#define DHCP_OPTION_MESSAGE_TYPE 53U
#define DHCP_OPTION_SERVER_ID 54U
#define DHCP_OPTION_PARAMETER_LIST 55U
#define DHCP_OPTION_CLIENT_ID 61U
#define DHCP_OPTION_END 255U
#define DHCP_WAIT_MS 20000U

enum {
    DHCP_STATE_INIT = 0,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND
};

typedef struct __attribute__((packed)) {
    u8 op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u8 ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    u8 chaddr[16];
    u8 sname[64];
    u8 file[128];
    u32 cookie;
} dhcp_wire_t;
_Static_assert(sizeof(dhcp_wire_t) == 240, "DHCP fixed header size");

typedef struct {
    net_device_t *device;
    u32 xid;
    u8 state;
    net_ipv4_t offered;
    dhcp_lease_t lease;
} dhcp_state_t;
static volatile dhcp_state_t state;
static volatile u32 dhcp_event;
static u32 dhcp_event_load(void) { return __atomic_load_n(&dhcp_event, __ATOMIC_ACQUIRE); }
static void dhcp_event_store(u32 value) { __atomic_store_n(&dhcp_event, value, __ATOMIC_RELEASE); }

static bool dhcp_wait_for_event(bool offer)
{
    timer_monotonic_deadline_t deadline;

    if (!timer_monotonic_deadline_start(&deadline,
                                        (u64)DHCP_WAIT_MS * 1000ULL)) {
        return false;
    }

    for (;;) {
        u32 event = dhcp_event_load();
        if ((offer && event == DHCP_OFFER) ||
            (!offer && (event == DHCP_ACK || event == DHCP_NAK))) {
            return true;
        }
        if (timer_monotonic_deadline_expired(&deadline)) {
            return false;
        }

        /* This wait can run inside a userspace network syscall, where SYSCALL
         * has masked IF.  A cooperative yield is not sufficient when no
         * other worker is runnable: it can return on this same stack and
         * leave the CPU polling with interrupts disabled.  Sleep through the
         * existing scheduler path so the idle context can receive device
         * IRQs; the monotonic deadline remains the authoritative timeout. */
        (void)scheduler_sleep(1);
    }
}

static u16 be16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static u32 be32(u32 v) { return ((v & 0xffU) << 24) | ((v & 0xff00U) << 8) |
                                   ((v >> 8) & 0xff00U) | ((v >> 24) & 0xffU); }
static u32 read_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}
static net_ipv4_t ip_from(const u8 *p) { return (net_ipv4_t){{p[0],p[1],p[2],p[3]}}; }
static void ip_to(u8 *p, net_ipv4_t ip) { for (u32 i=0;i<4;i++) p[i]=ip.octet[i]; }

static bool option_find(const u8 *options, usize length, u8 wanted,
                        const u8 **value, u8 *value_length)
{
    usize i = 0;
    while (i < length) {
        u8 type = options[i++];
        u8 size;
        if (type == DHCP_OPTION_PAD) continue;
        if (type == DHCP_OPTION_END) return false;
        if (i >= length) return false;
        size = options[i++];
        if ((usize)size > length - i) return false;
        if (type == wanted) { *value = options + i; *value_length = size; return true; }
        i += size;
    }
    return false;
}

static bool add_option(u8 *options, usize capacity, usize *used, u8 type,
                       const void *value, u8 length)
{
    if (!options || !used || *used + 2U + length > capacity) return false;
    options[(*used)++] = type; options[(*used)++] = length;
    for (u8 i=0;i<length;i++) options[(*used)++] = ((const u8 *)value)[i];
    return true;
}

static bool send_message(net_device_t *device, u8 message_type,
                         net_ipv4_t requested, net_ipv4_t server,
                         bool include_request)
{
    u8 packet[576] = {0};
    dhcp_wire_t *fixed = (dhcp_wire_t *)packet;
    usize used = sizeof(*fixed);
    static const net_ipv4_t zero = {{0,0,0,0}};
    static const u8 params[] = {1,3,6,51,54};
    u8 client_id[7];
    fixed->op = 1; fixed->htype = 1; fixed->hlen = 6;
    fixed->xid = be32(state.xid); fixed->flags = be16(0x8000);
    fixed->cookie = be32(DHCP_MAGIC_COOKIE);
    for (u32 i=0;i<6;i++) fixed->chaddr[i] = device->mac[i];
    if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_MESSAGE_TYPE,
                    &message_type, 1)) return false;
    client_id[0] = 1; for (u32 i=0;i<6;i++) client_id[i+1]=device->mac[i];
    if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_CLIENT_ID, client_id, 7)) return false;
    if (include_request) {
        u8 ip[4]; ip_to(ip, requested);
        if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_REQUESTED_IP, ip, 4)) return false;
        ip_to(ip, server);
        if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_SERVER_ID, ip, 4)) return false;
    }
    if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_PARAMETER_LIST,
                    params, sizeof(params))) return false;
    packet[used++] = DHCP_OPTION_END;
    bool sent = udp_transmit(device, zero, (net_ipv4_t){{255,255,255,255}},
                             DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet, used);
    return sent;
}

static void dhcp_udp_receive(net_device_t *device, net_ipv4_t source,
                             u16 source_port, net_ipv4_t destination,
                             u16 destination_port, const u8 *packet, usize length)
{
    const dhcp_wire_t *fixed;
    const u8 *options, *value;
    u8 value_length, message;
    (void)source; (void)source_port; (void)destination; (void)destination_port;
    if (!device || device != state.device) return;
    if (length < sizeof(dhcp_wire_t)) return;
    fixed = (const dhcp_wire_t *)packet;
    if (fixed->op != 2 || fixed->htype != 1 || fixed->hlen != 6 ||
        fixed->xid != be32(state.xid) || fixed->cookie != be32(DHCP_MAGIC_COOKIE)) return;
    for (u32 i=0;i<6;i++) if (fixed->chaddr[i] != device->mac[i]) return;
    options = packet + sizeof(*fixed);
    if (!option_find(options, length - sizeof(*fixed), DHCP_OPTION_MESSAGE_TYPE, &value, &value_length) || value_length != 1) return;
    message = value[0];
    if (message == DHCP_OFFER && state.state == DHCP_STATE_SELECTING) {
        state.offered = ip_from(fixed->yiaddr);
        state.lease.address = state.offered;
        state.lease.server = (net_ipv4_t){{0,0,0,0}};
        if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_SERVER_ID,
                        &value, &value_length) && value_length == 4)
            state.lease.server = ip_from(value);
        state.lease.netmask = (net_ipv4_t){{0,0,0,0}};
        state.lease.gateway = (net_ipv4_t){{0,0,0,0}};
        state.lease.dns = (net_ipv4_t){{0,0,0,0}};
        state.lease.has_gateway = false; state.lease.has_dns = false;
        if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_SUBNET_MASK, &value, &value_length) && value_length == 4) state.lease.netmask=ip_from(value);
        if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_ROUTER, &value, &value_length) && value_length >= 4) { state.lease.gateway=ip_from(value); state.lease.has_gateway=true; }
        if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_DNS, &value, &value_length) && value_length >= 4) { state.lease.dns=ip_from(value); state.lease.has_dns=true; }
        /* The waiter sends REQUEST only after this RX callback has returned
         * and the E1000 descriptor has been recycled.  This also avoids
         * protocol transmission re-entering the device's receive callback. */
        dhcp_event_store(DHCP_OFFER);
        return;
    }
    if (message == DHCP_NAK && state.state == DHCP_STATE_REQUESTING) {
        dhcp_event_store(DHCP_NAK);
        return;
    }
    if (message != DHCP_ACK || state.state != DHCP_STATE_REQUESTING) return;
    state.lease.address = ip_from(fixed->yiaddr);
    state.lease.netmask = (net_ipv4_t){{0,0,0,0}};
    state.lease.gateway = (net_ipv4_t){{0,0,0,0}};
    state.lease.dns = (net_ipv4_t){{0,0,0,0}};
    state.lease.server = (net_ipv4_t){{0,0,0,0}};
    state.lease.has_gateway = false; state.lease.has_dns = false; state.lease.lease_seconds = 0;
    if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_SUBNET_MASK, &value, &value_length) && value_length == 4) state.lease.netmask=ip_from(value);
    if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_ROUTER, &value, &value_length) && value_length >= 4) { state.lease.gateway=ip_from(value); state.lease.has_gateway=true; }
    if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_DNS, &value, &value_length) && value_length >= 4) { state.lease.dns=ip_from(value); state.lease.has_dns=true; }
    if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_SERVER_ID, &value, &value_length) && value_length == 4) state.lease.server=ip_from(value);
    if (option_find(options, length-sizeof(*fixed), DHCP_OPTION_LEASE_TIME, &value, &value_length) && value_length == 4) state.lease.lease_seconds=read_be32(value);
    state.state = DHCP_STATE_BOUND;
    dhcp_event_store(DHCP_ACK);
}

void dhcp_init(void)
{
    state = (dhcp_state_t){0};
    dhcp_event_store(0);
    (void)udp_register_handler(68, dhcp_udp_receive);
}

void dhcp_reset(void)
{
    state = (dhcp_state_t){0};
    dhcp_event_store(0);
}

bool dhcp_acquire(net_device_t *device, dhcp_lease_t *lease)
{
    if (!device || !lease) return false;
    state.device = device;
    state.xid = (u32)timer_ticks() ^ ((u32)device->mac[4] << 8) ^ device->mac[5] ^ 0x4d475200U;
    state.state = DHCP_STATE_SELECTING;
    dhcp_event_store(0);
    if (!send_message(device, DHCP_DISCOVER, (net_ipv4_t){{0}}, (net_ipv4_t){{0}}, false)) {
        return false;
    }
    if (!dhcp_wait_for_event(true)) {
        return false;
    }

    /* Arm REQUESTING before transmitting so an immediate ACK cannot race the
     * state transition.  The acquire/release event handoff makes the parsed
     * offer fields visible outside the RX interrupt callback. */
    state.state = DHCP_STATE_REQUESTING;
    dhcp_event_store(0);
    if (!send_message(device, DHCP_REQUEST, state.offered,
                      state.lease.server, true)) {
        return false;
    }

    if (!dhcp_wait_for_event(false) || dhcp_event_load() != DHCP_ACK ||
        state.state != DHCP_STATE_BOUND) {
        return false;
    }

    lease->address = state.lease.address;
    lease->netmask = state.lease.netmask;
    lease->gateway = state.lease.gateway;
    lease->dns = state.lease.dns;
    lease->server = state.lease.server;
    lease->lease_seconds = state.lease.lease_seconds;
    lease->has_gateway = state.lease.has_gateway;
    lease->has_dns = state.lease.has_dns;
    if (!lease->netmask.octet[0] && !lease->netmask.octet[1] && !lease->netmask.octet[2] && !lease->netmask.octet[3]) lease->netmask=(net_ipv4_t){{255,255,255,255}};
    return true;
}
