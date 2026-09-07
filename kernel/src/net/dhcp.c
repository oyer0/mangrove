/* SPDX-License-Identifier: GPL-3.0-only */
#include <net/dhcp.h>
#include <net/udp.h>
#include <timer.h>
#include <scheduler.h>
#include <string.h>

#define DHCP_CLIENT_PORT 68U
#define DHCP_SERVER_PORT 67U
#define DHCP_MAGIC_COOKIE 0x63825363U
#define DHCP_DISCOVER 1U
#define DHCP_OFFER 2U
#define DHCP_REQUEST 3U
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
#define DHCP_OPTION_RENEWAL_TIME 58U
#define DHCP_OPTION_REBINDING_TIME 59U
#define DHCP_OPTION_END 255U
#define DHCP_WAIT_MS 20000U
#define DHCP_RENEW_RETRY_MS 1000U

enum {
    DHCP_STATE_INIT = 0,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING,
};

enum {
    DHCP_EVENT_NONE = 0,
    DHCP_EVENT_OFFER,
    DHCP_EVENT_ACK,
    DHCP_EVENT_NAK,
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
    u64 device_generation;
    u32 xid;
    u8 state;
    net_ipv4_t offered;
    dhcp_lease_t lease;
} dhcp_state_t;

static volatile dhcp_state_t state;
static volatile u32 dhcp_event;
static kernel_thread_t *volatile dhcp_waiter;

static u64 dhcp_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void dhcp_irq_restore(u64 flags)
{
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" ::: "memory");
}

static void dhcp_wake_waiter(void)
{
    kernel_thread_t *waiter = __atomic_load_n(&dhcp_waiter,
                                               __ATOMIC_ACQUIRE);

    if (waiter)
        (void)scheduler_unblock(waiter);
}

static u32 dhcp_event_load(void)
{
    return __atomic_load_n(&dhcp_event, __ATOMIC_ACQUIRE);
}

static void dhcp_event_store(u32 value)
{
    __atomic_store_n(&dhcp_event, value, __ATOMIC_RELEASE);
    if (value != DHCP_EVENT_NONE)
        dhcp_wake_waiter();
}

static u16 be16(u16 value)
{
    return (u16)((value >> 8) | (value << 8));
}

static u32 be32(u32 value)
{
    return ((value & 0xffU) << 24) | ((value & 0xff00U) << 8) |
           ((value >> 8) & 0xff00U) | ((value >> 24) & 0xffU);
}

static u32 read_be32(const u8 *value)
{
    return ((u32)value[0] << 24) | ((u32)value[1] << 16) |
           ((u32)value[2] << 8) | value[3];
}

static net_ipv4_t ip_from(const u8 *value)
{
    return (net_ipv4_t){{value[0], value[1], value[2], value[3]}};
}

static void ip_to(u8 *value, net_ipv4_t address)
{
    for (u32 index = 0; index < 4; index++) value[index] = address.octet[index];
}

static bool ip_zero(net_ipv4_t address)
{
    return address.octet[0] == 0 && address.octet[1] == 0 &&
           address.octet[2] == 0 && address.octet[3] == 0;
}

static bool ip_equal(net_ipv4_t left, net_ipv4_t right)
{
    return left.octet[0] == right.octet[0] &&
           left.octet[1] == right.octet[1] &&
           left.octet[2] == right.octet[2] &&
           left.octet[3] == right.octet[3];
}

static bool netmask_valid(net_ipv4_t mask)
{
    bool zero_seen = false;

    for (u32 octet = 0; octet < 4; octet++) {
        for (u8 bit = 0x80U; bit != 0; bit >>= 1U) {
            if (mask.octet[octet] & bit) {
                if (zero_seen) return false;
            } else {
                zero_seen = true;
            }
        }
    }
    return true;
}

static bool option_find(const u8 *options, usize length, u8 wanted,
                        const u8 **value, u8 *value_length)
{
    usize index = 0;

    if (!options || !value || !value_length) return false;
    while (index < length) {
        u8 type = options[index++];
        u8 size;
        if (type == DHCP_OPTION_PAD) continue;
        if (type == DHCP_OPTION_END) return false;
        if (index >= length) return false;
        size = options[index++];
        if ((usize)size > length - index) return false;
        if (type == wanted) {
            *value = options + index;
            *value_length = size;
            return true;
        }
        index += size;
    }
    return false;
}

static bool options_valid(const u8 *options, usize length)
{
    usize index = 0;

    if (!options) return false;
    while (index < length) {
        u8 type = options[index++];
        u8 size;

        if (type == DHCP_OPTION_PAD) continue;
        if (type == DHCP_OPTION_END) return true;
        if (index >= length) return false;
        size = options[index++];
        if ((usize)size > length - index) return false;
        index += size;
    }
    return false;
}

static bool add_option(u8 *options, usize capacity, usize *used, u8 type,
                       const void *value, u8 length)
{
    if (!options || !used || !value || *used + 2U + length > capacity)
        return false;
    options[(*used)++] = type;
    options[(*used)++] = length;
    for (u8 index = 0; index < length; index++)
        options[(*used)++] = ((const u8 *)value)[index];
    return true;
}

static bool send_message(net_device_t *device, u8 message_type,
                         net_ipv4_t requested, net_ipv4_t server,
                         bool include_request, bool renewal, bool rebinding)
{
    u8 packet[576] = {0};
    dhcp_wire_t *fixed = (dhcp_wire_t *)packet;
    usize used = sizeof(*fixed);
    static const net_ipv4_t zero = {{0, 0, 0, 0}};
    static const net_ipv4_t broadcast = {{255, 255, 255, 255}};
    static const u8 params[] = {1, 3, 6, 51, 54, 58, 59};
    u8 client_id[7];
    net_ipv4_t source = zero;
    net_ipv4_t destination = broadcast;

    if (!device) return false;
    fixed->op = 1;
    fixed->htype = 1;
    fixed->hlen = 6;
    fixed->xid = be32(state.xid);
    fixed->flags = be16((renewal || rebinding) ? (rebinding ? 0x8000 : 0) :
                        0x8000);
    fixed->cookie = be32(DHCP_MAGIC_COOKIE);
    if (renewal || rebinding) {
        ip_to(fixed->ciaddr, requested);
        source = requested;
        if (renewal && !rebinding && !ip_zero(server)) destination = server;
        else destination = broadcast;
    }
    for (u32 index = 0; index < 6; index++) fixed->chaddr[index] = device->mac[index];
    if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_MESSAGE_TYPE,
                    &message_type, 1)) return false;
    client_id[0] = 1;
    for (u32 index = 0; index < 6; index++) client_id[index + 1] = device->mac[index];
    if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_CLIENT_ID,
                    client_id, sizeof(client_id))) return false;
    if (include_request) {
        u8 address[4];
        ip_to(address, requested);
        if (!add_option(packet, sizeof(packet), &used,
                        DHCP_OPTION_REQUESTED_IP, address, sizeof(address)))
            return false;
        ip_to(address, server);
        if (!add_option(packet, sizeof(packet), &used, DHCP_OPTION_SERVER_ID,
                        address, sizeof(address))) return false;
    }
    if (!add_option(packet, sizeof(packet), &used,
                    DHCP_OPTION_PARAMETER_LIST, params, sizeof(params)))
        return false;
    packet[used++] = DHCP_OPTION_END;
    return udp_transmit(device, source, destination, DHCP_CLIENT_PORT,
                        DHCP_SERVER_PORT, packet, used);
}

static bool dhcp_timing_valid(u32 lease_seconds, u32 renewal_seconds,
                              u32 rebinding_seconds)
{
    return lease_seconds != 0 && renewal_seconds < rebinding_seconds &&
           rebinding_seconds < lease_seconds;
}

static bool option_seconds(const u8 *options, usize length, u8 type,
                           u32 *output, bool *present)
{
    usize index = 0;

    if (!options || !output || !present) return false;
    *output = 0;
    *present = false;
    while (index < length) {
        u8 option = options[index++];
        u8 value_length;
        if (option == DHCP_OPTION_PAD) continue;
        if (option == DHCP_OPTION_END) return true;
        if (index >= length) return false;
        value_length = options[index++];
        if ((usize)value_length > length - index) return false;
        if (option == type) {
            if (value_length != 4) return false;
            *output = read_be32(options + index);
            *present = true;
            return true;
        }
        index += value_length;
    }
    return true;
}

static void dhcp_udp_receive(net_device_t *device, net_ipv4_t source,
                             u16 source_port, net_ipv4_t destination,
                             u16 destination_port, const u8 *packet,
                             usize length)
{
    const dhcp_wire_t *fixed;
    const u8 *options;
    const u8 *value;
    u8 value_length;
    u8 message;
    bool lease_present;
    bool renewal_present;
    bool rebinding_present;
    u32 lease_seconds;
    u32 renewal_seconds;
    u32 rebinding_seconds;
    bool renewing;

    (void)source_port;
    (void)destination;
    (void)destination_port;
    if (!device || device != state.device ||
        !net_device_instance_current(device, state.device_generation) ||
        !packet ||
        length < sizeof(dhcp_wire_t)) return;
    fixed = (const dhcp_wire_t *)packet;
    if (fixed->op != 2 || fixed->htype != 1 || fixed->hlen != 6 ||
        fixed->xid != be32(state.xid) || fixed->cookie != be32(DHCP_MAGIC_COOKIE))
        return;
    for (u32 index = 0; index < 6; index++)
        if (fixed->chaddr[index] != device->mac[index]) return;
    options = packet + sizeof(*fixed);
    if (!options_valid(options, length - sizeof(*fixed))) return;
    if (!option_find(options, length - sizeof(*fixed), DHCP_OPTION_MESSAGE_TYPE,
                     &value, &value_length) || value_length != 1) return;
    message = value[0];
    renewing = state.state == DHCP_STATE_RENEWING ||
               state.state == DHCP_STATE_REBINDING;
    if (message == DHCP_OFFER && state.state == DHCP_STATE_SELECTING) {
        state.offered = ip_from(fixed->yiaddr);
        state.lease = (dhcp_lease_t){0};
        state.lease.address = state.offered;
        if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_SERVER_ID,
                        &value, &value_length)) {
            if (value_length != 4) return;
            state.lease.server = ip_from(value);
        }
        if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_SUBNET_MASK,
                        &value, &value_length)) {
            if (value_length != 4) return;
            state.lease.netmask = ip_from(value);
            if (!netmask_valid(state.lease.netmask)) return;
        }
        if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_ROUTER,
                        &value, &value_length)) {
            if (value_length < 4) return;
            state.lease.gateway = ip_from(value);
            if (ip_zero(state.lease.gateway)) return;
            state.lease.has_gateway = true;
        }
        if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_DNS,
                        &value, &value_length)) {
            if (value_length < 4) return;
            state.lease.dns = ip_from(value);
            if (ip_zero(state.lease.dns)) return;
            state.lease.has_dns = true;
        }
        dhcp_event_store(DHCP_EVENT_OFFER);
        return;
    }
    if (message == DHCP_NAK &&
        (state.state == DHCP_STATE_REQUESTING || renewing)) {
        dhcp_event_store(DHCP_EVENT_NAK);
        return;
    }
    if (message != DHCP_ACK ||
        (state.state != DHCP_STATE_REQUESTING && !renewing)) return;
    if (!renewing && ip_zero(ip_from(fixed->yiaddr))) return;
    if (renewing && state.state == DHCP_STATE_RENEWING &&
        !ip_zero(state.lease.server) && !ip_equal(source, state.lease.server))
        return;
    if (!option_seconds(options, length - sizeof(*fixed),
                        DHCP_OPTION_LEASE_TIME, &lease_seconds,
                        &lease_present) || !lease_present || !lease_seconds ||
        !option_seconds(options, length - sizeof(*fixed),
                        DHCP_OPTION_RENEWAL_TIME, &renewal_seconds,
                        &renewal_present) ||
        !option_seconds(options, length - sizeof(*fixed),
                        DHCP_OPTION_REBINDING_TIME, &rebinding_seconds,
                        &rebinding_present)) return;
    if (!renewal_present) renewal_seconds = lease_seconds / 2U;
    if (!rebinding_present)
        rebinding_seconds = (u32)(((u64)lease_seconds * 7ULL) / 8ULL);
    if (!renewal_seconds) renewal_seconds = 1U;
    if (rebinding_seconds <= renewal_seconds) rebinding_seconds = renewal_seconds + 1U;
    if (rebinding_seconds >= lease_seconds) rebinding_seconds = lease_seconds - 1U;
    if (!dhcp_timing_valid(lease_seconds, renewal_seconds, rebinding_seconds)) return;
    if (!ip_zero(ip_from(fixed->yiaddr)))
        state.lease.address = ip_from(fixed->yiaddr);
    if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_SUBNET_MASK,
                    &value, &value_length)) {
        if (value_length != 4) return;
        state.lease.netmask = ip_from(value);
        if (!netmask_valid(state.lease.netmask)) return;
    }
    if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_ROUTER,
                    &value, &value_length)) {
        if (value_length < 4) return;
        state.lease.gateway = ip_from(value);
        if (ip_zero(state.lease.gateway)) return;
        state.lease.has_gateway = true;
    }
    if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_DNS,
                    &value, &value_length)) {
        if (value_length < 4) return;
        state.lease.dns = ip_from(value);
        if (ip_zero(state.lease.dns)) return;
        state.lease.has_dns = true;
    }
    if (option_find(options, length - sizeof(*fixed), DHCP_OPTION_SERVER_ID,
                    &value, &value_length)) {
        if (value_length != 4) return;
        state.lease.server = ip_from(value);
    }
    state.lease.lease_seconds = lease_seconds;
    state.lease.renewal_seconds = renewal_seconds;
    state.lease.rebinding_seconds = rebinding_seconds;
    state.state = DHCP_STATE_BOUND;
    dhcp_event_store(DHCP_EVENT_ACK);
}

static bool dhcp_wait_for_event(bool offer, u32 timeout_ms)
{
    u64 start = timer_uptime_ms();
    u64 deadline = start > ~(u64)0 - timeout_ms ?
        ~(u64)0 : start + timeout_ms;
    kernel_thread_t *self = thread_current();

    if (!self) return false;

    for (;;) {
        u64 saved_flags = dhcp_irq_save();
        u64 now = timer_uptime_ms();
        u64 remaining = now >= deadline ? 0 : deadline - now;
        u32 event = dhcp_event_load();
        bool ready = (offer && event == DHCP_EVENT_OFFER) ||
            (!offer && (event == DHCP_EVENT_ACK || event == DHCP_EVENT_NAK));
        bool current = state.device && net_device_instance_current(
            state.device, state.device_generation);
        kernel_thread_t *registered = __atomic_load_n(
            &dhcp_waiter, __ATOMIC_ACQUIRE);

        if (ready || !current || remaining == 0) {
            if (registered == self)
                __atomic_store_n(&dhcp_waiter, NULL, __ATOMIC_RELEASE);
            dhcp_irq_restore(saved_flags);
            return ready && current;
        }
        if (registered && registered != self) {
            dhcp_irq_restore(saved_flags);
            return false;
        }

        /* Register before blocking while interrupts are masked.  The packet
         * receive path can then wake this exact sleeper, and the event cannot
         * race into the gap between the final check and scheduler_sleep(). */
        __atomic_store_n(&dhcp_waiter, self, __ATOMIC_RELEASE);
        if (!scheduler_sleep(remaining)) {
            if (__atomic_load_n(&dhcp_waiter, __ATOMIC_ACQUIRE) == self)
                __atomic_store_n(&dhcp_waiter, NULL, __ATOMIC_RELEASE);
            dhcp_irq_restore(saved_flags);
            return false;
        }
        if (__atomic_load_n(&dhcp_waiter, __ATOMIC_ACQUIRE) == self)
            __atomic_store_n(&dhcp_waiter, NULL, __ATOMIC_RELEASE);
        dhcp_irq_restore(saved_flags);
    }
}

static bool dhcp_exchange(net_device_t *device, u8 message_type,
                          net_ipv4_t requested, net_ipv4_t server,
                          bool include_request, bool renewal, bool rebinding,
                          bool offer, u32 timeout_ms)
{
    u64 start = timer_uptime_ms();
    u32 wait_ms = DHCP_RENEW_RETRY_MS;

    for (;;) {
        u64 elapsed = timer_uptime_ms() - start;
        u32 remaining = elapsed >= timeout_ms ? 0U :
            (u32)((timeout_ms - elapsed) > 0xffffffffULL ? 0xffffffffU :
                  timeout_ms - elapsed);
        if (!remaining || !net_device_instance_current(
                device, state.device_generation)) return false;
        if (wait_ms > remaining) wait_ms = remaining;
        dhcp_event_store(DHCP_EVENT_NONE);
        /* A unicast renewal may need an ARP resolution before the first
         * packet can leave.  Treat that as a bounded transmission miss, not
         * as a failed lease exchange: arp_request() has been issued by the
         * IPv4 layer and the next backoff attempt can use the resolved peer.
         * Broadcast discovery/rebinding follows the same bounded path if the
         * device briefly cannot transmit. */
        if (!send_message(device, message_type, requested, server,
                          include_request, renewal, rebinding)) {
            if (dhcp_wait_for_event(offer, wait_ms)) {
                return true;
            }
            if (timer_uptime_ms() - start >= timeout_ms) return false;
            if (wait_ms < 8000U) wait_ms *= 2U;
            continue;
        }
        if (wait_ms > remaining) wait_ms = remaining;
        if (dhcp_wait_for_event(offer, wait_ms)) {
            return true;
        }
        if (wait_ms < 8000U) wait_ms *= 2U;
    }
}

static void lease_copy(dhcp_lease_t *output)
{
    if (!output) return;
    *output = state.lease;
    if (ip_zero(output->netmask))
        output->netmask = (net_ipv4_t){{255, 255, 255, 255}};
}

void dhcp_init(void)
{
    state = (dhcp_state_t){0};
    __atomic_store_n(&dhcp_waiter, NULL, __ATOMIC_RELEASE);
    dhcp_event_store(DHCP_EVENT_NONE);
    (void)udp_register_handler(DHCP_CLIENT_PORT, dhcp_udp_receive);
}

void dhcp_reset(void)
{
    state = (dhcp_state_t){0};
    dhcp_event_store(DHCP_EVENT_NONE);
    dhcp_wake_waiter();
}

bool dhcp_acquire(net_device_t *device, dhcp_lease_t *lease)
{
    if (!device || !lease) return false;
    state = (dhcp_state_t){0};
    state.device = device;
    state.device_generation = device->generation;
    state.xid = (u32)timer_ticks() ^ ((u32)device->mac[4] << 8) ^
        device->mac[5] ^ 0x4d475200U;
    state.state = DHCP_STATE_SELECTING;
    if (!dhcp_exchange(device, DHCP_DISCOVER, (net_ipv4_t){{0}},
                       (net_ipv4_t){{0}}, false, false, false, true,
                       DHCP_WAIT_MS)) {
        return false;
    }
    state.state = DHCP_STATE_REQUESTING;
    if (!dhcp_exchange(device, DHCP_REQUEST, state.offered,
                       state.lease.server, true, false, false, false,
                       DHCP_WAIT_MS)) {
        return false;
    }
    if (dhcp_event_load() != DHCP_EVENT_ACK || state.state != DHCP_STATE_BOUND)
        return false;
    lease_copy(lease);
    return true;
}

bool dhcp_renew(net_device_t *device, const dhcp_lease_t *current,
                bool rebinding, dhcp_lease_t *lease, bool *rejected)
{
    if (rejected) *rejected = false;
    if (!device || !current || !lease || ip_zero(current->address)) return false;
    state = (dhcp_state_t){0};
    state.device = device;
    state.device_generation = device->generation;
    state.lease = *current;
    state.xid = (u32)timer_ticks() ^ ((u32)device->mac[4] << 8) ^
        device->mac[5] ^ 0x4d475200U;
    state.state = rebinding ? DHCP_STATE_REBINDING : DHCP_STATE_RENEWING;
    if (!dhcp_exchange(device, DHCP_REQUEST, current->address,
                       current->server, false, true, rebinding, false,
                       DHCP_RENEW_RETRY_MS * 7U)) {
        if (rejected && dhcp_event_load() == DHCP_EVENT_NAK) *rejected = true;
        return false;
    }
    if (dhcp_event_load() != DHCP_EVENT_ACK || state.state != DHCP_STATE_BOUND)
        return false;
    lease_copy(lease);
    return true;
}
