/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <mg/net.h>
#include <net/icmp.h>
#include <net/net.h>

#include <spinlock.h>
#include "../../kernel/src/net/user.c"

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t echo_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t echo_condition = PTHREAD_COND_INITIALIZER;
static net_device_t echo_device = {.generation = 1};
static atomic_uint_fast64_t echo_clock = 100;
static unsigned echo_requests;
static bool echo_info_entered;
static bool echo_info_release;
static bool echo_neighbor_pending_once;
static bool echo_neighbor_available;
static bool resolve_neighbor_on_sleep;
static bool invalidate_device_on_sleep;
static bool echo_request_fail_once;
static bool echo_reply_immediate;
static unsigned echo_sleep_calls;
static u64 echo_reply_timestamp;
static bool echo_wire_pending;
static u16 echo_pending_identifier;
static u16 echo_pending_sequence;
static unsigned echo_ipv4_submissions;
static unsigned echo_cancel_calls;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    (void)lock;
    assert(pthread_mutex_lock(&table_mutex) == 0);
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)lock;
    (void)flags;
    assert(pthread_mutex_unlock(&table_mutex) == 0);
}

net_device_t *net_primary_device(void)
{
    return &echo_device;
}

bool net_device_instance_current(const net_device_t *device, u64 generation)
{
    return device == &echo_device && generation == echo_device.generation;
}

bool ipv4_neighbor_resolved(net_ipv4_t destination)
{
    (void)destination;
    return echo_neighbor_available;
}

u64 timer_uptime_ms(void)
{
    return atomic_load_explicit(&echo_clock, memory_order_relaxed);
}

bool scheduler_sleep(u64 ticks)
{
    atomic_fetch_add_explicit(&echo_clock, ticks, memory_order_relaxed);
    echo_sleep_calls++;
    if (resolve_neighbor_on_sleep) echo_neighbor_available = true;
    if (invalidate_device_on_sleep) echo_device.generation++;
    return true;
}

icmp_echo_request_result_t icmp_echo_request(
    net_device_t *device, net_ipv4_t destination, u16 identifier,
    u16 sequence, const void *payload, usize length)
{
    (void)destination;
    (void)payload;
    (void)length;
    assert(device == &echo_device);
    __atomic_add_fetch(&echo_requests, 1U, __ATOMIC_RELAXED);
    if (echo_wire_pending)
        return ICMP_ECHO_REQUEST_FAILED;
    if (echo_request_fail_once) {
        echo_request_fail_once = false;
        return ICMP_ECHO_REQUEST_FAILED;
    }
    if (echo_neighbor_pending_once) {
        echo_neighbor_pending_once = false;
        return ICMP_ECHO_REQUEST_NEIGHBOR_PENDING;
    }
    echo_wire_pending = true;
    echo_pending_identifier = identifier;
    echo_pending_sequence = sequence;
    echo_ipv4_submissions++;
    if (echo_reply_immediate) {
        echo_reply_timestamp = timer_uptime_ms() + 2U;
        atomic_fetch_add_explicit(&echo_clock, 2U, memory_order_relaxed);
    }
    return ICMP_ECHO_REQUEST_SUBMITTED;
}

void icmp_echo_cancel(u16 identifier, u16 sequence)
{
    echo_cancel_calls++;
    if (echo_wire_pending && echo_pending_identifier == identifier &&
        echo_pending_sequence == sequence)
        echo_wire_pending = false;
}

bool icmp_echo_reply_info(u16 identifier, u16 sequence, net_ipv4_t *source,
                          usize *reply_length, u64 *received_at_ms)
{
    if (echo_reply_immediate) {
        if (echo_wire_pending && echo_pending_identifier == identifier &&
            echo_pending_sequence == sequence)
            echo_wire_pending = false;
        if (source) *source = (net_ipv4_t){{10, 0, 2, 2}};
        if (reply_length) *reply_length = 1;
        if (received_at_ms) *received_at_ms = echo_reply_timestamp;
        return true;
    }
    assert(pthread_mutex_lock(&echo_mutex) == 0);
    echo_info_entered = true;
    assert(pthread_cond_broadcast(&echo_condition) == 0);
    while (!echo_info_release)
        assert(pthread_cond_wait(&echo_condition, &echo_mutex) == 0);
    assert(pthread_mutex_unlock(&echo_mutex) == 0);
    return false;
}

typedef struct {
    u32 slot;
    u16 port;
    int result;
} reservation_t;

static void *reserve_automatic(void *argument)
{
    reservation_t *reservation = argument;
    reservation->result = datagram_reserve_slot(
        0, &reservation->slot, &reservation->port);
    return NULL;
}

typedef struct {
    net_icmp_object_t object;
    mg_ipv4_addr_t destination;
    mg_icmp_echo_result_t result;
    i64 status;
} echo_call_t;

static void *run_echo(void *argument)
{
    echo_call_t *call = argument;
    call->status = net_user_icmp_echo(&call->object.object,
                                      &call->destination, "x", 1, 100,
                                      &call->result);
    return NULL;
}

int main(void)
{
    reservation_t a = {0}, b = {0};
    pthread_t first, second;
    pthread_t echo_thread;
    echo_call_t echo = {0}, competing = {0};
    u32 held_slots[4];
    u16 held_ports[4];
    u32 slot;
    u16 port;

    assert(datagram_reserve_slot(40000U, &slot, &port) == 1);
    assert(port == 40000U);
    u32 held_slot = slot;
    assert(datagram_reserve_slot(40000U, &slot, &port) == -1);
    datagram_release_reservation(held_slot, 40000U);

    assert(pthread_create(&first, NULL, reserve_automatic, &a) == 0);
    assert(pthread_create(&second, NULL, reserve_automatic, &b) == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert(a.result == 1 && b.result == 1);
    assert(a.slot != b.slot && a.port != b.port);
    datagram_release_reservation(a.slot, a.port);
    datagram_release_reservation(b.slot, b.port);

    for (u16 index = 0; index < 4U; index++) {
        assert(datagram_reserve_slot((u16)(41000U + index),
                                    &held_slots[index],
                                    &held_ports[index]) == 1);
    }
    assert(datagram_reserve_slot(42000U, &slot, &port) == 0);
    for (u16 index = 0; index < 4U; index++)
        datagram_release_reservation(held_slots[index], held_ports[index]);

    echo.object.object.type = OBJECT_TYPE_NETWORK_ICMP;
    echo.object.identifier = 7;
    competing.object.object.type = OBJECT_TYPE_NETWORK_ICMP;
    competing.object.identifier = 8;
    assert(pthread_create(&echo_thread, NULL, run_echo, &echo) == 0);
    assert(pthread_mutex_lock(&echo_mutex) == 0);
    while (!echo_info_entered)
        assert(pthread_cond_wait(&echo_condition, &echo_mutex) == 0);
    assert(pthread_mutex_unlock(&echo_mutex) == 0);
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 100,
                              &competing.result) == MG_ERR_BUSY);
    assert(__atomic_load_n(&echo_requests, __ATOMIC_RELAXED) == 1U);
    assert(pthread_mutex_lock(&echo_mutex) == 0);
    echo_info_release = true;
    assert(pthread_cond_broadcast(&echo_condition) == 0);
    assert(pthread_mutex_unlock(&echo_mutex) == 0);
    assert(pthread_join(echo_thread, NULL) == 0);
    assert(echo.status == MG_ERR_TIMEOUT);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));
    /* A submitted request that times out must retire its global wire slot.
       Otherwise every later ICMP request is rejected before IPv4 submission. */
    assert(echo_cancel_calls == 1U && !echo_wire_pending);
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 0,
                              &competing.result) == MG_ERR_TIMEOUT);
    assert(__atomic_load_n(&echo_requests, __ATOMIC_RELAXED) == 2U);
    assert(echo_cancel_calls == 2U && !echo_wire_pending);
    assert(echo_ipv4_submissions == 2U);

    /* The first cache miss queues ARP. The same API call waits for neighbor
       publication, submits exactly one echo, and returns the reply. */
    echo_requests = 0;
    echo_neighbor_pending_once = true;
    echo_neighbor_available = false;
    resolve_neighbor_on_sleep = true;
    invalidate_device_on_sleep = false;
    echo_reply_immediate = true;
    echo_sleep_calls = 0;
    atomic_store_explicit(&echo_clock, 100, memory_order_relaxed);
    competing.status = net_user_icmp_echo(&competing.object.object,
                                          &competing.destination, "y", 1,
                                          100, &competing.result);
    assert(competing.status == MG_OK);
    assert(echo_requests == 2U && echo_sleep_calls == 1U);
    assert(competing.result.source.octet[3] == 2U);
    assert(competing.result.rtt_ms == 2U);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));

    /* Immediate mode reports ARP work as WOULD_BLOCK; a missing response
       within a finite timeout reports TIMEOUT, not network unavailable. */
    echo_requests = 0;
    echo_neighbor_pending_once = true;
    echo_neighbor_available = false;
    resolve_neighbor_on_sleep = false;
    echo_sleep_calls = 0;
    atomic_store_explicit(&echo_clock, 100, memory_order_relaxed);
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 0,
                              &competing.result) == MG_ERR_WOULD_BLOCK);
    assert(echo_requests == 1U && echo_sleep_calls == 0U);
    echo_requests = 0;
    echo_neighbor_pending_once = true;
    atomic_store_explicit(&echo_clock, 100, memory_order_relaxed);
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 2,
                              &competing.result) == MG_ERR_TIMEOUT);
    assert(echo_requests == 1U && echo_sleep_calls == 2U);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));

    /* Device-generation changes during neighbor wait retain the existing
       teardown result and release the one-echo admission slot. */
    echo_requests = 0;
    echo_neighbor_pending_once = true;
    echo_neighbor_available = false;
    invalidate_device_on_sleep = true;
    echo_sleep_calls = 0;
    atomic_store_explicit(&echo_clock, 100, memory_order_relaxed);
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 100,
                              &competing.result) == MG_ERR_NETWORK_UNAVAILABLE);
    assert(echo_requests == 1U && echo_sleep_calls == 1U);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));

    /* If the device generation changes after an echo was submitted, retire
       that exact wire request before returning the lifecycle error. */
    echo_requests = 0;
    echo_ipv4_submissions = 0;
    echo_neighbor_pending_once = false;
    echo_neighbor_available = true;
    resolve_neighbor_on_sleep = false;
    invalidate_device_on_sleep = true;
    echo_reply_immediate = false;
    echo_sleep_calls = 0;
    atomic_store_explicit(&echo_clock, 100, memory_order_relaxed);
    unsigned cancels_before_device_change = echo_cancel_calls;
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 100,
                              &competing.result) == MG_ERR_NETWORK_UNAVAILABLE);
    assert(echo_requests == 1U && echo_ipv4_submissions == 1U);
    assert(echo_cancel_calls == cancels_before_device_change + 1U);
    assert(!echo_wire_pending);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));

    /* A hard packet-submission failure remains an unavailable-network error,
       releases admission state, and does not poison the next request. */
    echo_requests = 0;
    echo_request_fail_once = true;
    echo_neighbor_available = true;
    echo_reply_immediate = true;
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 100,
                              &competing.result) ==
           MG_ERR_NETWORK_UNAVAILABLE);
    assert(echo_requests == 1U);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 100,
                              &competing.result) == MG_OK);
    assert(echo_requests == 2U);
    assert(!__atomic_load_n(&icmp_echo_active, __ATOMIC_ACQUIRE));
    puts("native network object admission and port ownership tests passed");
    return 0;
}
