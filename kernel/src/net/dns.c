#include <net/arp.h>
#include <net/dns.h>
#include <net/ipv4.h>
#include <net/udp.h>
#include <scheduler.h>
#include <timer.h>

#define DNS_PORT 53U
#define DNS_SOURCE_PORT 53000U
#define DNS_PACKET_MAX 512U
#define DNS_WAIT_TICKS 5000U
#define DNS_RETRIES 3U

typedef struct {
    net_device_t *device;
    u16 id;
    volatile u32 event;
    volatile dns_status_t status;
    volatile net_ipv4_t address;
    volatile u32 answers;
} dns_state_t;

static dns_state_t state;

static u16 dns_read16(const u8 *p)
{
    return ((u16)p[0] << 8) | p[1];
}

static void dns_write16(u8 *p, u16 value)
{
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static u16 dns_next_id(void)
{
    static u16 sequence;
    return (u16)(timer_ticks() + ++sequence);
}

static void dns_udp_receive(net_device_t *device, net_ipv4_t source,
                            u16 source_port, net_ipv4_t destination,
                            u16 destination_port, const u8 *payload,
                            usize length)
{
    net_ipv4_t answer;
    u32 answers = 0;
    dns_status_t status;
    const net_config_t *configuration = net_config();
    if (!device || source_port != DNS_PORT || destination_port != DNS_SOURCE_PORT ||
        !configuration->has_dns || !net_ipv4_equal(source, configuration->dns) ||
        !net_ipv4_equal(destination, configuration->address)) return;
    status = dns_parse_response(payload, length, state.id, &answer, &answers);
    if (status == DNS_STATUS_SUCCESS) {
        state.address = answer;
        state.answers = answers;
    }
    state.status = status;
    __atomic_store_n(&state.event, 1, __ATOMIC_RELEASE);
}

void dns_init(void)
{
    state = (dns_state_t){0};
    (void)udp_register_handler(DNS_SOURCE_PORT, dns_udp_receive);
}

void dns_reset(void)
{
    state = (dns_state_t){0};
}

bool dns_resolve_a(net_device_t *device, const char *hostname,
                   net_ipv4_t *address_out, dns_status_t *status_out)
{
    const net_config_t *configuration = net_config();
    u8 packet[DNS_PACKET_MAX];
    usize name_length, query_length;
    u16 id;
    u32 attempt;
    dns_status_t status = DNS_STATUS_UNCONFIGURED;
    if (status_out) *status_out = status;
    if (!device || !hostname || !address_out || !configuration->configured ||
        !configuration->has_dns) return false;
    if (!dns_encode_hostname(hostname, packet + 12, sizeof(packet) - 12, &name_length)) {
        if (status_out) *status_out = DNS_STATUS_INVALID;
        return false;
    }
    query_length = 12 + name_length + 4;
    dns_write16(packet + 0, (id = dns_next_id()));
    dns_write16(packet + 2, 0x0100); /* RD */
    dns_write16(packet + 4, 1);
    dns_write16(packet + 6, 0);
    dns_write16(packet + 8, 0);
    dns_write16(packet + 10, 0);
    dns_write16(packet + 12 + name_length, 1); /* A */
    dns_write16(packet + 14 + name_length, 1); /* IN */
    state.device = device;
    state.id = id;
    state.status = DNS_STATUS_TIMEOUT;
    state.answers = 0;
    __atomic_store_n(&state.event, 0, __ATOMIC_RELEASE);
    for (attempt = 0; attempt < DNS_RETRIES; attempt++) {
        u64 start;
        if (attempt) {
            id = dns_next_id();
            state.id = id;
            dns_write16(packet, id);
            __atomic_store_n(&state.event, 0, __ATOMIC_RELEASE);
        }
        (void)udp_transmit(device, configuration->address, configuration->dns,
                           DNS_SOURCE_PORT, DNS_PORT, packet, query_length);
        start = timer_ticks();
        while (timer_ticks() - start < DNS_WAIT_TICKS &&
               __atomic_load_n(&state.event, __ATOMIC_ACQUIRE) == 0) {
            /* DNS can run beneath a syscall, whose entry path masks IF. */
            (void)scheduler_sleep(1);
        }
        if (__atomic_load_n(&state.event, __ATOMIC_ACQUIRE)) {
            status = state.status;
            if (status == DNS_STATUS_SUCCESS) {
                *address_out = state.address;
                if (status_out) *status_out = status;
                return true;
            }
            if (status == DNS_STATUS_NXDOMAIN || status == DNS_STATUS_SERVFAIL ||
                status == DNS_STATUS_INVALID || status == DNS_STATUS_NO_ANSWER) break;
        }
    }
    if (status_out) *status_out = status;
    return false;
}
