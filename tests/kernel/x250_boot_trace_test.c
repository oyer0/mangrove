/* SPDX-License-Identifier: GPL-3.0-or-later */
#define X250_BOOT_TRACE 1

#include <stdio.h>
#include <string.h>
#include <terminal_kernel.h>
#include <framebuffer.h>
#include <font.h>

#include "../../kernel/src/x250_boot_trace.c"

const u8 _binary_kernel_assets_font_psf_start[64] = {0};
const u8 _binary_kernel_assets_font_psf_end[64] = {0};

static char terminal_capture[32768];
static usize terminal_capture_length;
static terminal_color_t terminal_colors[256];
static u32 terminal_color_count;
static u32 terminal_rows = 74U;
static u32 terminal_columns = 168U;
static u32 terminal_batches_begun;
static u32 terminal_batches_ended;
static u32 terminal_write_calls;
static u32 terminal_fail_call;
static bool terminal_alternate_active;
static u32 framebuffer_pixel_calls;
static u32 framebuffer_flush_calls;

void framebuffer_put_pixel(u32 x, u32 y, u32 color)
{
    (void)x;
    (void)y;
    (void)color;
    framebuffer_pixel_calls++;
}

void framebuffer_flush_rect(u32 x, u32 y, u32 width, u32 height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    framebuffer_flush_calls++;
}

u32 framebuffer_width(void) { return 1366U; }
u32 framebuffer_height(void) { return 768U; }
u32 font_width(void) { return 8U; }
u32 font_height(void) { return 16U; }

u32 inl(u16 port)
{
    (void)port;
    return 0U;
}

void terminal_get_dimensions(u32 *rows, u32 *columns)
{
    if (rows) *rows = terminal_rows;
    if (columns) *columns = terminal_columns;
}

bool terminal_kernel_output_begin(void)
{
    if (terminal_alternate_active) return false;
    terminal_batches_begun++;
    return true;
}

void terminal_kernel_output_end(void)
{
    terminal_batches_ended++;
}

bool terminal_write_kernel_styled(const char *text, u64 length,
                                  terminal_color_t foreground,
                                  terminal_color_t background)
{
    terminal_write_calls++;
    if (terminal_fail_call == terminal_write_calls ||
        length > sizeof(terminal_capture) - terminal_capture_length)
        return false;
    memcpy(terminal_capture + terminal_capture_length, text, (usize)length);
    terminal_capture_length += (usize)length;
    if (terminal_color_count < sizeof(terminal_colors) /
                               sizeof(terminal_colors[0]))
        terminal_colors[terminal_color_count++] = foreground;
    (void)background;
    return true;
}

static void terminal_capture_reset(void)
{
    memset(terminal_capture, 0, sizeof(terminal_capture));
    memset(terminal_colors, 0, sizeof(terminal_colors));
    terminal_capture_length = 0U;
    terminal_color_count = 0U;
    terminal_batches_begun = 0U;
    terminal_batches_ended = 0U;
    terminal_write_calls = 0U;
    terminal_fail_call = 0U;
}

static int trace_test_fail(const char *message)
{
    fprintf(stderr, "x250 boot trace test failed: %s\n", message);
    return 1;
}

static void trace_test_result(u32 checkpoint, bool passed,
                              const char *description, bool required,
                              bool status_available, int status,
                              const char *state, const char *reason)
{
    /* Failure display hold is covered elsewhere; keep this fixture bounded. */
    trace_failure_hold_used = true;
    x250_boot_trace_result(checkpoint, passed, description, required,
                           status_available, status, state, reason);
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
    u32 position = 0U;

    for (u32 row = first; row < end; row++) {
        const char *line = trace_test_line(row);
        if (row != first) {
            if (position + 1U >= capacity) return false;
            output[position++] = ' ';
            if (line[0] == ' ' && line[1] == ' ') line += 2;
        }
        while (*line) {
            if (position + 1U >= capacity) return false;
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
    u32 root_end;

    trace_console_columns = 24U;
    trace_line_count = trace_line_start = 0U;
    trace_append_result(37U, false, "root filesystem", true, true, 2,
                        "FAILED", "Mangrove root volume not found");
    root_end = trace_line_count;
    if (root_end < 2U ||
        !trace_test_reconstruct(0U, root_end, reconstructed,
                                sizeof(reconstructed)) ||
        strcmp(reconstructed, expected_root) != 0)
        return trace_test_fail("complete root failure wraps without clipping");
    for (u32 row = 0U; row < root_end; row++)
        if (strlen(trace_test_line(row)) > trace_console_columns ||
            trace_test_state(row) != 'F')
            return trace_test_fail("wrapped failure rows fit console columns");

    trace_console_columns = 168U;
    trace_line_count = trace_line_start = 0U;
    trace_active = true;
    x250_boot_trace_xhci_summary(16U, 1U, 0U, 1U, 0U);
    x250_boot_trace_xhci_port(5U, 0x00000c01U, "FAILED", true, 7,
                              "reset-completion-change-missing");
    x250_boot_trace_usb_device(5U, 2U, 3U, 0x0781U, 0x5567U,
                               0U, 0U, 0U, 0U, false, NULL, 0);
    x250_boot_trace_msc_probe(5U, 2U, "inquiry", 9, false, false,
                              false, false, false, 0U, 0U, 0U, NULL);
    x250_boot_trace_block_device(0U, "SATA", 512U, 123456U, "ThinkDisk",
                                 0, 1U, 2U, 0U, 0U);
    x250_boot_trace_root_inventory(3U, 2U, 2U, 1U);
    if (trace_line_count != 6U ||
        strcmp(trace_test_line(0U),
            "42 [INFO] xHCI ports=16 ccs=1 enum=0 failed=1 usb-msd=0") != 0 ||
        strcmp(trace_test_line(1U),
            "43 [FAIL] port=5 PORTSC=00000c01 CCS=1 PED=0 PLS=0 PP=0 speed=3 state=FAILED rc=7 reason=reset-completion-change-missing") != 0 ||
        strcmp(trace_test_line(2U),
            "44 [INFO] USB p=5 s=2 sp=3 id=0781:5567 cls=00/00/00 bind=unsupported msc-ep=absent") != 0 ||
        strcmp(trace_test_line(3U),
            "45 [FAIL] MSC p=5 s=2 stage=inquiry rc=9 BOT=0 cap=0 blk=0 GPT=-") != 0 ||
        strcmp(trace_test_line(4U),
            "46 [INFO] block#0 SATA sectors=123456 size=512 GPT-rc=0 type=1 entries=2 reg=0 root=0 model=ThinkDisk") != 0 ||
        strcmp(trace_test_line(5U),
            "46 [INFO] root scan blocks=3 disks=2 gpt-parts=2 root-role=1") != 0)
        return trace_test_fail("USB discovery diagnostics retain stage and status");

    trace_line_count = trace_line_start = 0U;
    x250_boot_trace_usb_device(7U, 3U, 3U, 0x8087U, 0x0a2aU,
                               0U, 0U, 0U, 0U, false, "storage", 6);
    if (trace_line_count != 1U ||
        strcmp(trace_test_line(0U),
            "44 [FAIL] USB p=7 s=3 sp=3 id=8087:0a2a cls=00/00/00 bind=failed probe=storage rc=6") != 0)
        return trace_test_fail("low-level probe failure remains visible");

    trace_line_count = trace_line_start = 0U;
    x250_boot_trace_timing(12345U, "port probe complete", 7U, true,
                           10000U, "FAILED");
    if (trace_line_count != 1U ||
        strcmp(trace_test_line(0U),
            "47 [INFO] up=12345ms port probe complete p=7 dt=10000ms state=FAILED") != 0)
        return trace_test_fail("timestamped operation duration is retained");
    trace_line_count = trace_line_start = 0U;
    x250_boot_trace_timing_value(16399U, "boot log data write", "bytes",
                                 35446U, true, 9955U, "SAVED");
    if (trace_line_count != 1U ||
        strcmp(trace_test_line(0U),
            "47 [INFO] up=16399ms boot log data write bytes=35446 dt=9955ms state=SAVED") != 0)
        return trace_test_fail("boot-log timing and byte count are retained");

    {
        BOOT_INFO gop_boot = {
            .FramebufferWidth = 1366U,
            .FramebufferHeight = 768U,
            .PixelsPerScanLine = 1408U,
            .GopInitialMode = 0U,
            .GopCurrentMode = 3U,
            .GopRequestedMode = 3U,
            .GopSetModeAttempted = 1U,
            .GopFallbackUsed = 0U,
            .GopMaxMode = 3U,
            .GopModeCount = 3U,
            .GopModeInventoryComplete = 1U,
            .GopActivePixelFormat = 1U,
            .GopSetModeStatus = 0U,
            .GopRestoreModeStatus = 0x800000000000000eULL,
            .GopSelectionStatus = 0U,
            .GopModes = {
                { .Number = 0U, .HorizontalResolution = 640U,
                  .VerticalResolution = 480U, .PixelFormat = 1U,
                  .PixelsPerScanLine = 640U, .Valid = 1U,
                  .QueryStatus = 0U,
                  .ValidationStatus = 0U,
                  .FreeStatus = 0U },
                { .Number = 1U, .HorizontalResolution = 1366U,
                  .VerticalResolution = 768U, .PixelFormat = 1U,
                  .PixelsPerScanLine = 1408U, .Valid = 1U,
                  .QueryStatus = 0U,
                  .ValidationStatus = 0U,
                  .FreeStatus = 0U },
                { .Number = 2U, .HorizontalResolution = 1920U,
                  .VerticalResolution = 1080U, .PixelFormat = 1U,
                  .PixelsPerScanLine = 1920U, .Valid = 1U,
                  .QueryStatus = 0U,
                  .ValidationStatus = 0U,
                  .FreeStatus = 0U }
            }
        };

        trace_line_count = trace_line_start = 0U;
        trace_console_columns = 168U;
        trace_active = true;
        x250_boot_trace_event(&gop_boot, 37U, true,
                              "GOP inventory test source");
        x250_boot_trace_gop_modes();
        if (trace_line_count != 5U ||
            strcmp(trace_test_line(1U),
                "47 [INFO] GOP initial=0 request=3 set=0x0000000000000000 restore=0x800000000000000e actual=3 1366x768 fmt=1 pitch=1408 modes=3/3 complete=1") != 0 ||
            strcmp(trace_test_line(2U),
                "48 [INFO] GOP mode=0 query=0x0000000000000000 640x480 format=1 pitch=640 valid=1 validate=0x0000000000000000 free=0x0000000000000000") != 0 ||
            strcmp(trace_test_line(3U),
                "48 [INFO] GOP mode=1 query=0x0000000000000000 1366x768 format=1 pitch=1408 valid=1 validate=0x0000000000000000 free=0x0000000000000000") != 0 ||
            strcmp(trace_test_line(4U),
                "48 [INFO] GOP mode=2 query=0x0000000000000000 1920x1080 format=1 pitch=1920 valid=1 validate=0x0000000000000000 free=0x0000000000000000") != 0) {
            for (u32 index = 0U; index < trace_line_count; index++)
                fprintf(stderr, "trace[%u]=%s\n", index,
                        trace_test_line(index));
            return trace_test_fail("GOP trace records active mode and queried alternatives");
        }
    }

    trace_line_count = trace_line_start = 0U;
    {
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
    }
    if (trace_line_count != 2U ||
        !strstr(trace_test_line(0U), "USBblk r=2/17s w=11/88s") ||
        !strstr(trace_test_line(0U), "BOTfail=1") ||
        !strstr(trace_test_line(1U), "CBW/data/CSW=180/16000/900"))
        return trace_test_fail("storage operation counters and timings are retained");

    /* Event capture is presentation-free. The completed bounded report is
     * then submitted to the shared terminal in one colored batch. */
    trace_line_count = trace_line_start = 0U;
    trace_active = false;
    x250_boot_trace_begin();
    trace_test_result(21U, true, "required kernel bring-up", true, false,
                      0, "READY", NULL);
    x250_boot_trace_xhci_summary(15U, 4U, 2U, 2U, 1U);
    trace_test_result(37U, false, "root filesystem", true, true, 2,
                      "FAILED", "Mangrove root volume not found");
    trace_console_columns = 0U;
    trace_width = 1366U;
    trace_height = 768U;
    terminal_rows = 74U;
    terminal_columns = 168U;
    x250_boot_trace_console_ready();
    terminal_capture_reset();
    framebuffer_pixel_calls = framebuffer_flush_calls = 0U;
    x250_boot_trace_refresh();
    terminal_capture[terminal_capture_length] = '\0';
    if (!strstr(terminal_capture, "21 [PASS] required kernel bring-up") ||
        !strstr(terminal_capture, "42 [INFO] xHCI ports=15 ccs=4") ||
        !strstr(terminal_capture, "37 [FAIL] root filesystem") ||
        !strstr(terminal_capture, "Mangrove root volume not found") ||
        !strstr(terminal_capture, "47 [INFO] console fb=1366x768 grid=74x168 cell=8x16") ||
        terminal_batches_begun != 1U || terminal_batches_ended != 1U ||
        terminal_color_count != terminal_write_calls ||
        framebuffer_pixel_calls != 0U || framebuffer_flush_calls != 0U)
    {
        fprintf(stderr, "capture: %s calls=%u colors=%u batch=%u/%u framebuffer=%u/%u cols=%u rows=%u\n",
            terminal_capture, terminal_write_calls, terminal_color_count,
            terminal_batches_begun, terminal_batches_ended,
            framebuffer_pixel_calls, framebuffer_flush_calls,
            trace_console_columns, terminal_rows);
        return trace_test_fail("diagnostic report uses the normal console only");
    }
    bool green = false, red = false, cyan = false;
    for (u32 i = 0U; i < terminal_color_count; i++) {
        green |= terminal_colors[i] == TERMINAL_COLOR_GREEN;
        red |= terminal_colors[i] == TERMINAL_COLOR_BRIGHT_RED;
        cyan |= terminal_colors[i] == TERMINAL_COLOR_CYAN;
    }
    if (!green || !red || !cyan)
        return trace_test_fail("normal console severity colors are preserved");
    u32 writes_after_present = terminal_write_calls;
    x250_boot_trace_refresh();
    if (terminal_write_calls != writes_after_present)
        return trace_test_fail("report presentation is idempotent");

    /* A failed presentation is not declared complete and can be retried. */
    trace_line_count = trace_line_start = 0U;
    trace_active = false;
    x250_boot_trace_begin();
    trace_test_result(37U, false, "root filesystem", true, true, 2,
                      "FAILED", "Mangrove root volume not found");
    trace_report_presented = false;
    terminal_capture_reset();
    terminal_fail_call = 1U;
    x250_boot_trace_refresh();
    if (trace_report_presented || terminal_batches_ended != 1U)
        return trace_test_fail("failed terminal write remains retryable");
    terminal_capture_reset();
    x250_boot_trace_refresh();
    if (!trace_report_presented || !terminal_capture_length)
        return trace_test_fail("later successful presentation completes report");

    terminal_alternate_active = true;
    trace_report_presented = false;
    terminal_capture_reset();
    x250_boot_trace_refresh();
    if (trace_report_presented || terminal_batches_begun != 0U)
        return trace_test_fail("diagnostic presentation respects terminal ownership");
    terminal_alternate_active = false;

    /* Recent failures survive the bounded ring after high-volume discovery. */
    trace_line_count = trace_line_start = 0U;
    trace_active = false;
    x250_boot_trace_begin();
    x250_boot_trace_xhci_summary(200U, 200U, 0U, 200U, 0U);
    for (u32 port = 1U; port <= 200U; port++)
        x250_boot_trace_xhci_port(port, 0x00000c01U, "FAILED", true, 7,
                                  "reset-completion-change-missing");
    trace_test_result(37U, false, "root filesystem", true, true, 2,
                      "FAILED", "Mangrove root volume not found");
    trace_test_result(37U, false, "accounts", true, true, 4, "SKIPPED",
                      "Mangrove root volume not found");
    if (trace_line_count != TRACE_HISTORY_CAPACITY ||
        trace_test_state(trace_line_count - 1U) != 'F' ||
        strncmp(trace_test_line(trace_line_count - 1U),
                "37 [FAIL] accounts", 18U) != 0)
        return trace_test_fail("bounded history retains the latest failure");

    trace_line_count = trace_line_start = 0U;
    trace_active = false;
    x250_boot_trace_begin();
    trace_test_result(21U, true, "first event", false, false, 0,
                      "READY", NULL);
    x250_boot_trace_timing(1234U, "last event", 7U, true, 88U, "FAILED");
    {
        char exported[X250_BOOT_TRACE_EXPORT_CAPACITY];
        u32 exported_lines = 0U;
        bool export_complete = false;
        usize exported_bytes = x250_boot_trace_export(
            exported, sizeof(exported), &exported_lines, &export_complete);
        if (exported_bytes >= sizeof(exported))
            return trace_test_fail("trace export stays within capacity");
        exported[exported_bytes] = '\0';
        if (!export_complete || exported_lines != 2U ||
            !strstr(exported, "21 [PASS] first event optional state=READY\n") ||
            !strstr(exported, "47 [INFO] up=1234ms last event p=7 dt=88ms state=FAILED\n"))
            return trace_test_fail("persistent export preserves order and rows");

        exported_bytes = x250_boot_trace_export(exported, 2U,
            &exported_lines, &export_complete);
        if (export_complete || exported_lines != 0U || exported_bytes != 0U)
            return trace_test_fail("short export buffer fails closed");
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
        measurements.kinds[X250_BOOT_TRACE_MEASURE_RECOVERY_PIN_HANDOFF].elapsed_ms != 4U)
        return trace_test_fail("phase counters stop at their measurement boundary");

    puts("boot trace: bounded capture, failure retention and shared-console presentation passed");
    return 0;
}
