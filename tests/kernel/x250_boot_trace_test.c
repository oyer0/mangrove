/* SPDX-License-Identifier: GPL-3.0-or-later */
#define X250_BOOT_TRACE 1

#include <stdio.h>

#include "../../kernel/src/x250_boot_trace.c"

const u8 _binary_kernel_assets_font_psf_start[64] = {0};
const u8 _binary_kernel_assets_font_psf_end[64] = {0};
static u32 trace_test_flush_count;
static bool trace_test_bad_pixel;

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    if (x >= trace_width || y >= trace_height) trace_test_bad_pixel = true;
    (void)color;
}

void framebuffer_flush_rect(u32 x, u32 y, u32 width, u32 height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    trace_test_flush_count++;
}

u32 inl(u16 port)
{
    (void)port;
    return 0;
}

static int trace_test_fail(const char *message)
{
    fprintf(stderr, "x250 boot trace test failed: %s\n", message);
    return 1;
}

static u32 trace_test_slot(u32 chronological_index)
{
    return (trace_line_start + chronological_index) %
           TRACE_HISTORY_CAPACITY;
}

static const char *trace_test_line(u32 chronological_index)
{
    return trace_lines[trace_test_slot(chronological_index)];
}

static char trace_test_state(u32 chronological_index)
{
    return trace_line_states[trace_test_slot(chronological_index)];
}

static bool trace_test_reconstruct(u32 first, u32 end, char *output,
                                   u32 capacity)
{
    u32 position = 0;

    for (u32 row = first; row < end; row++) {
        const char *line = trace_test_line(row);

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
    trace_line_start = 0;
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
    trace_line_start = 0;
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
        if (__builtin_strlen(trace_test_line(row)) > columns ||
            trace_test_state(row) != 'F')
            return trace_test_fail("wrapped failure rows fit diagnostic width");
    }

    trace_append_result(37U, false, "accounts", true, true, 4, "SKIPPED",
                        "Mangrove root volume not found");
    if (__builtin_strncmp(trace_test_line(0U), "37 [PASS] xHCI",
                          sizeof("37 [PASS] xHCI") - 1U) != 0 ||
        trace_line_count <= root_end ||
        trace_test_state(trace_line_count - 1U) != 'F')
        return trace_test_fail("first failure and dependency rows are retained");

    if (TRACE_HISTORY_LINES * TRACE_DIAG_LINE_HEIGHT +
            TRACE_PANEL_PADDING_Y * 2U + TRACE_PANEL_MARGIN_Y > 768U)
        return trace_test_fail("diagnostic history fits X250 display height");

    trace_line_count = 0;
    trace_line_start = 0;
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
                               0U, 0U, 0U, 0U, false, NULL, 0);
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
        __builtin_strcmp(trace_test_line(0U),
            "42 [INFO] xHCI ports=16 ccs=1 enum=0 failed=1 usb-msd=0") != 0 ||
        __builtin_strcmp(trace_test_line(1U),
            "43 [FAIL] port=5 PORTSC=00000c01 CCS=1 PED=0 PLS=0 PP=0 speed=3 state=FAILED rc=7 reason=reset-completion-change-missing") != 0 ||
        __builtin_strcmp(trace_test_line(2U),
            "44 [INFO] USB p=5 s=2 sp=3 id=0781:5567 cls=00/00/00 bind=unsupported msc-ep=absent") != 0 ||
        __builtin_strcmp(trace_test_line(3U),
            "45 [FAIL] MSC p=5 s=2 stage=inquiry rc=9 BOT=0 cap=0 blk=0 GPT=-") != 0 ||
        __builtin_strcmp(trace_test_line(4U),
            "46 [INFO] block#0 SATA sectors=123456 size=512 GPT-rc=0 type=1 entries=2 reg=0 root=0 model=ThinkDisk") != 0 ||
        __builtin_strcmp(trace_test_line(5U),
            "46 [INFO] block#1 USB sectors=123456 size=512 GPT=USB-scan reg=2 root=1 model=SanDisk Cruzer Blade") != 0 ||
        __builtin_strcmp(trace_test_line(6U),
            "46 [INFO] root scan blocks=3 disks=2 gpt-parts=2 root-role=1") != 0)
        return trace_test_fail("USB discovery diagnostic rows preserve stage and status");

    trace_line_count = 0;
    trace_line_start = 0;
    x250_boot_trace_usb_device(7U, 3U, 3U, 0x8087U, 0x0a2aU,
                               0U, 0U, 0U, 0U, false, "storage",
                               6);
    if (trace_line_count != 1U ||
        __builtin_strcmp(trace_test_line(0U),
            "44 [FAIL] USB p=7 s=3 sp=3 id=8087:0a2a cls=00/00/00 bind=failed probe=storage rc=6") != 0)
        return trace_test_fail("descriptor probe failure is not labeled unsupported");

    trace_line_count = 0;
    trace_line_start = 0;
    trace_width = 1366U;
    columns = (trace_width - TRACE_TEXT_MARGIN_X * 2U) /
              TRACE_DIAG_CELL_WIDTH;
    x250_boot_trace_timing(12345U, "port probe complete", 7U, true,
                           10000U, "FAILED");
    if (trace_line_count != 1U ||
        __builtin_strcmp(trace_test_line(0U),
            "47 [INFO] up=12345ms port probe complete p=7 dt=10000ms state=FAILED") != 0 ||
        __builtin_strlen(trace_test_line(0U)) > columns)
        return trace_test_fail("timestamped port timing fits diagnostic width");

    trace_line_count = 0;
    trace_line_start = 0;
    x250_boot_trace_timing_value(16399U, "boot log data write", "bytes",
                                 65536U, true, 9955U, "SAVED");
    if (trace_line_count != 1U ||
        __builtin_strcmp(trace_test_line(0U),
            "47 [INFO] up=16399ms boot log data write bytes=65536 dt=9955ms state=SAVED") != 0 ||
        __builtin_strlen(trace_test_line(0U)) > columns)
        return trace_test_fail("boot-log byte count and elapsed time fit diagnostic width");

    trace_line_count = 0;
    trace_line_start = 0;
    const xhci_storage_trace_snapshot_t io = {
        .block_read_requests = 2U, .block_read_sectors = 17U,
        .block_read_min_sectors = 1U, .block_read_max_sectors = 8U,
        .block_write_requests = 11U, .block_write_sectors = 88U,
        .block_write_min_sectors = 8U, .block_write_max_sectors = 8U,
        .block_read_failures = 0U, .block_write_failures = 0U,
        .block_elapsed_ms = 18900U, .block_max_ms = 300U,
        .bot_commands = 18U, .bot_read10 = 2U, .bot_write10 = 11U,
        .bot_other = 5U, .bot_failed = 1U, .bot_data_bytes = 8210U,
        .bot_elapsed_ms = 18000U, .bot_min_ms = 30U, .bot_max_ms = 2300U,
        .bot_cbw_ms = 180U, .bot_data_ms = 16000U, .bot_csw_ms = 900U
    };
    x250_boot_trace_storage_io(19131U, "boot log write", &io);
    if (trace_line_count != 2U ||
        __builtin_strcmp(trace_test_line(0U),
            "47 [INFO] up=19131ms boot log write USBblk r=2/17s w=11/88s sizeR=1-8 sizeW=8-8 BOT=18 R10=2 W10=11 other=5 iofail=0/0 BOTfail=1") != 0 ||
        __builtin_strcmp(trace_test_line(1U),
            "47 [INFO] up=19131ms boot log write ms USBblk=18900/300 BOT=18000/1000/30/2300 CBW/data/CSW=180/16000/900") != 0)
        return trace_test_fail("boot-log phase reports bounded USB block and BOT counts/timings");

    /* Exercise more than the visible panel capacity. The ring must retain
       recent discovery and root-failure evidence instead of silently
       dropping every event after the first forty wrapped rows. */
    trace_line_count = 0;
    trace_line_start = 0;
    trace_active = false;
    x250_boot_trace_begin();
    trace_width = 1366U;
    x250_boot_trace_xhci_summary(200U, 200U, 0U, 200U, 0U);
    for (u32 port = 1U; port <= 200U; port++)
        x250_boot_trace_xhci_port(port, 0x00000c01U, "FAILED", true, 7,
                                  "reset-completion-change-missing");
    x250_boot_trace_usb_device(200U, 2U, 3U, 0x0781U, 0x5567U,
                               0x08U, 0x06U, 0x50U, 2U, true, NULL, 0);
    x250_boot_trace_msc_probe(200U, 2U, "inquiry", 9, false, false,
                              false, false, false, 0U, 0U, 0U, NULL);
    x250_boot_trace_block_device(0U, "SATA", 512U, 123456U, "ThinkDisk",
                                 0, 1U, 2U, 0U, 0U);
    x250_boot_trace_root_inventory(1U, 1U, 0U, 0U);
    trace_append_result(37U, false, "root filesystem", true, true, 2,
                        "FAILED", "Mangrove root volume not found");
    trace_append_result(37U, false, "accounts", true, true, 4,
                        "SKIPPED", "Mangrove root volume not found");
    if (trace_line_count != TRACE_HISTORY_CAPACITY ||
        __builtin_strncmp(trace_test_line(trace_line_count - 1U),
                          "37 [FAIL] accounts", 18U) != 0 ||
        trace_test_state(trace_line_count - 2U) != 'F')
        return trace_test_fail("bounded trace history retains latest failures");
    bool retained_usb = false;
    bool retained_msc = false;
    bool retained_root = false;
    u32 visible_start = trace_line_count - TRACE_HISTORY_LINES;
    for (u32 row = visible_start; row < trace_line_count; row++) {
        const char *line = trace_test_line(row);
        if (__builtin_strncmp(line, "44 [INFO] USB p=", 16U) == 0)
            retained_usb = true;
        if (__builtin_strncmp(line, "45 [FAIL] MSC p=", 16U) == 0)
            retained_msc = true;
        if (__builtin_strncmp(line, "37 [FAIL] root filesystem", 25U) == 0)
            retained_root = true;
    }
    if (!retained_usb || !retained_msc || !retained_root)
        return trace_test_fail("visible window keeps USB, MSC, and root evidence");

    /* Successful boot reaches Shoot after callbacks that can push early xHCI
       timing rows above the visible ring window. The final view must retain
       those rows from the bounded history. */
    trace_line_count = 0;
    trace_line_start = 0;
    x250_boot_trace_begin();
    x250_boot_trace_timing(321U, "xHCI total", 0, true, 250U, "READY");
    for (u32 callback = 0U; callback < 50U; callback++)
        x250_boot_trace_stage(37U, true, "optional callback");
    x250_boot_trace_refresh();
    bool retained_timing = false;
    visible_start = trace_line_count > TRACE_HISTORY_LINES ?
        trace_line_count - TRACE_HISTORY_LINES : 0U;
    for (u32 row = visible_start; row < trace_line_count; row++) {
        if (__builtin_strcmp(trace_test_line(row),
                "47 [INFO] up=321ms xHCI total dt=250ms state=READY") == 0)
            retained_timing = true;
    }
    if (!retained_timing)
        return trace_test_fail("final view retains early xHCI timing");

    trace_test_flush_count = 0;
    trace_width = 1366U;
    trace_height = 768U;
    trace_stride = trace_width;
    trace_framebuffer = (volatile u32 *)(uintptr_t)1U;
    trace_use_framebuffer_api = true;
    x250_boot_trace_refresh();
    if (!trace_test_flush_count)
        return trace_test_fail("final refresh flushes report after console output");

    /* Firmware GOP mode need not match the LCD's native resolution. */
    trace_width = 640U;
    trace_height = 480U;
    trace_stride = trace_width;
    trace_test_flush_count = 0;
    x250_boot_trace_refresh();
    if (!trace_test_flush_count)
        return trace_test_fail("firmware 640x480 mode must display discovery results");

    trace_line_count = trace_line_start = 0;
    x250_boot_trace_xhci_summary(16U, 0U, 0U, 0U, 0U);
    x250_boot_trace_block_device(0U, "SATA", 512U, 123456U, "Internal disk",
                                 0, 1U, 0U, 0U, 0U);
    x250_boot_trace_root_inventory(1U, 1U, 0U, 0U);
    trace_append_result(37U, false, "root filesystem", true, true, 2,
                        "FAILED", "Mangrove root volume not found");
    for (u32 i = 0; i < 24U; i++)
        trace_append_result(37U, true, "optional callback", false, true, 0,
                            "READY", NULL);
    x250_boot_trace_refresh();
    bool summary_visible = false;
    bool reason_visible = false;
    u32 first = trace_line_count > trace_visible_rows()
        ? trace_line_count - trace_visible_rows() : 0U;
    for (u32 i = first; i < trace_line_count; i++) {
        if (__builtin_strncmp(trace_test_line(i), "42 [INFO]", 9U) == 0)
            summary_visible = true;
        if (__builtin_strncmp(trace_test_line(i), "37 [FAIL] root filesystem", 25U) == 0) {
            u32 end = i + 1U;
            while (end < trace_line_count && trace_test_line(end)[0] == ' ')
                end++;
            reason_visible = trace_test_reconstruct(i, end, reconstructed,
                sizeof(reconstructed)) && __builtin_strstr(reconstructed,
                    "Mangrove root volume not found") != NULL;
        }
    }
    if (!summary_visible || !reason_visible || trace_test_bad_pixel)
        return trace_test_fail("640x480 final report preserves discovery and complete failure within bounds");

    trace_line_count = trace_line_start = 0;
    x250_boot_trace_routing(false, 0x24U, 0U, 0U, 3U, 0U, 3U);
    if (!trace_test_reconstruct(0U, trace_line_count, reconstructed,
                                sizeof(reconstructed)) ||
        !__builtin_strstr(reconstructed,
            "Intel routing USB2=00000000>00000000 allow=00000024 USB3=00000000>00000003 allow=00000003"))
        return trace_test_fail("routing report preserves masks and failed readback values");
    for (u32 i = 0; i < trace_line_count; i++)
        if (trace_test_state(i) != 'F')
            return trace_test_fail("routing failure remains marked across wrapped rows");

    /* The failure view must include raw disconnected ports as well as the
       complete root error at the firmware's 640x480 mode. */
    trace_line_count = trace_line_start = 0;
    u32 samples[15];
    for (u32 i = 0; i < 15U; i++) samples[i] = 0x2a0U;
    trace_append_result(37U, false, "root filesystem", true, true, 2,
                        "FAILED", "Mangrove root volume not found");
    const char *stages[] = {"reset", "routed", "started", "drained", "final"};
    for (u32 i = 0; i < 5U; i++)
        x250_boot_trace_ports_stage(stages[i], 1234U + i, 5U, 0U, 0U, 0x7fffU);
    x250_boot_trace_protocol(2U, 0U, 1U, 11U, 0U);
    x250_boot_trace_protocol(3U, 0U, 12U, 4U, 0U);
    x250_boot_trace_ports_raw(0x80U, 0xf1220480U, samples, samples, samples, 15U);
    trace_append_result(21U, false, "required kernel bring-up", true, true, 1U,
                        "FAILED", "Mangrove root volume not found");
    x250_boot_trace_refresh();
    if (trace_line_count > trace_visible_rows())
        return trace_test_fail("15-port failure evidence fits the 640x480 view");
    bool first_port = false, last_port = false;
    for (u32 i = 0; i < trace_line_count; i++) {
        if (__builtin_strstr(trace_test_line(i), "1:000002a0/000002a0/000002a0")) first_port = true;
        if (__builtin_strstr(trace_test_line(i), "15:000002a0/000002a0/000002a0")) last_port = true;
    }
    if (!first_port || !last_port || trace_test_bad_pixel)
        return trace_test_fail("all disconnected port registers remain visible within display bounds");

    trace_active = false;
    x250_boot_trace_begin();
    x250_boot_trace_stage(37U, true, "callback chatter");
    x250_boot_trace_result(21U, true, "required kernel bring-up", true,
                           true, 0, "READY", NULL);
    x250_boot_trace_refresh();
    if (trace_line_count < 1U ||
        __builtin_strncmp(trace_test_line(0U),
                          "21 [PASS] required kernel bring-up",
                          sizeof("21 [PASS] required kernel bring-up") - 1U) != 0)
        return trace_test_fail("final bring-up outcome survives persistent trace filtering");

    trace_active = false;
    x250_boot_trace_begin();
    trace_append_result(47U, true, "first event", false, false, 0,
                        "READY", NULL);
    x250_boot_trace_timing(1234U, "last event", 7U, true, 88U, "FAILED");
    char exported[X250_BOOT_TRACE_EXPORT_CAPACITY];
    u32 exported_lines = 0;
    bool export_complete = false;
    usize exported_bytes = x250_boot_trace_export(
        exported, sizeof(exported), &exported_lines, &export_complete);
    if (exported_bytes >= sizeof(exported))
        return trace_test_fail("trace export exceeded caller capacity");
    exported[exported_bytes] = '\0';
    if (!export_complete || exported_lines != 2U ||
        __builtin_strncmp(exported,
            "47 [PASS] first event optional state=READY\n",
            sizeof("47 [PASS] first event optional state=READY\n") - 1U) != 0 ||
        exported_bytes <=
            sizeof("47 [PASS] first event optional state=READY\n") - 1U ||
        !__builtin_strstr(exported, "47 [INFO] up=1234ms last event p=7 dt=88ms state=FAILED\n")) {
        fprintf(stderr, "export complete=%u lines=%u bytes=%llu: %s\n",
                export_complete, exported_lines,
                (unsigned long long)exported_bytes, exported);
        return trace_test_fail("persistent export preserves trace order and line boundaries");
    }

    x250_boot_trace_measurements_begin();
    x250_boot_trace_measurement_note(
        X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE, 7U);
    x250_boot_trace_measurement_note(
        X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE, 3U);
    x250_boot_trace_measurement_note(
        X250_BOOT_TRACE_MEASURE_RECOVERY_PIN_HANDOFF, 4U);
    x250_boot_trace_measurement_snapshot_t measurements;
    x250_boot_trace_measurements_end(&measurements);
    x250_boot_trace_measurement_note(
        X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE, 99U);
    if (measurements.kinds[X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE].count != 2U ||
        measurements.kinds[X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE].elapsed_ms != 10U ||
        measurements.kinds[X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE].maximum_ms != 7U ||
        measurements.kinds[X250_BOOT_TRACE_MEASURE_RECOVERY_PIN_HANDOFF].count != 1U ||
        measurements.kinds[X250_BOOT_TRACE_MEASURE_RECOVERY_PIN_HANDOFF].elapsed_ms != 4U ||
        measurements.kinds[X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE].elapsed_ms == 109U)
        return trace_test_fail("wait counters are bounded to their active window");

    x250_boot_trace_measurements_begin();
    x250_boot_trace_measurement_note(
        X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE, 5U);
    x250_boot_trace_measurements_end(NULL);
    if (__atomic_load_n(&trace_measurements_active, __ATOMIC_ACQUIRE))
        return trace_test_fail("discarded measurement snapshot closes its window");
    x250_boot_trace_measurement_note(
        X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE, 99U);
    x250_boot_trace_measurements_end(&measurements);
    if (measurements.kinds[X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE].count != 1U ||
        measurements.kinds[X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE].elapsed_ms != 5U)
        return trace_test_fail("failed persistence cannot retain an active measurement window");

    trace_active = false;
    x250_boot_trace_begin();
    for (u32 index = 0; index < TRACE_HISTORY_CAPACITY; index++)
        x250_boot_trace_timing(index, "bounded event", 0U, false, 0U,
                               "READY");
    exported_bytes = x250_boot_trace_export(
        exported, sizeof(exported), &exported_lines, &export_complete);
    if (!export_complete || exported_lines != TRACE_HISTORY_CAPACITY ||
        exported_bytes == 0U)
        return trace_test_fail("history is complete at its configured capacity");
    x250_boot_trace_timing(TRACE_HISTORY_CAPACITY, "bounded event", 0U,
                           false, 0U, "READY");
    exported_bytes = x250_boot_trace_export(
        exported, sizeof(exported), &exported_lines, &export_complete);
    if (export_complete || exported_lines != TRACE_HISTORY_CAPACITY ||
        exported_bytes == 0U)
        return trace_test_fail("overwritten history is marked incomplete");

    exported_bytes = x250_boot_trace_export(exported, 2U, &exported_lines,
                                             &export_complete);
    if (export_complete || exported_lines != 0U || exported_bytes != 0U)
        return trace_test_fail("short export buffer fails closed without partial lines");

    return 0;
}
