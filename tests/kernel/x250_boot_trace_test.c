/* SPDX-License-Identifier: GPL-3.0-or-later */
#define X250_BOOT_TRACE 1

#include <stdio.h>

#include "../../kernel/src/x250_boot_trace.c"

const u8 _binary_kernel_assets_font_psf_start[64] = {0};
const u8 _binary_kernel_assets_font_psf_end[64] = {0};

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    (void)x;
    (void)y;
    (void)color;
}

void framebuffer_flush_rect(u32 x, u32 y, u32 width, u32 height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static int trace_test_fail(const char *message)
{
    fprintf(stderr, "x250 boot trace test failed: %s\n", message);
    return 1;
}

static bool trace_test_reconstruct(u32 first, u32 end, char *output,
                                   u32 capacity)
{
    u32 position = 0;

    for (u32 row = first; row < end; row++) {
        const char *line = trace_lines[row];

        if (row != first) {
            if (position + 1U >= capacity)
                return false;
            output[position++] = ' ';
            if (line[0] == ' ' && line[1] == ' ')
                line += 2;
        }
        while (*line) {
            if (position + 1U >= capacity)
                return false;
            output[position++] = *line++;
        }
    }
    output[position] = '\0';
    return true;
}

int main(void)
{
    static const char expected_root[] =
        "37 [FAIL] root filesystem required rc=2 state=FAILED reason="
        "Mangrove root volume not found";
    char reconstructed[TRACE_MESSAGE_CAPACITY];
    u32 root_first;
    u32 root_end;
    u32 columns;

    trace_width = 1366U;
    columns = (trace_width - TRACE_TEXT_MARGIN_X * 2U) /
              TRACE_DIAG_CELL_WIDTH;
    trace_line_count = 0;
    trace_append_result(37U, false, "root filesystem", true, true, 2,
                        "FAILED", "Mangrove root volume not found");
    if (trace_line_count != 1U ||
        __builtin_strlen(trace_lines[0]) > columns ||
        !trace_test_reconstruct(0U, trace_line_count, reconstructed,
                                sizeof(reconstructed)) ||
        __builtin_strcmp(reconstructed, expected_root) != 0)
        return trace_test_fail("X250-width root failure fits without clipping");

    trace_width = 350U;
    columns = (trace_width - TRACE_TEXT_MARGIN_X * 2U) /
              TRACE_DIAG_CELL_WIDTH;
    trace_line_count = 0;
    trace_append_result(37U, true, "xHCI", false, true, 0, "READY", NULL);
    root_first = trace_line_count;
    trace_append_result(37U, false, "root filesystem", true, true, 2,
                        "FAILED", "Mangrove root volume not found");
    root_end = trace_line_count;

    if (root_end - root_first < 2U ||
        !trace_test_reconstruct(root_first, root_end, reconstructed,
                                sizeof(reconstructed)) ||
        __builtin_strcmp(reconstructed, expected_root) != 0)
        return trace_test_fail("root failure wraps without losing text");

    for (u32 row = root_first; row < root_end; row++) {
        if (__builtin_strlen(trace_lines[row]) > columns ||
            trace_line_states[row] != 'F')
            return trace_test_fail("wrapped failure rows fit diagnostic width");
    }

    trace_append_result(37U, false, "accounts", true, true, 4, "SKIPPED",
                        "Mangrove root volume not found");
    if (__builtin_strncmp(trace_lines[0], "37 [PASS] xHCI",
                          sizeof("37 [PASS] xHCI") - 1U) != 0 ||
        trace_line_count <= root_end ||
        trace_line_states[trace_line_count - 1U] != 'F')
        return trace_test_fail("first failure and dependency rows are retained");

    if (TRACE_HISTORY_LINES * TRACE_DIAG_LINE_HEIGHT +
            TRACE_PANEL_PADDING_Y * 2U + TRACE_PANEL_MARGIN_Y > 768U)
        return trace_test_fail("diagnostic history fits X250 display height");

    trace_line_count = 0;
    trace_active = true;
    trace_width = 1366U;
    trace_height = 768U;
    trace_stride = trace_width;
    trace_framebuffer = NULL;
    trace_glyphs_ready = true;
    x250_boot_trace_xhci_summary(16U, 1U, 0U, 1U, 0U);
    x250_boot_trace_xhci_port(5U, 0x00000c01U, "FAILED", true, 7,
                              "reset-completion-change-missing");
    x250_boot_trace_usb_device(5U, 2U, 3U, 0x0781U, 0x5567U,
                               0U, 0U, 0U, 0);
    x250_boot_trace_msc_probe(5U, 2U, "inquiry", 9, false, false,
                              false, false, false, 0U, 0U, 0U, NULL);
    x250_boot_trace_block_device(0U, "SATA", 512U, 123456U, "ThinkDisk",
                                 0, 1U, 2U, 0U, 0U);
    x250_boot_trace_block_device(1U, "USB", 512U, 123456U,
                                 "SanDisk Cruzer Blade",
                                 X250_TRACE_GPT_USB_SCAN_REPORTED, 0U, 0U,
                                 2U, 1U);
    x250_boot_trace_root_inventory(3U, 2U, 2U, 1U);
    if (trace_line_count != 7U ||
        __builtin_strcmp(trace_lines[0],
            "42 [INFO] xHCI ports=16 ccs=1 enum=0 failed=1 usb-msd=0") != 0 ||
        __builtin_strcmp(trace_lines[1],
            "43 [FAIL] port=5 PORTSC=00000c01 CCS=1 PED=0 speed=3 state=FAILED rc=7 reason=reset-completion-change-missing") != 0 ||
        __builtin_strcmp(trace_lines[2],
            "44 [INFO] USB p=5 s=2 sp=3 id=0781:5567 cls=00/00/00 msc-ep-rc=0") != 0 ||
        __builtin_strcmp(trace_lines[3],
            "45 [FAIL] MSC p=5 s=2 stage=inquiry rc=9 BOT=0 cap=0 blk=0 GPT=-") != 0 ||
        __builtin_strcmp(trace_lines[4],
            "46 [INFO] block#0 SATA sectors=123456 size=512 GPT-rc=0 type=1 entries=2 reg=0 root=0 model=ThinkDisk") != 0 ||
        __builtin_strcmp(trace_lines[5],
            "46 [INFO] block#1 USB sectors=123456 size=512 GPT=USB-scan reg=2 root=1 model=SanDisk Cruzer Blade") != 0 ||
        __builtin_strcmp(trace_lines[6],
            "46 [INFO] root scan blocks=3 disks=2 gpt-parts=2 root-role=1") != 0)
        return trace_test_fail("USB discovery diagnostic rows preserve stage and status");

    return 0;
}
