/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <mg/net.h>
#include <net/icmp.h>
#include <net/net.h>

#include <spinlock.h>
#include "../kernel/src/net/user.c"

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t echo_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t echo_condition = PTHREAD_COND_INITIALIZER;
static net_device_t echo_device = {.generation = 1};
static atomic_uint_fast64_t echo_clock = 100;
static unsigned echo_requests;
static bool echo_info_entered;
static bool echo_info_release;

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

u64 timer_uptime_ms(void)
{
    return atomic_load_explicit(&echo_clock, memory_order_relaxed);
}

bool scheduler_sleep(u64 ticks)
{
    atomic_store_explicit(&echo_clock, 1000 + ticks, memory_order_relaxed);
    return true;
}

bool icmp_echo_request(net_device_t *device, net_ipv4_t destination,
                       u16 identifier, u16 sequence,
                       const void *payload, usize length)
{
    (void)destination;
    (void)identifier;
    (void)sequence;
    (void)payload;
    (void)length;
    assert(device == &echo_device);
    __atomic_add_fetch(&echo_requests, 1U, __ATOMIC_RELAXED);
    return true;
}

bool icmp_echo_reply_info(u16 identifier, u16 sequence, net_ipv4_t *source,
                          usize *reply_length, u64 *received_at_ms)
{
    (void)identifier;
    (void)sequence;
    (void)source;
    (void)reply_length;
    (void)received_at_ms;
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
    assert(net_user_icmp_echo(&competing.object.object,
                              &competing.destination, "y", 1, 0,
                              &competing.result) == MG_ERR_TIMEOUT);
    assert(__atomic_load_n(&echo_requests, __ATOMIC_RELAXED) == 2U);
    puts("native network object admission and port ownership tests passed");
    return 0;
}
