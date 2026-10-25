/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include <net/icmp.h>
#include <net/ipv4.h>
#include <spinlock.h>
#include "../../kernel/src/net/icmp.c"

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t transmit_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t transmit_condition = PTHREAD_COND_INITIALIZER;
static bool transmit_entered;
static bool transmit_release;
static ipv4_send_result_t send_result = IPV4_SEND_SUBMITTED;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    (void)lock;
    assert(pthread_mutex_lock(&state_mutex) == 0);
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)lock;
    (void)flags;
    assert(pthread_mutex_unlock(&state_mutex) == 0);
}

u16 net_checksum(const void *data, usize length)
{
    (void)data;
    (void)length;
    return 0x3412U;
}

bool net_checksum_valid(const void *data, usize length)
{
    (void)data;
    (void)length;
    return true;
}

u64 timer_uptime_ms(void)
{
    return 1234U;
}

ipv4_send_result_t ipv4_send(net_device_t *device, net_ipv4_t destination,
                             u8 protocol, const void *payload, usize length)
{
    (void)destination;
    assert(device != NULL);
    assert(protocol == IPV4_PROTOCOL_ICMP);
    assert(payload != NULL && length == 8U);
    assert(pthread_mutex_lock(&transmit_mutex) == 0);
    transmit_entered = true;
    assert(pthread_cond_broadcast(&transmit_condition) == 0);
    while (!transmit_release)
        assert(pthread_cond_wait(&transmit_condition, &transmit_mutex) == 0);
    assert(pthread_mutex_unlock(&transmit_mutex) == 0);
    return send_result;
}

bool ipv4_transmit(net_device_t *device, net_ipv4_t destination,
                   u8 protocol, const void *payload, usize length)
{
    (void)device;
    (void)destination;
    (void)protocol;
    (void)payload;
    (void)length;
    return true;
}

typedef struct {
    net_device_t device;
    net_ipv4_t destination;
    icmp_echo_request_result_t result;
} request_t;

static void *start_request(void *argument)
{
    request_t *request = argument;
    u8 payload = 0;
    request->result = icmp_echo_request(&request->device,
        request->destination, 0x1234U, 0x5678U, &payload, 0);
    return NULL;
}

int main(void)
{
    request_t request = {0};
    pthread_t thread;
    net_ipv4_t source = {{192, 0, 2, 9}};
    net_ipv4_t observed_source = {{0}};
    u8 reply[8] = {0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
    usize reply_length = 99;
    u64 received_at = 0;

    icmp_init();
    assert(pthread_create(&thread, NULL, start_request, &request) == 0);
    assert(pthread_mutex_lock(&transmit_mutex) == 0);
    while (!transmit_entered)
        assert(pthread_cond_wait(&transmit_condition, &transmit_mutex) == 0);
    assert(pthread_mutex_unlock(&transmit_mutex) == 0);

    u8 payload = 0;
    assert(icmp_echo_request(&request.device, request.destination,
                             0xabcdU, 1U, &payload, 0) ==
           ICMP_ECHO_REQUEST_FAILED);

    assert(pthread_mutex_lock(&transmit_mutex) == 0);
    transmit_release = true;
    assert(pthread_cond_broadcast(&transmit_condition) == 0);
    assert(pthread_mutex_unlock(&transmit_mutex) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(request.result == ICMP_ECHO_REQUEST_SUBMITTED);

    icmp_receive(&request.device, source, request.destination,
                 reply, sizeof(reply));
    assert(icmp_echo_reply_received(0x1234U, 0x5678U));
    assert(icmp_echo_reply_info(0x1234U, 0x5678U, &observed_source,
                                &reply_length, &received_at));
    assert(observed_source.octet[0] == 192 && observed_source.octet[3] == 9);
    assert(reply_length == 0U && received_at == 1234U);
    assert(!icmp_echo_reply_received(0x1234U, 0x5679U));

    /* A successful ARP probe is distinct from a rejected ICMP packet. Neither
       result may leave a pending echo identifier behind. */
    send_result = IPV4_SEND_NEIGHBOR_PENDING;
    assert(icmp_echo_request(&request.device, request.destination,
                             0x2345U, 1U, &payload, 0) ==
           ICMP_ECHO_REQUEST_NEIGHBOR_PENDING);
    assert(!icmp_echo_reply_received(0x2345U, 1U));
    send_result = IPV4_SEND_SUBMITTED;
    assert(icmp_echo_request(&request.device, request.destination,
                             0x2345U, 2U, &payload, 0) ==
           ICMP_ECHO_REQUEST_SUBMITTED);
    u8 retry_reply[8] = {0, 0, 0, 0, 0x23, 0x45, 0, 2};
    icmp_receive(&request.device, source, request.destination,
                 retry_reply, sizeof(retry_reply));
    assert(icmp_echo_reply_received(0x2345U, 2U));
    send_result = IPV4_SEND_FAILED;
    assert(icmp_echo_request(&request.device, request.destination,
                             0x3456U, 1U, &payload, 0) ==
           ICMP_ECHO_REQUEST_FAILED);
    puts("ICMP request ownership and reply publication tests passed");
    return 0;
}
