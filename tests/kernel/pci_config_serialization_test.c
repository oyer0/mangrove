/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <spinlock.h>
#include <pci.h>

#define WORKERS 4U
#define READS_PER_WORKER 12000U
#define PCI_ADDRESS_PORT 0xCF8U
#define PCI_DATA_PORT 0xCFCU

static volatile u32 current_config_address;
static volatile u16 command_words[65536];
static volatile u32 mismatches;

static u32 config_address(const pci_device_t *device, u8 offset)
{
    return (1U << 31) | ((u32)device->bus << 16) |
           ((u32)device->device << 11) |
           ((u32)device->function << 8) | (offset & 0xFCU);
}

static u32 observed_value(u32 address)
{
    u32 bus = (address >> 16) & 0xffU;
    u32 device = (address >> 11) & 0x1fU;
    u32 function = (address >> 8) & 0x07U;
    u32 offset = address & 0xfcU;
    u32 bdf = (bus << 8) | (device << 3) | function;

    if (offset == 0x04U)
        return command_words[bdf];
    return 0xa5000000U | (bus << 16) | (device << 11) |
           (function << 8) | offset;
}

u32 pci_test_inl(u16 port)
{
    assert(port == PCI_DATA_PORT);
    return observed_value(__atomic_load_n(&current_config_address,
                                          __ATOMIC_RELAXED));
}

void pci_test_outl(u16 port, u32 value)
{
    if (port == PCI_ADDRESS_PORT) {
        __atomic_store_n(&current_config_address, value, __ATOMIC_RELAXED);
        /* Force a competing CPU between the address and data port accesses. */
        sched_yield();
        return;
    }

    assert(port == PCI_DATA_PORT);
    u32 address = __atomic_load_n(&current_config_address, __ATOMIC_RELAXED);
    u32 bus = (address >> 16) & 0xffU;
    u32 device = (address >> 11) & 0x1fU;
    u32 function = (address >> 8) & 0x07U;
    u32 offset = address & 0xfcU;
    u32 bdf = (bus << 8) | (device << 3) | function;

    if (offset == 0x04U)
        __atomic_store_n(&command_words[bdf], (u16)value,
                         __ATOMIC_RELAXED);
}

u64 spin_lock_irqsave(spinlock_t *lock)
{
    u32 expected;

    do {
        expected = 0U;
        if (__atomic_compare_exchange_n(&lock->value, &expected, 1U, false,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return 0U;
        sched_yield();
    } while (true);
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

static void *read_worker(void *argument)
{
    pci_device_t *device = argument;
    u32 expected = observed_value(config_address(device, 0x40U));

    for (u32 iteration = 0; iteration < READS_PER_WORKER; iteration++) {
        if (pci_read_config32(device, 0x40U) != expected)
            __atomic_add_fetch(&mismatches, 1U, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void *unsafe_read_worker(void *argument)
{
    pci_device_t *device = argument;
    u32 expected_address = config_address(device, 0x40U);
    u32 expected = observed_value(expected_address);

    for (u32 iteration = 0; iteration < READS_PER_WORKER; iteration++) {
        pci_test_outl(PCI_ADDRESS_PORT, expected_address);
        if (pci_test_inl(PCI_DATA_PORT) != expected)
            __atomic_add_fetch(&mismatches, 1U, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void run_workers(pthread_t threads[WORKERS],
                        pci_device_t devices[WORKERS],
                        void *(*worker)(void *))
{
    for (u32 index = 0; index < WORKERS; index++)
        assert(pthread_create(&threads[index], NULL, worker,
                              &devices[index]) == 0);
    for (u32 index = 0; index < WORKERS; index++)
        assert(pthread_join(threads[index], NULL) == 0);
}

int main(void)
{
    pthread_t threads[WORKERS];
    pci_device_t devices[WORKERS] = {0};

    for (u32 index = 0; index < WORKERS; index++) {
        devices[index].bus = (u8)(index + 1U);
        devices[index].device = (u8)(index + 3U);
        devices[index].function = (u8)(index & 1U);
    }

    mismatches = 0U;
    run_workers(threads, devices, unsafe_read_worker);
    assert(mismatches != 0U); /* Negative control: unsynchronized CF8/CFC. */

    mismatches = 0U;
    run_workers(threads, devices, read_worker);
    assert(mismatches == 0U);

    command_words[(9U << 8) | (2U << 3)] = 0U;
    devices[0].bus = 9U;
    devices[0].device = 2U;
    devices[0].function = 0U;
    assert(pci_enable_memory_busmaster(&devices[0]));
    assert(command_words[(9U << 8) | (2U << 3)] == 6U);

    puts("PCI config serialization: interleaving negative control detected; "
         "production CF8/CFC reads and bus-master admission passed");
    return 0;
}
