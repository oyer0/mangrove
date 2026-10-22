/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <net/ipv4.h>
#include <net/checksum.h>
#include <net/udp.h>
#include <spinlock.h>

#include "../../kernel/src/net/udp.c"

static pthread_mutex_t udp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local bool unregister_thread;
static bool receive_entered, release_receive, unregister_waiting;
static _Atomic bool unregister_done;
static _Atomic unsigned calls;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    (void)lock;
    if (unregister_thread) {
        pthread_mutex_lock(&control_mutex);
        unregister_waiting = true;
        pthread_cond_broadcast(&control_changed);
        pthread_mutex_unlock(&control_mutex);
    }
    assert(pthread_mutex_lock(&udp_mutex) == 0);
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)lock;
    (void)flags;
    assert(pthread_mutex_unlock(&udp_mutex) == 0);
}

u16 net_checksum(const void *data, usize length)
{
    (void)data;
    (void)length;
    return 0;
}

bool ipv4_transmit_from(net_device_t *device, net_ipv4_t source,
                        net_ipv4_t destination, u8 protocol,
                        const void *payload, usize length)
{
    (void)device;
    (void)source;
    (void)destination;
    (void)protocol;
    (void)payload;
    (void)length;
    return false;
}

static void noop_handler(net_device_t *device, net_ipv4_t source,
                         u16 source_port, net_ipv4_t destination,
                         u16 destination_port, const u8 *payload,
                         usize length)
{
    (void)device;
    (void)source;
    (void)source_port;
    (void)destination;
    (void)destination_port;
    (void)payload;
    (void)length;
}

static void paused_handler(net_device_t *device, net_ipv4_t source,
                           u16 source_port, net_ipv4_t destination,
                           u16 destination_port, const u8 *payload,
                           usize length)
{
    (void)device;
    (void)source;
    (void)source_port;
    (void)destination;
    (void)destination_port;
    (void)payload;
    (void)length;
    atomic_fetch_add_explicit(&calls, 1U, memory_order_relaxed);
    pthread_mutex_lock(&control_mutex);
    receive_entered = true;
    pthread_cond_broadcast(&control_changed);
    while (!release_receive)
        pthread_cond_wait(&control_changed, &control_mutex);
    pthread_mutex_unlock(&control_mutex);
}

typedef struct { udp_register_result_t result; } register_call_t;
static _Atomic bool start_race;

static void *register_same_port(void *argument)
{
    register_call_t *call = argument;
    while (!atomic_load_explicit(&start_race, memory_order_acquire)) { }
    call->result = udp_register_handler(0x4321U, noop_handler);
    return NULL;
}

static void *receive_packet(void *unused)
{
    u8 packet[UDP_HEADER_SIZE] = {0x00, 0x35, 0x12, 0x34,
                                  0x00, UDP_HEADER_SIZE, 0x00, 0x00};
    net_device_t device = {0};
    (void)unused;
    udp_receive(&device, (net_ipv4_t){0}, (net_ipv4_t){0}, packet,
                sizeof(packet));
    return NULL;
}

static void *unregister_paused(void *unused)
{
    (void)unused;
    unregister_thread = true;
    assert(udp_unregister_handler(0x1234U, paused_handler));
    atomic_store_explicit(&unregister_done, true, memory_order_release);
    return NULL;
}

int main(void)
{
    pthread_t first, second, receiver, unregisterer;
    register_call_t a = {0}, b = {0};

    udp_init();
    assert(udp_register_handler(0x1234U, paused_handler) == UDP_REGISTERED);
    assert(udp_register_handler(0x1234U, noop_handler) == UDP_REGISTER_PORT_IN_USE);

    assert(pthread_create(&first, NULL, register_same_port, &a) == 0);
    assert(pthread_create(&second, NULL, register_same_port, &b) == 0);
    atomic_store_explicit(&start_race, true, memory_order_release);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert((a.result == UDP_REGISTERED && b.result == UDP_REGISTER_PORT_IN_USE) ||
           (b.result == UDP_REGISTERED && a.result == UDP_REGISTER_PORT_IN_USE));
    assert(udp_unregister_handler(0x4321U, noop_handler));

    assert(pthread_create(&receiver, NULL, receive_packet, NULL) == 0);
    pthread_mutex_lock(&control_mutex);
    while (!receive_entered)
        pthread_cond_wait(&control_changed, &control_mutex);
    pthread_mutex_unlock(&control_mutex);

    assert(pthread_create(&unregisterer, NULL, unregister_paused, NULL) == 0);
    pthread_mutex_lock(&control_mutex);
    while (!unregister_waiting)
        pthread_cond_wait(&control_changed, &control_mutex);
    pthread_mutex_unlock(&control_mutex);
    assert(!atomic_load_explicit(&unregister_done, memory_order_acquire));

    pthread_mutex_lock(&control_mutex);
    release_receive = true;
    pthread_cond_broadcast(&control_changed);
    pthread_mutex_unlock(&control_mutex);
    assert(pthread_join(receiver, NULL) == 0);
    assert(pthread_join(unregisterer, NULL) == 0);
    assert(atomic_load_explicit(&unregister_done, memory_order_acquire));
    assert(atomic_load_explicit(&calls, memory_order_relaxed) == 1U);

    /* Once unregister returns, no stale callback can run after its owner frees. */
    receive_entered = false;
    assert(pthread_create(&receiver, NULL, receive_packet, NULL) == 0);
    assert(pthread_join(receiver, NULL) == 0);
    assert(atomic_load_explicit(&calls, memory_order_relaxed) == 1U);
    return 0;
}
