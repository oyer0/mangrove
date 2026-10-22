/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../kernel/src/net/e1000.c"

#define WORKER_COUNT 8U
#define SENDS_PER_WORKER 8U

static u8 mmio[E1000_MMIO_SIZE];
static e1000_tx_descriptor_t tx_ring[E1000_TX_RING_COUNT];
static u8 tx_buffers[E1000_TX_RING_COUNT][E1000_BUFFER_SIZE];
static volatile u32 start_sends;
static volatile u32 successful_sends;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

static void *send_worker(void *argument)
{
    u8 tag = (u8)(uintptr_t)argument;
    u8 frame[NET_ETHERNET_HEADER_SIZE];

    memset(frame, tag, sizeof(frame));
    while (!__atomic_load_n(&start_sends, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    for (u32 index = 0; index < SENDS_PER_WORKER; index++) {
        if (e1000_transmit(&controllers[0].device, frame, sizeof(frame)))
            __atomic_add_fetch(&successful_sends, 1U, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void)
{
    pthread_t workers[WORKER_COUNT];
    u32 tag_counts[WORKER_COUNT + 1U] = {0};

    memset(controllers, 0, sizeof(controllers));
    memset(mmio, 0, sizeof(mmio));
    memset(tx_ring, 0, sizeof(tx_ring));
    memset(tx_buffers, 0, sizeof(tx_buffers));
    controllers[0].active = true;
    controllers[0].mmio = mmio;
    controllers[0].tx_ring = tx_ring;
    controllers[0].tx_lock.value = 0;
    controllers[0].device.driver_data = &controllers[0];
    for (u32 index = 0; index < E1000_TX_RING_COUNT; index++) {
        controllers[0].tx_buffer[index] = tx_buffers[index];
        tx_ring[index].status = E1000_TX_STATUS_DD;
    }

    for (u32 index = 0; index < WORKER_COUNT; index++)
        assert(pthread_create(&workers[index], NULL, send_worker,
                              (void *)(uintptr_t)(index + 1U)) == 0);
    __atomic_store_n(&start_sends, 1U, __ATOMIC_RELEASE);
    for (u32 index = 0; index < WORKER_COUNT; index++)
        assert(pthread_join(workers[index], NULL) == 0);

    assert(successful_sends == E1000_TX_RING_COUNT);
    assert(controllers[0].tx_producer == 0U);
    assert(controllers[0].tx_consumer == 0U);
    for (u32 index = 0; index < E1000_TX_RING_COUNT; index++) {
        u8 tag = tx_buffers[index][0];
        assert(tag > 0U && tag <= WORKER_COUNT);
        assert(tx_ring[index].status == 0U);
        tag_counts[tag]++;
        for (u32 byte = 1; byte < NET_ETHERNET_HEADER_SIZE; byte++)
            assert(tx_buffers[index][byte] == tag);
    }
    for (u32 tag = 1; tag <= WORKER_COUNT; tag++)
        assert(tag_counts[tag] == SENDS_PER_WORKER);

    puts("e1000 concurrent transmit owns each descriptor exactly once");
    return 0;
}
