/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <types.h>
#include <mg/types.h>
#include <mangrove_errors.h>

#define USER_SYNC_WAIT_FOREVER UINT64_MAX

struct process;
typedef struct page_table {
    struct process *owner;
} page_table_t;

typedef struct vmm_user_range_lease {
    page_table_t *address_space;
    bool active;
} vmm_user_range_lease_t;

typedef struct process {
    page_table_t address_space_record;
    page_table_t *address_space;
    pthread_mutex_t mapping_lock;
    volatile u32 *mapped_word;
    usize active_leases;
    bool mapped;
    volatile bool interrupt_requested;
    volatile bool shutdown_requested;
    volatile bool exit_requested;
} process_t;

typedef struct kernel_thread {
    process_t *process;
    pthread_mutex_t event_lock;
    pthread_cond_t event_cond;
    bool event_pending;
} kernel_thread_t;

typedef pthread_mutex_t spinlock_t;

void spinlock_init(spinlock_t *lock);
u64 spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags);
bool scheduler_block(void);
bool scheduler_sleep(u64 milliseconds);
bool scheduler_wake_waiter(kernel_thread_t *thread);
void scheduler_clear_event_wake(kernel_thread_t *thread);
u64 timer_uptime_ms(void);
bool process_shutdown_requested(const process_t *process);
bool process_take_interrupt(process_t *process);
bool vmm_user_range_lease(page_table_t *address_space, const void *address,
                          usize length, bool writable,
                          vmm_user_range_lease_t *out_lease);
void vmm_user_range_release(vmm_user_range_lease_t *lease);

void panic(const char *message) __attribute__((noreturn));
