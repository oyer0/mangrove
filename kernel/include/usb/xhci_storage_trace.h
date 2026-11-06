/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Bounded, opt-in counters used to correlate boot-log VFS phases with native
 * USB mass-storage requests. These counters never select or alter I/O. */
typedef struct {
    volatile u32 active;
    volatile u64 block_read_requests;
    volatile u64 block_read_sectors;
    volatile u64 block_read_min_sectors;
    volatile u64 block_read_max_sectors;
    volatile u64 block_write_requests;
    volatile u64 block_write_sectors;
    volatile u64 block_write_min_sectors;
    volatile u64 block_write_max_sectors;
    volatile u64 block_read_failures;
    volatile u64 block_write_failures;
    volatile u64 block_elapsed_ms;
    volatile u64 block_max_ms;
    volatile u64 bot_commands;
    volatile u64 bot_read10;
    volatile u64 bot_write10;
    volatile u64 bot_other;
    volatile u64 bot_failed;
    volatile u64 bot_data_bytes;
    volatile u64 bot_elapsed_ms;
    volatile u64 bot_min_ms;
    volatile u64 bot_max_ms;
    volatile u64 bot_cbw_ms;
    volatile u64 bot_data_ms;
    volatile u64 bot_csw_ms;
} xhci_storage_trace_counters_t;

typedef struct xhci_storage_trace_snapshot {
    u64 block_read_requests;
    u64 block_read_sectors;
    u64 block_read_min_sectors;
    u64 block_read_max_sectors;
    u64 block_write_requests;
    u64 block_write_sectors;
    u64 block_write_min_sectors;
    u64 block_write_max_sectors;
    u64 block_read_failures;
    u64 block_write_failures;
    u64 block_elapsed_ms;
    u64 block_max_ms;
    u64 bot_commands;
    u64 bot_read10;
    u64 bot_write10;
    u64 bot_other;
    u64 bot_failed;
    u64 bot_data_bytes;
    u64 bot_elapsed_ms;
    u64 bot_min_ms;
    u64 bot_max_ms;
    u64 bot_cbw_ms;
    u64 bot_data_ms;
    u64 bot_csw_ms;
} xhci_storage_trace_snapshot_t;

static inline void xhci_storage_trace_atomic_max(volatile u64 *target,
                                                 u64 value)
{
    u64 current = __atomic_load_n(target, __ATOMIC_RELAXED);

    while (current < value &&
           !__atomic_compare_exchange_n(target, &current, value, true,
                                         __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED)) {
    }
}

static inline void xhci_storage_trace_counters_begin(
    xhci_storage_trace_counters_t *counters)
{
    if (!counters)
        return;
    __atomic_store_n(&counters->active, 0U, __ATOMIC_RELEASE);
#define XHCI_TRACE_ZERO(field) \
    __atomic_store_n(&counters->field, 0ULL, __ATOMIC_RELAXED)
    XHCI_TRACE_ZERO(block_read_requests);
    XHCI_TRACE_ZERO(block_read_sectors);
    __atomic_store_n(&counters->block_read_min_sectors, (u64)-1,
                     __ATOMIC_RELAXED);
    XHCI_TRACE_ZERO(block_read_max_sectors);
    XHCI_TRACE_ZERO(block_write_requests);
    XHCI_TRACE_ZERO(block_write_sectors);
    __atomic_store_n(&counters->block_write_min_sectors, (u64)-1,
                     __ATOMIC_RELAXED);
    XHCI_TRACE_ZERO(block_write_max_sectors);
    XHCI_TRACE_ZERO(block_read_failures);
    XHCI_TRACE_ZERO(block_write_failures);
    XHCI_TRACE_ZERO(block_elapsed_ms);
    XHCI_TRACE_ZERO(block_max_ms);
    XHCI_TRACE_ZERO(bot_commands);
    XHCI_TRACE_ZERO(bot_read10);
    XHCI_TRACE_ZERO(bot_write10);
    XHCI_TRACE_ZERO(bot_other);
    XHCI_TRACE_ZERO(bot_failed);
    XHCI_TRACE_ZERO(bot_data_bytes);
    XHCI_TRACE_ZERO(bot_elapsed_ms);
    __atomic_store_n(&counters->bot_min_ms, (u64)-1, __ATOMIC_RELAXED);
    XHCI_TRACE_ZERO(bot_max_ms);
    XHCI_TRACE_ZERO(bot_cbw_ms);
    XHCI_TRACE_ZERO(bot_data_ms);
    XHCI_TRACE_ZERO(bot_csw_ms);
#undef XHCI_TRACE_ZERO
    __atomic_store_n(&counters->active, 1U, __ATOMIC_RELEASE);
}

static inline bool xhci_storage_trace_is_active(
    const xhci_storage_trace_counters_t *counters)
{
    return counters &&
        __atomic_load_n(&counters->active, __ATOMIC_ACQUIRE) != 0U;
}

static inline void xhci_storage_trace_note_block_begin(
    xhci_storage_trace_counters_t *counters, bool sampled_active,
    bool write, u32 sectors)
{
    if (!counters || !sampled_active)
        return;
    volatile u64 *requests = write ? &counters->block_write_requests :
                                     &counters->block_read_requests;
    volatile u64 *total = write ? &counters->block_write_sectors :
                                  &counters->block_read_sectors;
    volatile u64 *minimum = write ? &counters->block_write_min_sectors :
                                    &counters->block_read_min_sectors;
    volatile u64 *maximum = write ? &counters->block_write_max_sectors :
                                    &counters->block_read_max_sectors;
    __atomic_fetch_add(requests, 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add(total, sectors, __ATOMIC_RELAXED);
    u64 current = __atomic_load_n(minimum, __ATOMIC_RELAXED);
    while (current > sectors &&
           !__atomic_compare_exchange_n(minimum, &current, sectors, true,
                                         __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED)) {
    }
    xhci_storage_trace_atomic_max(maximum, sectors);
}

static inline void xhci_storage_trace_note_block_end(
    xhci_storage_trace_counters_t *counters, bool sampled_active,
    bool write, bool success, u64 elapsed_ms)
{
    if (!counters || !sampled_active)
        return;
    if (!success) {
        volatile u64 *failures = write ? &counters->block_write_failures :
                                         &counters->block_read_failures;
        __atomic_fetch_add(failures, 1ULL, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&counters->block_elapsed_ms, elapsed_ms,
                       __ATOMIC_RELAXED);
    xhci_storage_trace_atomic_max(&counters->block_max_ms, elapsed_ms);
}

static inline void xhci_storage_trace_note_bot(
    xhci_storage_trace_counters_t *counters, bool sampled_active, u8 opcode,
    u32 data_bytes, u64 elapsed_ms, u64 cbw_ms, u64 data_ms, u64 csw_ms,
    bool success)
{
    if (!counters || !sampled_active)
        return;
    __atomic_fetch_add(&counters->bot_commands, 1ULL, __ATOMIC_RELAXED);
    if (opcode == 0x28U)
        __atomic_fetch_add(&counters->bot_read10, 1ULL, __ATOMIC_RELAXED);
    else if (opcode == 0x2aU)
        __atomic_fetch_add(&counters->bot_write10, 1ULL, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&counters->bot_other, 1ULL, __ATOMIC_RELAXED);
    if (!success)
        __atomic_fetch_add(&counters->bot_failed, 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters->bot_data_bytes, data_bytes,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters->bot_elapsed_ms, elapsed_ms,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters->bot_cbw_ms, cbw_ms, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters->bot_data_ms, data_ms, __ATOMIC_RELAXED);
    __atomic_fetch_add(&counters->bot_csw_ms, csw_ms, __ATOMIC_RELAXED);
    volatile u64 *minimum = &counters->bot_min_ms;
    u64 current = __atomic_load_n(minimum, __ATOMIC_RELAXED);
    while (current > elapsed_ms &&
           !__atomic_compare_exchange_n(minimum, &current, elapsed_ms, true,
                                         __ATOMIC_RELAXED,
                                         __ATOMIC_RELAXED)) {
    }
    xhci_storage_trace_atomic_max(&counters->bot_max_ms, elapsed_ms);
}

static inline void xhci_storage_trace_counters_end(
    xhci_storage_trace_counters_t *counters,
    xhci_storage_trace_snapshot_t *snapshot)
{
    if (!counters || !snapshot)
        return;
    __atomic_store_n(&counters->active, 0U, __ATOMIC_RELEASE);
#define XHCI_TRACE_LOAD(field) \
    snapshot->field = __atomic_load_n(&counters->field, __ATOMIC_ACQUIRE)
    XHCI_TRACE_LOAD(block_read_requests);
    XHCI_TRACE_LOAD(block_read_sectors);
    XHCI_TRACE_LOAD(block_read_min_sectors);
    XHCI_TRACE_LOAD(block_read_max_sectors);
    XHCI_TRACE_LOAD(block_write_requests);
    XHCI_TRACE_LOAD(block_write_sectors);
    XHCI_TRACE_LOAD(block_write_min_sectors);
    XHCI_TRACE_LOAD(block_write_max_sectors);
    XHCI_TRACE_LOAD(block_read_failures);
    XHCI_TRACE_LOAD(block_write_failures);
    XHCI_TRACE_LOAD(block_elapsed_ms);
    XHCI_TRACE_LOAD(block_max_ms);
    XHCI_TRACE_LOAD(bot_commands);
    XHCI_TRACE_LOAD(bot_read10);
    XHCI_TRACE_LOAD(bot_write10);
    XHCI_TRACE_LOAD(bot_other);
    XHCI_TRACE_LOAD(bot_failed);
    XHCI_TRACE_LOAD(bot_data_bytes);
    XHCI_TRACE_LOAD(bot_elapsed_ms);
    XHCI_TRACE_LOAD(bot_min_ms);
    XHCI_TRACE_LOAD(bot_max_ms);
    XHCI_TRACE_LOAD(bot_cbw_ms);
    XHCI_TRACE_LOAD(bot_data_ms);
    XHCI_TRACE_LOAD(bot_csw_ms);
#undef XHCI_TRACE_LOAD
    if (!snapshot->block_read_requests)
        snapshot->block_read_min_sectors = 0;
    if (!snapshot->block_write_requests)
        snapshot->block_write_min_sectors = 0;
    if (!snapshot->bot_commands)
        snapshot->bot_min_ms = 0;
}
