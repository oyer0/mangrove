/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <x250_boot_trace.h>
#include <usb/xhci_storage_trace.h>

#define BOOT_LOG_DIAGNOSTIC_VERSION 2U
#define BOOT_LOG_DIAGNOSTIC_V1_FOOTER_BYTES 272U
#define BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES 608U
#define BOOT_LOG_DIAGNOSTIC_STATE_INCOMPLETE 0U
#define BOOT_LOG_DIAGNOSTIC_STATE_COMPLETE 1U
#define BOOT_LOG_DIAGNOSTIC_FLAG_TRACE_INCOMPLETE (1U << 0)
#define BOOT_LOG_DIAGNOSTIC_TRACE_HEADER \
    "\n--- MANGROVE DIAGNOSTIC TRACE v1 ---\n"

typedef struct {
    u32 state;
    u32 flags;
    u32 trace_lines;
    u64 snapshot_bytes;
    u64 trace_bytes;
    u64 preparation_ms;
    u64 payload_write_ms;
    u64 elapsed_before_finalize_ms;
    xhci_storage_trace_snapshot_t storage;
    x250_boot_trace_measurement_snapshot_t preparation_measurements;
    x250_boot_trace_measurement_snapshot_t payload_measurements;
} boot_log_diagnostic_footer_t;

static inline void boot_log_diagnostic_store_u32(u8 *output, u32 value)
{
    output[0] = (u8)value;
    output[1] = (u8)(value >> 8);
    output[2] = (u8)(value >> 16);
    output[3] = (u8)(value >> 24);
}

static inline void boot_log_diagnostic_store_u64(u8 *output, u64 value)
{
    boot_log_diagnostic_store_u32(output, (u32)value);
    boot_log_diagnostic_store_u32(output + 4U, (u32)(value >> 32));
}

static inline u32 boot_log_diagnostic_crc32(const u8 *bytes, usize length)
{
    u32 crc = ~(u32)0U;

    for (usize index = 0; index < length; index++) {
        crc ^= bytes[index];
        for (u32 bit = 0; bit < 8U; bit++)
            crc = (crc >> 1) ^
                ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

static inline void boot_log_diagnostic_footer_encode(
    u8 output[BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES],
    const boot_log_diagnostic_footer_t *footer,
    const u8 *payload, usize payload_bytes)
{
    static const u8 magic[8] = {'M','G','B','L','O','G','0','1'};
    const u64 counters[] = {
        footer->storage.block_read_requests,
        footer->storage.block_read_sectors,
        footer->storage.block_read_min_sectors,
        footer->storage.block_read_max_sectors,
        footer->storage.block_write_requests,
        footer->storage.block_write_sectors,
        footer->storage.block_write_min_sectors,
        footer->storage.block_write_max_sectors,
        footer->storage.block_read_failures,
        footer->storage.block_write_failures,
        footer->storage.block_elapsed_ms,
        footer->storage.block_max_ms,
        footer->storage.bot_commands,
        footer->storage.bot_read10,
        footer->storage.bot_write10,
        footer->storage.bot_other,
        footer->storage.bot_failed,
        footer->storage.bot_data_bytes,
        footer->storage.bot_elapsed_ms,
        footer->storage.bot_min_ms,
        footer->storage.bot_max_ms,
        footer->storage.bot_cbw_ms,
        footer->storage.bot_data_ms,
        footer->storage.bot_csw_ms,
    };
    const x250_boot_trace_measurement_snapshot_t *measurements[] = {
        &footer->preparation_measurements,
        &footer->payload_measurements,
    };

    for (usize index = 0; index < BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES; index++)
        output[index] = 0;
    for (u32 index = 0; index < sizeof(magic); index++)
        output[index] = magic[index];
    boot_log_diagnostic_store_u32(output + 8U,
                                  BOOT_LOG_DIAGNOSTIC_VERSION);
    boot_log_diagnostic_store_u32(output + 12U,
                                  BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES);
    boot_log_diagnostic_store_u32(output + 16U, footer->state);
    boot_log_diagnostic_store_u32(output + 20U, footer->flags);
    boot_log_diagnostic_store_u64(output + 24U, footer->snapshot_bytes);
    boot_log_diagnostic_store_u64(output + 32U, footer->trace_bytes);
    boot_log_diagnostic_store_u64(output + 40U, footer->preparation_ms);
    boot_log_diagnostic_store_u64(output + 48U, footer->payload_write_ms);
    boot_log_diagnostic_store_u64(output + 56U,
                                  footer->elapsed_before_finalize_ms);
    boot_log_diagnostic_store_u32(output + 64U, footer->trace_lines);
    boot_log_diagnostic_store_u32(output + 68U,
        boot_log_diagnostic_crc32(payload, payload_bytes));
    /* Offset 72 contains the footer CRC and offset 76 is reserved zero. */
    for (u32 index = 0; index < 24U; index++)
        boot_log_diagnostic_store_u64(output + 80U + index * 8U,
                                      counters[index]);
    usize wait_offset = BOOT_LOG_DIAGNOSTIC_V1_FOOTER_BYTES;
    for (usize phase = 0;
         phase < sizeof(measurements) / sizeof(measurements[0]); phase++) {
        for (u32 kind = 0; kind < X250_BOOT_TRACE_MEASURE_COUNT; kind++) {
            const x250_boot_trace_measurement_stats_t *stats =
                &measurements[phase]->kinds[kind];
            boot_log_diagnostic_store_u64(output + wait_offset,
                                          stats->count);
            boot_log_diagnostic_store_u64(output + wait_offset + 8U,
                                          stats->elapsed_ms);
            boot_log_diagnostic_store_u64(output + wait_offset + 16U,
                                          stats->maximum_ms);
            wait_offset += 24U;
        }
    }
    boot_log_diagnostic_store_u32(output + 72U,
        boot_log_diagnostic_crc32(output,
                                  BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES));
}
