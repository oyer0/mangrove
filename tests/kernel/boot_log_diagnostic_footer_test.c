/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <boot_log_diagnostics.h>

int main(void)
{
    static const u8 payload[] = "bounded boot log payload";
    static const u8 crc_vector[] = "123456789";
    boot_log_diagnostic_footer_t input = {
        .state = BOOT_LOG_DIAGNOSTIC_STATE_COMPLETE,
        .flags = 0U,
        .trace_lines = 3U,
        .snapshot_bytes = 12U,
        .trace_bytes = 48U,
        .preparation_ms = 9496U,
        .payload_write_ms = 9464U,
        .elapsed_before_finalize_ms = 18960U,
        .preparation_measurements = {.kinds = {{1U, 2U, 3U}}},
        .payload_measurements = {.kinds = {{4U, 5U, 6U}}},
        .storage = {
            .block_read_requests = 7U,
            .block_write_requests = 9U,
            .bot_commands = 16U,
            .bot_write10 = 9U,
            .bot_data_bytes = 35840U,
            .bot_elapsed_ms = 9000U,
            .bot_max_ms = 1600U,
            .bot_csw_ms = 1200U,
        },
    };
    u8 footer[BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES];

    assert(boot_log_diagnostic_crc32(crc_vector, 9U) == 0xcbf43926U);
    boot_log_diagnostic_footer_encode(footer, &input, payload,
                                       sizeof(payload) - 1U);
    assert(memcmp(footer, "MGBLOG01", 8U) == 0);
    assert(footer[8] == BOOT_LOG_DIAGNOSTIC_VERSION && footer[9] == 0U);
    assert(footer[16] == 1U && footer[20] == 0U);
    assert(footer[64] == 3U && footer[65] == 0U);
    assert(footer[68] != 0U);
    assert(footer[72] != 0U);
    assert(footer[76] == 0U);
    assert(footer[80] == 7U);
    assert(footer[112] == 9U);
    assert(footer[176] == 16U);
    assert(footer[192] == 9U);
    assert(footer[216] == (u8)(35840U & 0xffU));
    assert(footer[272] == 1U && footer[280] == 2U && footer[288] == 3U);
    assert(footer[440] == 4U && footer[448] == 5U && footer[456] == 6U);

    input.flags = BOOT_LOG_DIAGNOSTIC_FLAG_TRACE_INCOMPLETE;
    boot_log_diagnostic_footer_encode(footer, &input, payload,
                                       sizeof(payload) - 1U);
    assert(footer[20] == BOOT_LOG_DIAGNOSTIC_FLAG_TRACE_INCOMPLETE);
    puts("boot-log footer: CRCs, timing, storage counters, and incomplete-trace flag passed");
    return 0;
}
