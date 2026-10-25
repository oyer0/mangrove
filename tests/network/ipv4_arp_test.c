/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include <net/arp.h>
#include <net/checksum.h>
#include <net/ethernet.h>
#include <net/ipv4.h>
#include <spinlock.h>

#include "../../kernel/src/net/arp.c"
#include "../../kernel/src/net/ipv4.c"

static const net_ipv4_t local_address = {{10, 0, 2, 15}};
static const net_ipv4_t subnet_mask = {{255, 255, 255, 0}};
static const net_ipv4_t default_gateway = {{10, 0, 2, 2}};
static bool configured = true;
static bool gateway_present = true;
static bool ethernet_succeeds = true;
static const u8 gateway_mac[6] = {0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc};
static pthread_mutex_t arp_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static u8 ethernet_destination[6];
static u8 ethernet_payload[NET_ETHERNET_MAX_FRAME];
static usize ethernet_payload_length;
static u16 ethernet_type;
static u32 ethernet_transmit_count;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock != NULL);
    assert(pthread_mutex_lock(&arp_cache_mutex) == 0);
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(lock != NULL);
    (void)flags;
    assert(pthread_mutex_unlock(&arp_cache_mutex) == 0);
}

const net_ipv4_t *net_local_ipv4(void) { return &local_address; }
const net_ipv4_t *net_netmask_ipv4(void) { return &subnet_mask; }
const net_ipv4_t *net_gateway_ipv4(void)
{
    return gateway_present ? &default_gateway : NULL;
}
bool net_network_configured(void) { return configured; }

bool net_ipv4_equal(net_ipv4_t left, net_ipv4_t right)
{
    for (u32 i = 0; i < 4; i++)
        if (left.octet[i] != right.octet[i]) return false;
    return true;
}

void net_mac_copy(u8 destination[6], const u8 source[6])
{
    for (u32 i = 0; i < 6; i++) destination[i] = source[i];
}

bool ethernet_transmit(net_device_t *device, const u8 destination[6],
                       u16 type, const void *payload, usize length)
{
    assert(device != NULL && destination != NULL && payload != NULL);
    assert(length <= sizeof(ethernet_payload));
    for (u32 i = 0; i < 6; i++) ethernet_destination[i] = destination[i];
    ethernet_type = type;
    ethernet_payload_length = length;
    for (usize i = 0; i < length; i++)
        ethernet_payload[i] = ((const u8 *)payload)[i];
    ethernet_transmit_count++;
    return ethernet_succeeds;
}

u16 net_checksum(const void *data, usize length)
{
    (void)data;
    (void)length;
    return 0x1234U;
}

bool net_checksum_valid(const void *data, usize length)
{
    (void)data;
    (void)length;
    return true;
}

static void deliver_gateway_arp_reply(net_device_t *device)
{
    u8 reply[28] = {
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x02,
        0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc,
        10, 0, 2, 2,
        0, 0, 0, 0, 0, 0,
        10, 0, 2, 15,
    };
    arp_receive(device, reply, sizeof(reply));
}

typedef struct {
    net_device_t *device;
    net_ipv4_t address;
} arp_thread_context_t;

static void *publish_gateway_replies(void *argument)
{
    arp_thread_context_t *context = argument;
    for (u32 index = 0; index < 2000U; index++)
        deliver_gateway_arp_reply(context->device);
    return NULL;
}

static void *read_gateway_neighbors(void *argument)
{
    arp_thread_context_t *context = argument;
    arp_snapshot_entry_t entries[ARP_CACHE_CAPACITY];

    for (u32 index = 0; index < 2000U; index++) {
        u8 mac[6];
        if (arp_lookup(&context->address, mac))
            assert(mac[0] == gateway_mac[0] && mac[5] == gateway_mac[5]);
        (void)ipv4_neighbor_resolved(context->address);
        (void)arp_snapshot(entries, ARP_CACHE_CAPACITY);
    }
    return NULL;
}

int main(void)
{
    net_device_t device = {
        .mtu = 1500U,
        .mac = {0x52, 0x54, 0x00, 0x01, 0x02, 0x03},
    };
    net_ipv4_t gateway_echo = {{10, 0, 2, 2}};
    net_ipv4_t external = {{198, 51, 100, 8}};
    u8 payload[] = {0x08, 0x00, 0x12, 0x34};
    arp_thread_context_t context = {&device, gateway_echo};
    pthread_t publisher, reader;

    /* An unknown next hop is a transmitted ARP request, not a failed IPv4
       submission. The pending ICMP datagram itself is never transmitted. */
    arp_init();
    assert(ipv4_send(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) ==
           IPV4_SEND_NEIGHBOR_PENDING);
    assert(ethernet_transmit_count == 1U);
    assert(ethernet_type == ETHERNET_ETHERTYPE_ARP);
    assert(ethernet_payload_length == 28U);
    assert(ethernet_destination[0] == 0xffU &&
           ethernet_destination[5] == 0xffU);
    assert(ethernet_payload[6] == 0 && ethernet_payload[7] == 1U);
    assert(ethernet_payload[24] == 10U && ethernet_payload[27] == 2U);
    assert(!ipv4_neighbor_resolved(gateway_echo));

    /* Once the real ARP receive path publishes the gateway mapping, the
       same-subnet destination is sent directly to that hardware address. */
    deliver_gateway_arp_reply(&device);
    assert(ipv4_neighbor_resolved(gateway_echo));
    assert(ipv4_send(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) == IPV4_SEND_SUBMITTED);
    assert(ethernet_transmit_count == 2U);
    assert(ethernet_type == ETHERNET_ETHERTYPE_IPV4);
    assert(ethernet_destination[0] == gateway_mac[0] &&
           ethernet_destination[5] == gateway_mac[5]);
    assert(ethernet_payload_length == 24U);
    assert(ethernet_payload[9] == IPV4_PROTOCOL_ICMP);
    assert(ethernet_payload[12] == 10U && ethernet_payload[15] == 15U);
    assert(ethernet_payload[16] == 10U && ethernet_payload[19] == 2U);

    /* An off-subnet destination resolves the configured gateway while the
       IPv4 header retains the original remote destination. */
    arp_clear_cache();
    assert(ipv4_send(&device, external, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) ==
           IPV4_SEND_NEIGHBOR_PENDING);
    assert(ethernet_type == ETHERNET_ETHERTYPE_ARP);
    assert(ethernet_payload[24] == default_gateway.octet[0] &&
           ethernet_payload[27] == default_gateway.octet[3]);
    deliver_gateway_arp_reply(&device);
    assert(ipv4_send(&device, external, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) == IPV4_SEND_SUBMITTED);
    assert(ethernet_payload[16] == 198U && ethernet_payload[19] == 8U);

    /* A failed ARP request, a failed driver submission, or absent network
       configuration remains a hard failure rather than a pending neighbor. */
    arp_clear_cache();
    ethernet_succeeds = false;
    assert(ipv4_send(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) == IPV4_SEND_FAILED);
    ethernet_succeeds = true;
    arp_clear_cache();
    deliver_gateway_arp_reply(&device);
    ethernet_succeeds = false;
    assert(ipv4_send(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) == IPV4_SEND_FAILED);
    ethernet_succeeds = true;
    configured = false;
    assert(ipv4_send(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                     payload, sizeof(payload)) == IPV4_SEND_FAILED);

    /* The bool compatibility wrappers retain their old meaning: they are
       true only when the IPv4 packet itself reached the device. */
    configured = true;
    ethernet_succeeds = true;
    arp_clear_cache();
    assert(!ipv4_transmit(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                          payload, sizeof(payload)));
    deliver_gateway_arp_reply(&device);
    assert(ipv4_transmit(&device, gateway_echo, IPV4_PROTOCOL_ICMP,
                         payload, sizeof(payload)));

    /* ARP reply publication runs from the device receive context while
       process-context route checks read the same cache on another CPU. */
    assert(pthread_create(&publisher, NULL, publish_gateway_replies,
                          &context) == 0);
    assert(pthread_create(&reader, NULL, read_gateway_neighbors,
                          &context) == 0);
    assert(pthread_join(publisher, NULL) == 0);
    assert(pthread_join(reader, NULL) == 0);
    assert(ipv4_neighbor_resolved(gateway_echo));
    puts("IPv4 route selection and ARP-pending submission tests passed");
    return 0;
}
