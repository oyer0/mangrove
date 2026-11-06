/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <usb/xhci_storage_trace.h>

int main(void)
{
    xhci_storage_trace_counters_t counters = {0};
    xhci_storage_trace_snapshot_t snapshot;

    assert(!xhci_storage_trace_is_active(&counters));
    xhci_storage_trace_counters_begin(&counters);
    assert(xhci_storage_trace_is_active(&counters));

    bool sampled = xhci_storage_trace_is_active(&counters);
    xhci_storage_trace_note_block_begin(&counters, sampled, false, 1U);
    xhci_storage_trace_note_block_end(&counters, sampled, false, true, 3U);
    xhci_storage_trace_note_block_begin(&counters, sampled, true, 8U);
    xhci_storage_trace_note_block_end(&counters, sampled, true, false, 21U);
    xhci_storage_trace_note_bot(&counters, sampled, 0x28U, 4096U,
                                16U, 2U, 9U, 3U, true);
    xhci_storage_trace_note_bot(&counters, sampled, 0x2aU, 4096U,
                                27U, 3U, 18U, 4U, false);
    xhci_storage_trace_note_bot(&counters, sampled, 0x03U, 18U,
                                5U, 1U, 2U, 1U, true);

    xhci_storage_trace_counters_end(&counters, &snapshot);
    assert(!xhci_storage_trace_is_active(&counters));
    assert(snapshot.block_read_requests == 1U &&
           snapshot.block_read_sectors == 1U &&
           snapshot.block_read_min_sectors == 1U &&
           snapshot.block_read_max_sectors == 1U);
    assert(snapshot.block_write_requests == 1U &&
           snapshot.block_write_sectors == 8U &&
           snapshot.block_write_min_sectors == 8U &&
           snapshot.block_write_max_sectors == 8U);
    assert(snapshot.block_read_failures == 0U &&
           snapshot.block_write_failures == 1U &&
           snapshot.block_elapsed_ms == 24U &&
           snapshot.block_max_ms == 21U);
    assert(snapshot.bot_commands == 3U && snapshot.bot_read10 == 1U &&
           snapshot.bot_write10 == 1U && snapshot.bot_other == 1U &&
           snapshot.bot_failed == 1U && snapshot.bot_data_bytes == 8210U);
    assert(snapshot.bot_elapsed_ms == 48U && snapshot.bot_min_ms == 5U &&
           snapshot.bot_max_ms == 27U &&
           snapshot.bot_cbw_ms == 6U && snapshot.bot_data_ms == 29U &&
           snapshot.bot_csw_ms == 8U);

    /* Counters sampled outside a trace window must not leak into the next
     * phase, and an empty phase reports zero rather than an uninitialized
     * minimum request size. */
    xhci_storage_trace_note_block_begin(&counters, false, true, 64U);
    xhci_storage_trace_note_bot(&counters, false, 0x2aU, 32768U,
                                100U, 1U, 90U, 1U, true);
    xhci_storage_trace_counters_begin(&counters);
    xhci_storage_trace_counters_end(&counters, &snapshot);
    assert(snapshot.block_read_requests == 0U &&
           snapshot.block_read_min_sectors == 0U &&
           snapshot.block_write_requests == 0U &&
           snapshot.block_write_min_sectors == 0U &&
           snapshot.bot_commands == 0U && snapshot.bot_elapsed_ms == 0U &&
           snapshot.bot_min_ms == 0U);
    return 0;
}
