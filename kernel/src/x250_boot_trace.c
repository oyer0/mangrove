/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <x250_boot_trace.h>

#if X250_BOOT_TRACE
#include <framebuffer.h>
#include <idt.h>
#include <io.h>
#include <stddef.h>
#include <usb/xhci_storage_trace.h>

#define TRACE_HISTORY_LINES 40U
#define TRACE_HISTORY_CAPACITY X250_BOOT_TRACE_HISTORY_CAPACITY
#define TRACE_TEXT_CAPACITY X250_BOOT_TRACE_TEXT_CAPACITY
#define TRACE_MESSAGE_CAPACITY 1024U
#define TRACE_PANIC_ROWS 9U
#define TRACE_GLYPH_WIDTH 8U
#define TRACE_GLYPH_HEIGHT 16U
#define TRACE_DIAG_GLYPH_WIDTH 7U
#define TRACE_DIAG_GLYPH_HEIGHT 12U
#define TRACE_DIAG_CELL_WIDTH 8U
#define TRACE_DIAG_LINE_HEIGHT 14U
#define TRACE_PANEL_MARGIN_X 12U
#define TRACE_PANEL_MARGIN_Y 12U
#define TRACE_TEXT_MARGIN_X 20U
#define TRACE_PANEL_PADDING_Y 8U
#define TRACE_COLOR_BACKGROUND 0x00000000U
#define TRACE_COLOR_ENTER 0x00ffff00U
#define TRACE_COLOR_PASS 0x0000ff00U
#define TRACE_COLOR_FAIL 0x00ff4040U
#define TRACE_COLOR_TEXT 0x00ffffffU
#define TRACE_COLOR_PANIC_BACKGROUND 0x00400000U
#define TRACE_COLOR_PANIC_TITLE 0x00ffff00U
#define TRACE_TSC_SPIN_LIMIT 1000000000ULL
#define TRACE_FALLBACK_SPIN_LIMIT 100000000ULL
#define TRACE_PM_TIMER_SPIN_LIMIT 10000000ULL
#define TRACE_PM_TIMER_HZ 3579545ULL
#define TRACE_FAILURE_HOLD_MS 500ULL
#define TRACE_PM_TIMER_MASK_24 0x00ffffffU
#define TRACE_PM_TIMER_MASK_32 0xffffffffU
#define TRACE_ACPI_IDENTITY_LIMIT 0x40000000ULL
#define TRACE_ACPI_MAX_TABLE_LENGTH (1024U * 1024U)

typedef struct {
    u8 magic[4];
    u32 version;
    u32 header_size;
    u32 flags;
    u32 glyph_count;
    u32 bytes_per_glyph;
    u32 height;
    u32 width;
} trace_psf2_header_t;

extern const u8 _binary_kernel_assets_font_psf_start[];
extern const u8 _binary_kernel_assets_font_psf_end[];

static char trace_lines[TRACE_HISTORY_CAPACITY][TRACE_TEXT_CAPACITY];
static char trace_line_states[TRACE_HISTORY_CAPACITY];
static u32 trace_line_count;
static u32 trace_line_start;
static u64 trace_dropped_lines;
static volatile u32 *trace_framebuffer;
static u32 trace_width;
static u32 trace_height;
static u32 trace_stride;
static u32 trace_glyphs[95];
static bool trace_glyphs_ready;
static bool trace_use_framebuffer_api;
static bool trace_initialized;
static bool trace_active;
static bool trace_failure_hold_used;
static bool trace_pm_timer_checked;
static u16 trace_pm_timer_port;
static u32 trace_pm_timer_mask;
static BOOT_INFO *trace_boot_info;
static volatile u32 trace_measurements_active;
static volatile u64 trace_measurement_counts[X250_BOOT_TRACE_MEASURE_COUNT];
static volatile u64 trace_measurement_elapsed_ms[X250_BOOT_TRACE_MEASURE_COUNT];
static volatile u64 trace_measurement_maximum_ms[X250_BOOT_TRACE_MEASURE_COUNT];

static u32 trace_read_u32(const u8 *bytes)
{
    return (u32)bytes[0] | ((u32)bytes[1] << 8) |
           ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
}

static u64 trace_rdtsc(void)
{
    u32 low;
    u32 high;

    __asm__ volatile ("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((u64)high << 32) | low;
}

static void trace_cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(0));
}

static u64 trace_tsc_frequency(void)
{
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 max_leaf;

    trace_cpuid(0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf >= 0x15U) {
        u32 denominator;
        u32 numerator;
        u32 crystal_hz;

        trace_cpuid(0x15U, &denominator, &numerator, &crystal_hz, &edx);
        if (denominator && numerator && crystal_hz &&
            crystal_hz <= (u64)-1 / numerator)
            return ((u64)crystal_hz * numerator) / denominator;
    }

    if (max_leaf >= 0x16U) {
        trace_cpuid(0x16U, &eax, &ebx, &ecx, &edx);
        if (eax)
            return (u64)eax * 1000000ULL;
    }

    return 0;
}

static u16 trace_read_u16(const u8 *bytes)
{
    return (u16)bytes[0] | ((u16)bytes[1] << 8);
}

static u64 trace_read_u64(const u8 *bytes)
{
    return (u64)trace_read_u32(bytes) |
           ((u64)trace_read_u32(bytes + 4U) << 32);
}

static bool trace_acpi_checksum_valid(const u8 *bytes, u32 length)
{
    u8 checksum = 0;

    for (u32 i = 0; i < length; i++)
        checksum = (u8)(checksum + bytes[i]);
    return checksum == 0;
}

static bool trace_pm_timer_init(BOOT_INFO *boot_info)
{
    static const u8 rsdp_signature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    const u8 *rsdp = (const u8 *)boot_info->Rsdp;
    const u8 *root;
    u64 root_address;
    u32 root_length;
    u32 entry_size;
    bool xsdt;

    if (trace_pm_timer_checked)
        return trace_pm_timer_port != 0;
    trace_pm_timer_checked = true;
    if (!rsdp || (u64)(uintptr_t)rsdp >
                 TRACE_ACPI_IDENTITY_LIMIT - 36U)
        return false;
    for (u32 i = 0; i < sizeof(rsdp_signature); i++)
        if (rsdp[i] != rsdp_signature[i])
            return false;
    if (!trace_acpi_checksum_valid(rsdp, 20U))
        return false;

    xsdt = rsdp[15] >= 2U;
    if (xsdt) {
        u32 rsdp_length = trace_read_u32(rsdp + 20U);
        if (rsdp_length < 36U || rsdp_length > 4096U ||
            (u64)(uintptr_t)rsdp >
                TRACE_ACPI_IDENTITY_LIMIT - rsdp_length ||
            !trace_acpi_checksum_valid(rsdp, rsdp_length))
            return false;
        root_address = trace_read_u64(rsdp + 24U);
    } else {
        root_address = trace_read_u32(rsdp + 16U);
    }

    if (!root_address || root_address >
        TRACE_ACPI_IDENTITY_LIMIT - 36U)
        return false;
    root = (const u8 *)(uintptr_t)root_address;
    if (xsdt) {
        if (root[0] != 'X' || root[1] != 'S' ||
            root[2] != 'D' || root[3] != 'T')
            return false;
        entry_size = sizeof(u64);
    } else {
        if (root[0] != 'R' || root[1] != 'S' ||
            root[2] != 'D' || root[3] != 'T')
            return false;
        entry_size = sizeof(u32);
    }
    root_length = trace_read_u32(root + 4U);
    if (root_length < 36U ||
        root_length > TRACE_ACPI_MAX_TABLE_LENGTH ||
        root_length > TRACE_ACPI_IDENTITY_LIMIT - root_address ||
        (root_length - 36U) % entry_size != 0 ||
        !trace_acpi_checksum_valid(root, root_length))
        return false;

    for (u32 offset = 36U; offset < root_length; offset += entry_size) {
        u64 table_address = entry_size == sizeof(u64)
            ? trace_read_u64(root + offset)
            : trace_read_u32(root + offset);
        const u8 *table;
        u32 table_length;
        u32 flags;
        u32 port;

        if (!table_address ||
            table_address > TRACE_ACPI_IDENTITY_LIMIT - 116U)
            continue;
        table = (const u8 *)(uintptr_t)table_address;
        if (table[0] != 'F' || table[1] != 'A' ||
            table[2] != 'C' || table[3] != 'P')
            continue;
        table_length = trace_read_u32(table + 4U);
        if (table_length < 116U ||
            table_length > TRACE_ACPI_MAX_TABLE_LENGTH ||
            table_length > TRACE_ACPI_IDENTITY_LIMIT - table_address ||
            !trace_acpi_checksum_valid(table, table_length))
            continue;

        flags = trace_read_u32(table + 112U);
        if (flags & (1U << 20))
            continue;
        port = trace_read_u32(table + 76U);
        if (table_length >= 220U && table[208] == 1U &&
            table[209] >= 24U && table[210] == 0U) {
            u64 extended_port = trace_read_u64(table + 212U);
            if (extended_port && extended_port <= 0xffffU)
                port = (u32)extended_port;
        }
        if (!port || port > 0xffffU || table[91] < 4U)
            continue;

        trace_pm_timer_port = (u16)port;
        trace_pm_timer_mask = flags & (1U << 8)
            ? TRACE_PM_TIMER_MASK_32 : TRACE_PM_TIMER_MASK_24;
        return true;
    }

    return false;
}

static bool trace_delay_pm_timer(BOOT_INFO *boot_info)
{
    if (!trace_pm_timer_init(boot_info))
        return false;

    u32 start = inl(trace_pm_timer_port) & trace_pm_timer_mask;
    u64 target = TRACE_PM_TIMER_HZ * TRACE_FAILURE_HOLD_MS / 1000ULL;
    for (u64 spin = 0; spin < TRACE_PM_TIMER_SPIN_LIMIT; spin++) {
        u32 current = inl(trace_pm_timer_port) & trace_pm_timer_mask;
        if (((current - start) & trace_pm_timer_mask) >= target)
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void trace_delay(BOOT_INFO *boot_info)
{
    u64 frequency = trace_tsc_frequency();

    if (frequency &&
        frequency <= (u64)-1 / TRACE_FAILURE_HOLD_MS) {
        u64 start = trace_rdtsc();
        u64 target = frequency * TRACE_FAILURE_HOLD_MS / 1000ULL;

        for (u64 spin = 0; spin < TRACE_TSC_SPIN_LIMIT; spin++) {
            if (trace_rdtsc() - start >= target)
                return;
            __asm__ volatile ("pause");
        }
    } else if (trace_delay_pm_timer(boot_info)) {
        return;
    }

    for (u64 spin = 0; spin < TRACE_FALLBACK_SPIN_LIMIT; spin++)
        __asm__ volatile ("pause");
}

static void trace_init_glyphs(void)
{
    const u8 *font = _binary_kernel_assets_font_psf_start;
    usize font_size = (usize)(_binary_kernel_assets_font_psf_end - font);
    u32 header_size;
    u32 flags;
    u32 glyph_count;
    u32 bytes_per_glyph;
    usize table_offset;

    for (u32 i = 0; i < 95U; i++)
        trace_glyphs[i] = i + 32U;

    if (font_size < 32U || font[0] != 0x72U || font[1] != 0xb5U ||
        font[2] != 0x4aU || font[3] != 0x86U)
        return;

    header_size = trace_read_u32(font + 8U);
    flags = trace_read_u32(font + 12U);
    glyph_count = trace_read_u32(font + 16U);
    bytes_per_glyph = trace_read_u32(font + 20U);
    if (header_size < 32U || header_size > font_size || !glyph_count ||
        !bytes_per_glyph ||
        glyph_count > (font_size - header_size) / bytes_per_glyph)
        return;

    if (!(flags & 1U))
        return;
    table_offset = header_size + (usize)glyph_count * bytes_per_glyph;
    for (u32 glyph = 0; glyph < glyph_count && table_offset < font_size;
         glyph++) {
        bool sequence = false;
        while (table_offset < font_size) {
            u8 value = font[table_offset++];

            if (value == 0xffU)
                break;
            if (value == 0xfeU) {
                sequence = true;
                continue;
            }
            if (!sequence && value >= 32U && value <= 126U)
                trace_glyphs[value - 32U] = glyph;
        }
    }
}

static void trace_put_pixel(u32 x, u32 y, u32 color)
{
    if (x >= trace_width || y >= trace_height)
        return;
    if (trace_use_framebuffer_api) {
        framebuffer_put_pixel(x, y, color);
        return;
    }
    trace_framebuffer[(u64)y * trace_stride + x] = color;
}

/* GOP may remain at 640x480 even on a larger LCD. */
static u32 trace_visible_rows(void)
{
    u32 overhead = TRACE_PANEL_PADDING_Y * 2U + TRACE_PANEL_MARGIN_Y * 2U;
    if (trace_height <= overhead)
        return 0;
    u32 rows = (trace_height - overhead) / TRACE_DIAG_LINE_HEIGHT;
    return rows < TRACE_HISTORY_LINES ? rows : TRACE_HISTORY_LINES;
}

static void trace_fill_panel(void)
{
    u32 panel_height = trace_visible_rows() * TRACE_DIAG_LINE_HEIGHT +
                       TRACE_PANEL_PADDING_Y * 2U;
    u32 start_x = TRACE_PANEL_MARGIN_X;
    u32 end_x;
    u32 end_y;
    u32 start_y;

    if (!trace_framebuffer || trace_width <= start_x * 2U ||
        trace_height < panel_height + TRACE_PANEL_MARGIN_Y ||
        trace_stride < trace_width)
        return;

    end_x = trace_width - TRACE_PANEL_MARGIN_X;
    start_y = trace_height - panel_height - TRACE_PANEL_MARGIN_Y;
    end_y = trace_height - TRACE_PANEL_MARGIN_Y;
    for (u32 y = start_y; y < end_y; y++) {
        for (u32 x = start_x; x < end_x; x++)
            trace_put_pixel(x, y, TRACE_COLOR_BACKGROUND);
    }
}

static void trace_draw_diagnostic_character(u8 character, u32 x, u32 y,
                                            u32 color)
{
    const u8 *font = _binary_kernel_assets_font_psf_start;
    usize font_size = (usize)(_binary_kernel_assets_font_psf_end - font);
    u32 header_size;
    u32 glyph_count;
    u32 bytes_per_glyph;
    u32 glyph_width;
    u32 glyph_height;
    u32 glyph;
    usize glyph_offset;

    if (character < 32U || character > 126U)
        character = '?';
    glyph = trace_glyphs[character - 32U];
    header_size = trace_read_u32(font + 8U);
    glyph_count = trace_read_u32(font + 16U);
    bytes_per_glyph = trace_read_u32(font + 20U);
    glyph_height = trace_read_u32(font + 24U);
    glyph_width = trace_read_u32(font + 28U);
    if (glyph >= glyph_count || glyph_width == 0 || glyph_height == 0 ||
        glyph_width > TRACE_GLYPH_WIDTH || glyph_height > TRACE_GLYPH_HEIGHT ||
        bytes_per_glyph < glyph_height)
        return;
    glyph_offset = header_size + (usize)glyph * bytes_per_glyph;
    if (glyph_offset > font_size ||
        bytes_per_glyph > font_size - glyph_offset)
        return;

    const u8 *bitmap = font + glyph_offset;
    for (u32 py = 0; py < TRACE_DIAG_GLYPH_HEIGHT; py++) {
        u32 source_y_start = py * glyph_height / TRACE_DIAG_GLYPH_HEIGHT;
        u32 source_y_end = (py + 1U) * glyph_height /
                           TRACE_DIAG_GLYPH_HEIGHT;
        for (u32 px = 0; px < TRACE_DIAG_GLYPH_WIDTH; px++) {
            u32 source_x_start = px * glyph_width /
                                 TRACE_DIAG_GLYPH_WIDTH;
            u32 source_x_end = (px + 1U) * glyph_width /
                               TRACE_DIAG_GLYPH_WIDTH;
            bool set = false;

            for (u32 source_y = source_y_start;
                 source_y < source_y_end; source_y++) {
                u8 bits = bitmap[source_y];
                for (u32 source_x = source_x_start;
                     source_x < source_x_end; source_x++) {
                    if (bits & (0x80U >> source_x)) {
                        set = true;
                        break;
                    }
                }
                if (set)
                    break;
            }
            if (set)
                trace_put_pixel(x + px, y + py, color);
        }
    }
}

static void trace_draw_character(u8 character, u32 x, u32 y, u32 color)
{
    const u8 *font = _binary_kernel_assets_font_psf_start;
    usize font_size = (usize)(_binary_kernel_assets_font_psf_end - font);
    u32 header_size;
    u32 glyph_count;
    u32 bytes_per_glyph;
    u32 glyph_width;
    u32 glyph_height;
    u32 glyph;
    u32 row_bytes;
    usize glyph_offset;

    if (character < 32U || character > 126U)
        character = '?';
    glyph = trace_glyphs[character - 32U];
    header_size = trace_read_u32(font + 8U);
    glyph_count = trace_read_u32(font + 16U);
    bytes_per_glyph = trace_read_u32(font + 20U);
    glyph_height = trace_read_u32(font + 24U);
    glyph_width = trace_read_u32(font + 28U);
    row_bytes = (glyph_width + 7U) / 8U;
    if (glyph >= glyph_count || glyph_width == 0 || glyph_height == 0 ||
        glyph_width > TRACE_GLYPH_WIDTH || glyph_height > TRACE_GLYPH_HEIGHT ||
        !row_bytes || bytes_per_glyph < row_bytes * glyph_height)
        return;
    glyph_offset = header_size + (usize)glyph * bytes_per_glyph;
    if (glyph_offset > font_size ||
        bytes_per_glyph > font_size - glyph_offset)
        return;

    const u8 *bitmap = font + glyph_offset;
    for (u32 py = 0; py < glyph_height; py++) {
        u8 bits = bitmap[py * row_bytes];
        for (u32 px = 0; px < glyph_width; px++) {
            if (bits & (0x80U >> (px & 7U)))
                trace_put_pixel(x + px, y + py, color);
        }
    }
}

static void trace_render(void)
{
    u32 visible_lines;
    u32 panel_height = trace_visible_rows() * TRACE_DIAG_LINE_HEIGHT +
                       TRACE_PANEL_PADDING_Y * 2U;
    u32 start_y;
    u32 first_line;

    if (!trace_glyphs_ready) {
        trace_init_glyphs();
        trace_glyphs_ready = true;
    }
    trace_fill_panel();
    if (!trace_framebuffer || trace_height < panel_height +
                               TRACE_PANEL_MARGIN_Y ||
        trace_width <= TRACE_TEXT_MARGIN_X * 2U)
        return;

    visible_lines = trace_line_count < trace_visible_rows()
        ? trace_line_count : trace_visible_rows();
    first_line = trace_visible_rows() - visible_lines;
    start_y = trace_height - panel_height - TRACE_PANEL_MARGIN_Y +
              TRACE_PANEL_PADDING_Y + first_line * TRACE_DIAG_LINE_HEIGHT;

    u32 first_visible = trace_line_count - visible_lines;
    for (u32 line_index = 0; line_index < visible_lines; line_index++) {
        u32 row = (trace_line_start + first_visible + line_index) %
                  TRACE_HISTORY_CAPACITY;
        const char *line = trace_lines[row];
        u32 y = start_y + line_index * TRACE_DIAG_LINE_HEIGHT;
        u32 color = trace_line_states[row] == 'P' ? TRACE_COLOR_PASS :
                    trace_line_states[row] == 'F' ? TRACE_COLOR_FAIL :
                                                     TRACE_COLOR_ENTER;
        for (u32 column = 0; column < TRACE_TEXT_CAPACITY - 1U &&
                             line[column] != '\0'; column++) {
            trace_draw_diagnostic_character(
                (u8)line[column], TRACE_TEXT_MARGIN_X +
                column * TRACE_DIAG_CELL_WIDTH, y, color);
        }
    }

    if (trace_use_framebuffer_api)
        framebuffer_flush_rect(TRACE_PANEL_MARGIN_X,
                               trace_height - panel_height -
                                   TRACE_PANEL_MARGIN_Y,
                               trace_width - TRACE_PANEL_MARGIN_X * 2U,
                               panel_height);
}

static void trace_panic_draw_text(const char *text, u32 row, u32 color)
{
    u32 columns = trace_width / TRACE_GLYPH_WIDTH;
    u32 x = 0;
    u32 y = row * TRACE_GLYPH_HEIGHT;

    while (*text && x < columns) {
        trace_draw_character((u8)*text++, x * TRACE_GLYPH_WIDTH, y, color);
        x++;
    }
}

static void trace_panic_draw_hex(const char *label, u64 value, u32 row)
{
    static const char digits[] = "0123456789abcdef";
    char line[TRACE_TEXT_CAPACITY];
    u32 length = 0;
    u32 first = 0;

    while (*label && length < sizeof(line) - 1U)
        line[length++] = *label++;
    line[length++] = '0';
    line[length++] = 'x';
    for (u32 index = 0; index < 16U; index++) {
        u32 shift = (15U - index) * 4U;
        line[length++] = digits[(value >> shift) & 0x0fU];
    }
    line[length] = '\0';
    while (first + 1U < length && line[first] == '0' &&
           line[first + 1U] != 'x')
        first++;
    trace_panic_draw_text(line + first, row, TRACE_COLOR_TEXT);
}

static void trace_render_panic(const char *message,
                               const struct cpu_registers *regs,
                               u64 cr2, u64 cr3)
{
    u32 panel_height = TRACE_PANIC_ROWS * TRACE_GLYPH_HEIGHT;

    if (!trace_initialized || !trace_framebuffer ||
        trace_width < TRACE_GLYPH_WIDTH || trace_height < panel_height ||
        trace_stride < trace_width)
        return;

    for (u32 y = 0; y < panel_height; y++) {
        for (u32 x = 0; x < trace_width; x++)
            trace_put_pixel(x, y, TRACE_COLOR_PANIC_BACKGROUND);
    }
    trace_panic_draw_text("KERNEL PANIC SUMMARY", 0U,
                          TRACE_COLOR_PANIC_TITLE);
    trace_panic_draw_text(message ? message : "(null)", 1U,
                          TRACE_COLOR_TEXT);
    if (regs) {
        trace_panic_draw_hex("VECTOR: ", regs->vec_no, 2U);
        trace_panic_draw_hex("ERROR: ", regs->err_code, 3U);
        trace_panic_draw_hex("RIP: ", regs->rip, 4U);
        trace_panic_draw_hex("CR2: ", cr2, 5U);
        trace_panic_draw_hex("CR3: ", cr3, 6U);
        trace_panic_draw_hex("RSP: ", regs->rsp, 7U);
        trace_panic_draw_hex("RFLAGS: ", regs->rflags, 8U);
    }
    if (trace_use_framebuffer_api)
        framebuffer_flush_rect(0U, 0U, trace_width, panel_height);
}

static void trace_append_text(char *line, u32 *position, const char *text)
{
    if (!text)
        return;
    while (*text && *position < TRACE_MESSAGE_CAPACITY - 1U)
        line[(*position)++] = *text++;
}

static void trace_append_u32(char *line, u32 *position, u32 value)
{
    char digits[10];
    u32 count = 0;

    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(digits));
    while (count && *position < TRACE_MESSAGE_CAPACITY - 1U)
        line[(*position)++] = digits[--count];
}

static void trace_append_u64(char *line, u32 *position, u64 value)
{
    char digits[20];
    u32 count = 0;

    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(digits));
    while (count && *position < TRACE_MESSAGE_CAPACITY - 1U)
        line[(*position)++] = digits[--count];
}

static void trace_append_hex(char *line, u32 *position, u32 value,
                             u32 digits)
{
    static const char alphabet[] = "0123456789abcdef";

    while (digits && *position < TRACE_MESSAGE_CAPACITY - 1U) {
        u32 shift = (digits - 1U) * 4U;
        line[(*position)++] = alphabet[(value >> shift) & 0x0fU];
        digits--;
    }
}

static void trace_append_i32(char *line, u32 *position, int value)
{
    u32 magnitude;

    if (value < 0) {
        trace_append_text(line, position, "-");
        magnitude = (u32)(-(value + 1)) + 1U;
    } else {
        magnitude = (u32)value;
    }
    trace_append_u32(line, position, magnitude);
}

static bool trace_text_equal(const char *left, const char *right)
{
    if (!left || !right)
        return left == right;
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static void trace_append_prefix(char *line, u32 *position,
                                u32 checkpoint, char state)
{
    line[(*position)++] = (char)('0' + (checkpoint / 10U) % 10U);
    line[(*position)++] = (char)('0' + checkpoint % 10U);
    line[(*position)++] = ' ';
    line[(*position)++] = '[';
    if (state == 'P')
        trace_append_text(line, position, "PASS");
    else if (state == 'F')
        trace_append_text(line, position, "FAIL");
    else if (state == 'I')
        trace_append_text(line, position, "INFO");
    else
        trace_append_text(line, position, "ENTER");
    line[(*position)++] = ']';
    line[(*position)++] = ' ';
}

static void trace_store_wrapped(const char *message, char state)
{
    u32 columns = TRACE_TEXT_CAPACITY - 1U;
    u32 x_margin = TRACE_TEXT_MARGIN_X * 2U;
    u32 message_offset = 0;

    if (trace_width > x_margin) {
        u32 available = (trace_width - x_margin) / TRACE_DIAG_CELL_WIDTH;
        if (available && available < columns)
            columns = available;
    }
    if (!columns)
        columns = 1U;

    while (message[message_offset]) {
        u32 slot;
        char *line;
        bool continuation = message_offset != 0U;
        u32 indent = continuation && columns > 2U ? 2U : 0U;
        u32 available = columns - indent;
        u32 length = 0;
        u32 next_offset;

        while (message[message_offset + length] && length < available)
            length++;
        if (message[message_offset + length]) {
            u32 split = length;
            while (split > 0U && message[message_offset + split - 1U] != ' ')
                split--;
            if (split > 0U)
                length = split - 1U;
        }
        if (!length)
            length = available;

        if (trace_line_count < TRACE_HISTORY_CAPACITY) {
            slot = (trace_line_start + trace_line_count) %
                   TRACE_HISTORY_CAPACITY;
            trace_line_count++;
        } else {
            slot = trace_line_start;
            trace_line_start = (trace_line_start + 1U) %
                               TRACE_HISTORY_CAPACITY;
            trace_dropped_lines++;
        }
        line = trace_lines[slot];
        for (u32 index = 0; index < indent; index++)
            line[index] = ' ';
        for (u32 index = 0; index < length; index++)
            line[indent + index] = message[message_offset + index];
        line[indent + length] = '\0';
        trace_line_states[slot] = state;

        next_offset = message_offset + length;
        while (message[next_offset] == ' ')
            next_offset++;
        if (next_offset <= message_offset)
            return;
        message_offset = next_offset;
    }
}

static void trace_append(u32 checkpoint, bool passed,
                         const char *description)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    trace_append_prefix(message, &position, checkpoint,
                        passed ? 'P' : 'E');
    trace_append_text(message, &position, description);
    message[position] = '\0';
    trace_store_wrapped(message, passed ? 'P' : 'E');
}

static void trace_append_result(u32 checkpoint, bool passed,
                                const char *description, bool required,
                                bool status_available, int status,
                                const char *state, const char *reason)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    trace_append_prefix(message, &position, checkpoint,
                        passed ? 'P' : 'F');
    trace_append_text(message, &position, description);
    trace_append_text(message, &position,
                      required ? " required" : " optional");
    if (status_available) {
        trace_append_text(message, &position, " rc=");
        trace_append_i32(message, &position, status);
    }
    if (state) {
        trace_append_text(message, &position, " state=");
        trace_append_text(message, &position, state);
    }
    if (reason) {
        trace_append_text(message, &position, " reason=");
        trace_append_text(message, &position, reason);
    }
    message[position] = '\0';
    trace_store_wrapped(message, passed ? 'P' : 'F');
}

static void trace_render_if_active(void)
{
    if (trace_active)
        trace_render();
}

void x250_boot_trace_begin(void)
{
    if (trace_active)
        return;
    trace_line_count = 0;
    trace_line_start = 0;
    trace_dropped_lines = 0;
    trace_failure_hold_used = false;
    trace_active = true;
}

void x250_boot_trace_measurements_begin(void)
{
    __atomic_store_n(&trace_measurements_active, 0U, __ATOMIC_RELEASE);
    for (u32 index = 0; index < X250_BOOT_TRACE_MEASURE_COUNT; index++) {
        __atomic_store_n(&trace_measurement_counts[index], 0ULL,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&trace_measurement_elapsed_ms[index], 0ULL,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&trace_measurement_maximum_ms[index], 0ULL,
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&trace_measurements_active, 1U, __ATOMIC_RELEASE);
}

void x250_boot_trace_measurement_note(
    x250_boot_trace_measurement_kind_t kind, u64 elapsed_ms)
{
    if ((u32)kind >= X250_BOOT_TRACE_MEASURE_COUNT ||
        !__atomic_load_n(&trace_measurements_active, __ATOMIC_ACQUIRE))
        return;
    __atomic_fetch_add(&trace_measurement_counts[kind], 1ULL,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&trace_measurement_elapsed_ms[kind], elapsed_ms,
                       __ATOMIC_RELAXED);
    xhci_storage_trace_atomic_max(&trace_measurement_maximum_ms[kind],
                                  elapsed_ms);
}

void x250_boot_trace_measurements_end(
    x250_boot_trace_measurement_snapshot_t *snapshot)
{
    __atomic_store_n(&trace_measurements_active, 0U, __ATOMIC_RELEASE);
    if (!snapshot)
        return;
    for (u32 index = 0; index < X250_BOOT_TRACE_MEASURE_COUNT; index++) {
        snapshot->kinds[index].count = __atomic_load_n(
            &trace_measurement_counts[index], __ATOMIC_ACQUIRE);
        snapshot->kinds[index].elapsed_ms = __atomic_load_n(
            &trace_measurement_elapsed_ms[index], __ATOMIC_ACQUIRE);
        snapshot->kinds[index].maximum_ms = __atomic_load_n(
            &trace_measurement_maximum_ms[index], __ATOMIC_ACQUIRE);
    }
}

usize x250_boot_trace_export(char *buffer, usize capacity, u32 *line_count,
                             bool *complete)
{
    usize used = 0;
    u32 copied = 0;
    bool full = trace_active && trace_dropped_lines == 0U;

    if (line_count)
        *line_count = 0;
    if (!buffer && capacity)
        full = false;

    if (buffer) {
        for (u32 index = 0; index < trace_line_count; index++) {
            u32 slot = (trace_line_start + index) % TRACE_HISTORY_CAPACITY;
            usize length = 0;

            while (length < TRACE_TEXT_CAPACITY &&
                   trace_lines[slot][length])
                length++;
            if (length == TRACE_TEXT_CAPACITY ||
                length + 1U > capacity - used) {
                full = false;
                break;
            }
            __builtin_memcpy(buffer + used, trace_lines[slot], length);
            used += length;
            buffer[used++] = '\n';
            copied++;
        }
    } else if (trace_line_count) {
        full = false;
    }

    if (line_count)
        *line_count = copied;
    if (complete)
        *complete = full && copied == trace_line_count;
    return used;
}

void x250_boot_trace_refresh(void)
{
    /* Drop callback chatter from the final view. Keep discovery rows and
       failures (including their wrapped continuations) in original order. */
    if (trace_active) {
        static char saved[TRACE_HISTORY_CAPACITY][TRACE_TEXT_CAPACITY];
        static char states[TRACE_HISTORY_CAPACITY];
        u32 count = 0;
        bool selected = false;
        for (u32 i = 0; i < trace_line_count; i++) {
            u32 slot = (trace_line_start + i) % TRACE_HISTORY_CAPACITY;
            const char *line = trace_lines[slot];
            if (line[0] != ' ')
                selected = ((line[0] == '2' && line[1] == '1') ||
                            (line[0] == '4' && line[1] >= '2' &&
                             line[1] <= '7') ||
                            trace_line_states[slot] == 'F');
            if (selected) {
                __builtin_memcpy(saved[count], line, TRACE_TEXT_CAPACITY);
                states[count++] = trace_line_states[slot];
            }
        }
        __builtin_memcpy(trace_lines, saved, count * TRACE_TEXT_CAPACITY);
        __builtin_memcpy(trace_line_states, states, count);
        trace_line_start = 0;
        trace_line_count = count;
        char message[TRACE_MESSAGE_CAPACITY];
        u32 position = 0;
        trace_append_prefix(message, &position, 47U, 'I');
        trace_append_text(message, &position, "USB ROOT REPORT fb=");
        trace_append_u32(message, &position, trace_width);
        trace_append_text(message, &position, "x");
        trace_append_u32(message, &position, trace_height);
        message[position] = '\0';
        trace_store_wrapped(message, 'I');
    }
    trace_render_if_active();
}

void x250_boot_trace_event(BOOT_INFO *boot_info, u32 checkpoint,
                           bool passed, const char *description)
{
    if (boot_info)
        trace_boot_info = boot_info;
    if (!trace_initialized && boot_info) {
        trace_framebuffer = (volatile u32 *)boot_info->FramebufferBase;
        trace_width = boot_info->FramebufferWidth;
        trace_height = boot_info->FramebufferHeight;
        trace_stride = boot_info->PixelsPerScanLine;
        if (!trace_glyphs_ready) {
            trace_init_glyphs();
            trace_glyphs_ready = true;
        }
        trace_initialized = true;
    }

    if (!trace_active)
        return;

    trace_append(checkpoint, passed, description);
    trace_render();
}

void x250_boot_trace_stage(u32 checkpoint, bool passed,
                           const char *description)
{
    if (trace_boot_info)
        x250_boot_trace_event(trace_boot_info, checkpoint, passed,
                              description);
}

void x250_boot_trace_timing(u64 uptime_ms, const char *event, u32 port,
                            bool duration_valid, u64 duration_ms,
                            const char *state)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "up=");
    trace_append_u64(message, &position, uptime_ms);
    trace_append_text(message, &position, "ms ");
    trace_append_text(message, &position, event ? event : "event");
    if (port) {
        trace_append_text(message, &position, " p=");
        trace_append_u32(message, &position, port);
    }
    if (duration_valid) {
        trace_append_text(message, &position, " dt=");
        trace_append_u64(message, &position, duration_ms);
        trace_append_text(message, &position, "ms");
    }
    if (state) {
        trace_append_text(message, &position, " state=");
        trace_append_text(message, &position, state);
    }
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_timing_value(u64 uptime_ms, const char *event,
                                  const char *value_name, u64 value,
                                  bool duration_valid, u64 duration_ms,
                                  const char *state)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "up=");
    trace_append_u64(message, &position, uptime_ms);
    trace_append_text(message, &position, "ms ");
    trace_append_text(message, &position, event ? event : "event");
    if (value_name) {
        trace_append_text(message, &position, " ");
        trace_append_text(message, &position, value_name);
        trace_append_text(message, &position, "=");
        trace_append_u64(message, &position, value);
    }
    if (duration_valid) {
        trace_append_text(message, &position, " dt=");
        trace_append_u64(message, &position, duration_ms);
        trace_append_text(message, &position, "ms");
    }
    if (state) {
        trace_append_text(message, &position, " state=");
        trace_append_text(message, &position, state);
    }
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_storage_io(
    u64 uptime_ms, const char *phase,
    const struct xhci_storage_trace_snapshot *io)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active || !io)
        return;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "up=");
    trace_append_u64(message, &position, uptime_ms);
    trace_append_text(message, &position, "ms ");
    trace_append_text(message, &position, phase ? phase : "storage");
    trace_append_text(message, &position, " USBblk r=");
    trace_append_u64(message, &position, io->block_read_requests);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->block_read_sectors);
    trace_append_text(message, &position, "s w=");
    trace_append_u64(message, &position, io->block_write_requests);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->block_write_sectors);
    trace_append_text(message, &position, "s sizeR=");
    trace_append_u64(message, &position, io->block_read_min_sectors);
    trace_append_text(message, &position, "-");
    trace_append_u64(message, &position, io->block_read_max_sectors);
    trace_append_text(message, &position, " sizeW=");
    trace_append_u64(message, &position, io->block_write_min_sectors);
    trace_append_text(message, &position, "-");
    trace_append_u64(message, &position, io->block_write_max_sectors);
    trace_append_text(message, &position, " BOT=");
    trace_append_u64(message, &position, io->bot_commands);
    trace_append_text(message, &position, " R10=");
    trace_append_u64(message, &position, io->bot_read10);
    trace_append_text(message, &position, " W10=");
    trace_append_u64(message, &position, io->bot_write10);
    trace_append_text(message, &position, " other=");
    trace_append_u64(message, &position, io->bot_other);
    trace_append_text(message, &position, " iofail=");
    trace_append_u64(message, &position, io->block_read_failures);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->block_write_failures);
    trace_append_text(message, &position, " BOTfail=");
    trace_append_u64(message, &position, io->bot_failed);
    message[position] = '\0';
    trace_store_wrapped(message, 'I');

    position = 0;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "up=");
    trace_append_u64(message, &position, uptime_ms);
    trace_append_text(message, &position, "ms ");
    trace_append_text(message, &position, phase ? phase : "storage");
    trace_append_text(message, &position, " ms USBblk=");
    trace_append_u64(message, &position, io->block_elapsed_ms);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->block_max_ms);
    trace_append_text(message, &position, " BOT=");
    trace_append_u64(message, &position, io->bot_elapsed_ms);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->bot_commands ?
                     io->bot_elapsed_ms / io->bot_commands : 0U);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->bot_min_ms);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->bot_max_ms);
    trace_append_text(message, &position, " CBW/data/CSW=");
    trace_append_u64(message, &position, io->bot_cbw_ms);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->bot_data_ms);
    trace_append_text(message, &position, "/");
    trace_append_u64(message, &position, io->bot_csw_ms);
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_result(u32 checkpoint, bool passed,
                            const char *description, bool required,
                            bool status_available, int status,
                            const char *state, const char *reason)
{
    if (!trace_active)
        return;

    trace_append_result(checkpoint, passed, description, required,
                        status_available, status, state, reason);
    trace_render();
    if (!passed && required && !trace_failure_hold_used &&
        !trace_text_equal(state, "SKIPPED")) {
        trace_failure_hold_used = true;
        trace_delay(trace_boot_info);
    }
}

void x250_boot_trace_storage_inventory(u32 block_devices,
                                       u32 usb_mass_storage_devices,
                                       bool xhci_ready)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    bool passed = block_devices != 0U;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 39U, passed ? 'P' : 'F');
    trace_append_text(message, &position, "storage inventory block=");
    trace_append_u32(message, &position, block_devices);
    trace_append_text(message, &position, " usb-msd=");
    trace_append_u32(message, &position, usb_mass_storage_devices);
    trace_append_text(message, &position, " xHCI=");
    trace_append_text(message, &position,
                      xhci_ready ? "READY" : "NOT-READY");
    message[position] = '\0';
    trace_store_wrapped(message, passed ? 'P' : 'F');
    trace_render();
}

void x250_boot_trace_xhci_summary(u32 ports, u32 connected,
                                  u32 enumerated, u32 failed,
                                  u32 usb_mass_storage)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 42U, 'I');
    trace_append_text(message, &position, "xHCI ports=");
    trace_append_u32(message, &position, ports);
    trace_append_text(message, &position, " ccs=");
    trace_append_u32(message, &position, connected);
    trace_append_text(message, &position, " enum=");
    trace_append_u32(message, &position, enumerated);
    trace_append_text(message, &position, " failed=");
    trace_append_u32(message, &position, failed);
    trace_append_text(message, &position, " usb-msd=");
    trace_append_u32(message, &position, usb_mass_storage);
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_ports_stage(const char *stage, u64 ms, u32 command,
                                  u32 status, u32 connected, u32 powered)
{
    if (!trace_active) return;
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, stage);
    trace_append_text(message, &position, " t=");
    trace_append_u64(message, &position, ms);
    trace_append_text(message, &position, " C=");
    trace_append_hex(message, &position, command, 8U);
    trace_append_text(message, &position, " S=");
    trace_append_hex(message, &position, status, 8U);
    trace_append_text(message, &position, " CCS=");
    trace_append_hex(message, &position, connected, 8U);
    trace_append_text(message, &position, " PP=");
    trace_append_hex(message, &position, powered, 8U);
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_protocol(u32 major, u32 minor, u32 first,
                               u32 count, u32 slot_type)
{
    if (!trace_active) return;
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "USB");
    trace_append_u32(message, &position, major);
    trace_append_text(message, &position, ".");
    trace_append_hex(message, &position, minor, 2U);
    trace_append_text(message, &position, " ports first=");
    trace_append_u32(message, &position, first);
    trace_append_text(message, &position, " count=");
    trace_append_u32(message, &position, count);
    trace_append_text(message, &position, " slot-type=");
    trace_append_u32(message, &position, slot_type);
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_ports_raw(u32 caplength, u64 first_physical,
                                const u32 *routed, const u32 *started,
                                const u32 *final, u32 count)
{
    if (!trace_active) return;
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "caplen=");
    trace_append_hex(message, &position, caplength, 2U);
    trace_append_text(message, &position, " PORTSC1_PA=");
    trace_append_hex(message, &position, first_physical, 16U);
    trace_append_text(message, &position, " raw=route/start/final");
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    for (u32 first = 0; first < count; first += 2U) {
        position = 0;
        trace_append_prefix(message, &position, 47U, 'I');
        for (u32 port = first; port < count && port < first + 2U; port++) {
            if (port != first) trace_append_text(message, &position, " ");
            trace_append_u32(message, &position, port + 1U);
            trace_append_text(message, &position, ":");
            trace_append_hex(message, &position, routed[port], 8U);
            trace_append_text(message, &position, "/");
            trace_append_hex(message, &position, started[port], 8U);
            trace_append_text(message, &position, "/");
            trace_append_hex(message, &position, final[port], 8U);
        }
        message[position] = '\0';
        trace_store_wrapped(message, 'I');
    }
    trace_render();
}

void x250_boot_trace_routing(bool passed, u32 usb2_mask, u32 usb2_before,
                              u32 usb2_after, u32 usb3_mask,
                              u32 usb3_before, u32 usb3_after)
{
    if (!trace_active) return;
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    trace_append_prefix(message, &position, 47U, passed ? 'P' : 'F');
    trace_append_text(message, &position, "Intel routing USB2=");
    trace_append_hex(message, &position, usb2_before, 8U);
    trace_append_text(message, &position, ">");
    trace_append_hex(message, &position, usb2_after, 8U);
    trace_append_text(message, &position, " allow=");
    trace_append_hex(message, &position, usb2_mask, 8U);
    trace_append_text(message, &position, " USB3=");
    trace_append_hex(message, &position, usb3_before, 8U);
    trace_append_text(message, &position, ">");
    trace_append_hex(message, &position, usb3_after, 8U);
    trace_append_text(message, &position, " allow=");
    trace_append_hex(message, &position, usb3_mask, 8U);
    message[position] = '\0';
    trace_store_wrapped(message, passed ? 'P' : 'F');
    trace_render();
}

void x250_boot_trace_pci(u32 bus, u32 device, u32 function,
                          u32 vendor, u32 product, u32 interface, u32 command,
                          u32 d0, u32 d4, u32 d8, u32 dc)
{
    if (!trace_active) return;
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    trace_append_prefix(message, &position, 47U, 'I');
    trace_append_text(message, &position, "PCI USB ");
    trace_append_hex(message, &position, bus, 2U);
    trace_append_text(message, &position, ":");
    trace_append_hex(message, &position, device, 2U);
    trace_append_text(message, &position, ".");
    trace_append_u32(message, &position, function);
    trace_append_text(message, &position, " id=");
    trace_append_hex(message, &position, vendor, 4U);
    trace_append_text(message, &position, ":");
    trace_append_hex(message, &position, product, 4U);
    trace_append_text(message, &position, " pi=");
    trace_append_hex(message, &position, interface, 2U);
    trace_append_text(message, &position, " cmd=");
    trace_append_hex(message, &position, command, 4U);
    if (vendor == 0x8086U && interface == 0x30U) {
        trace_append_text(message, &position, " cfgD0/D4/D8/DC=");
        trace_append_hex(message, &position, d0, 8U);
        trace_append_text(message, &position, "/");
        trace_append_hex(message, &position, d4, 8U);
        trace_append_text(message, &position, "/");
        trace_append_hex(message, &position, d8, 8U);
        trace_append_text(message, &position, "/");
        trace_append_hex(message, &position, dc, 8U);
    }
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_xhci_port(u32 port, u32 portsc, const char *state,
                               bool result_available, int result,
                               const char *reason)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    bool failed = state && trace_text_equal(state, "FAILED");

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 43U, failed ? 'F' : 'I');
    trace_append_text(message, &position, "port=");
    trace_append_u32(message, &position, port);
    trace_append_text(message, &position, " PORTSC=");
    trace_append_hex(message, &position, portsc, 8U);
    trace_append_text(message, &position, " CCS=");
    trace_append_u32(message, &position, (portsc & 1U) != 0U);
    trace_append_text(message, &position, " PED=");
    trace_append_u32(message, &position, (portsc & 2U) != 0U);
    trace_append_text(message, &position, " PLS=");
    trace_append_u32(message, &position, (portsc >> 5) & 0x0fU);
    trace_append_text(message, &position, " PP=");
    trace_append_u32(message, &position, (portsc >> 9) & 1U);
    trace_append_text(message, &position, " speed=");
    trace_append_u32(message, &position, (portsc >> 10) & 0x0fU);
    trace_append_text(message, &position, " state=");
    trace_append_text(message, &position, state ? state : "UNKNOWN");
    if (result_available) {
        trace_append_text(message, &position, " rc=");
        trace_append_i32(message, &position, result);
    }
    if (reason) {
        trace_append_text(message, &position, " reason=");
        trace_append_text(message, &position, reason);
    }
    message[position] = '\0';
    trace_store_wrapped(message, failed ? 'F' : 'I');
    trace_render();
}

void x250_boot_trace_usb_device(u32 port, u32 slot, u32 speed,
                                u32 vendor_id, u32 product_id,
                                u32 class_code, u32 subclass,
                                u32 protocol, u8 class_flags,
                                bool mass_storage_endpoints_found,
                                const char *probe_stage, int probe_result)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 44U,
                        probe_result != 0 ? 'F' : 'I');
    trace_append_text(message, &position, "USB p=");
    trace_append_u32(message, &position, port);
    trace_append_text(message, &position, " s=");
    trace_append_u32(message, &position, slot);
    trace_append_text(message, &position, " sp=");
    trace_append_u32(message, &position, speed);
    trace_append_text(message, &position, " id=");
    trace_append_hex(message, &position, vendor_id, 4U);
    trace_append_text(message, &position, ":");
    trace_append_hex(message, &position, product_id, 4U);
    trace_append_text(message, &position, " cls=");
    trace_append_hex(message, &position, class_code, 2U);
    trace_append_text(message, &position, "/");
    trace_append_hex(message, &position, subclass, 2U);
    trace_append_text(message, &position, "/");
    trace_append_hex(message, &position, protocol, 2U);
    if (probe_result != 0) {
        trace_append_text(message, &position, " bind=failed probe=");
        trace_append_text(message, &position, probe_stage ? probe_stage : "?");
        trace_append_text(message, &position, " rc=");
        trace_append_i32(message, &position, probe_result);
    } else {
        trace_append_text(message, &position,
                          class_flags ? " bind=matched" : " bind=unsupported");
        trace_append_text(message, &position, mass_storage_endpoints_found ?
                          " msc-ep=present" : " msc-ep=absent");
    }
    message[position] = '\0';
    trace_store_wrapped(message, probe_result != 0 ? 'F' : 'I');
    trace_render();
}

void x250_boot_trace_msc_probe(u32 port, u32 slot, const char *stage,
                               int result, bool bot_ready,
                               bool capacity_known, bool block_registered,
                               bool gpt_scan_ran, bool gpt_scan_succeeded,
                               u32 gpt_partitions, u32 sector_size,
                               u64 sectors, const char *model)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;
    bool passed = result == 0 && block_registered;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 45U, passed ? 'P' : 'F');
    trace_append_text(message, &position, "MSC p=");
    trace_append_u32(message, &position, port);
    trace_append_text(message, &position, " s=");
    trace_append_u32(message, &position, slot);
    trace_append_text(message, &position, " stage=");
    trace_append_text(message, &position, stage ? stage : "?");
    trace_append_text(message, &position, " rc=");
    trace_append_i32(message, &position, result);
    trace_append_text(message, &position, " BOT=");
    trace_append_u32(message, &position, bot_ready);
    trace_append_text(message, &position, " cap=");
    trace_append_u32(message, &position, capacity_known);
    trace_append_text(message, &position, " blk=");
    trace_append_u32(message, &position, block_registered);
    trace_append_text(message, &position, " GPT=");
    if (!gpt_scan_ran) {
        trace_append_text(message, &position, "-");
    } else {
        trace_append_u32(message, &position, gpt_scan_succeeded);
        trace_append_text(message, &position, "/");
        trace_append_u32(message, &position, gpt_partitions);
    }
    if (capacity_known) {
        trace_append_text(message, &position, " sectors=");
        trace_append_u64(message, &position, sectors);
        trace_append_text(message, &position, " size=");
        trace_append_u32(message, &position, sector_size);
    }
    if (model && model[0]) {
        trace_append_text(message, &position, " model=");
        for (u32 index = 0; model[index] && index < 20U &&
                             position < TRACE_MESSAGE_CAPACITY - 1U;
             index++)
            message[position++] = model[index];
    }
    message[position] = '\0';
    trace_store_wrapped(message, passed ? 'P' : 'F');
    trace_render();
}

void x250_boot_trace_root_inventory(u32 block_devices, u32 whole_disks,
                                    u32 partition_devices,
                                    u32 root_candidates)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 46U, 'I');
    trace_append_text(message, &position, "root scan blocks=");
    trace_append_u32(message, &position, block_devices);
    trace_append_text(message, &position, " disks=");
    trace_append_u32(message, &position, whole_disks);
    trace_append_text(message, &position, " gpt-parts=");
    trace_append_u32(message, &position, partition_devices);
    trace_append_text(message, &position, " root-role=");
    trace_append_u32(message, &position, root_candidates);
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_block_device(u32 index, const char *type,
                                  u32 sector_size, u64 sectors,
                                  const char *model, int gpt_result,
                                  u32 gpt_type, u32 gpt_partitions,
                                  u32 registered_partitions,
                                  u32 root_candidates)
{
    char message[TRACE_MESSAGE_CAPACITY];
    u32 position = 0;

    if (!trace_active)
        return;
    trace_append_prefix(message, &position, 46U, 'I');
    trace_append_text(message, &position, "block#");
    trace_append_u32(message, &position, index);
    trace_append_text(message, &position, " ");
    trace_append_text(message, &position, type ? type : "?");
    trace_append_text(message, &position, " sectors=");
    trace_append_u64(message, &position, sectors);
    trace_append_text(message, &position, " size=");
    trace_append_u32(message, &position, sector_size);
    if (gpt_result == X250_TRACE_GPT_USB_SCAN_REPORTED) {
        trace_append_text(message, &position, " GPT=USB-scan");
    } else {
        trace_append_text(message, &position, " GPT-rc=");
        trace_append_i32(message, &position, gpt_result);
        trace_append_text(message, &position, " type=");
        trace_append_u32(message, &position, gpt_type);
        trace_append_text(message, &position, " entries=");
        trace_append_u32(message, &position, gpt_partitions);
    }
    trace_append_text(message, &position, " reg=");
    trace_append_u32(message, &position, registered_partitions);
    trace_append_text(message, &position, " root=");
    trace_append_u32(message, &position, root_candidates);
    if (model && model[0]) {
        trace_append_text(message, &position, " model=");
        for (u32 index = 0; model[index] && index < 20U &&
                             position < TRACE_MESSAGE_CAPACITY - 1U;
             index++)
            message[position++] = model[index];
    }
    message[position] = '\0';
    trace_store_wrapped(message, 'I');
    trace_render();
}

void x250_boot_trace_rebind(void *framebuffer, u32 width, u32 height,
                            u32 stride)
{
    trace_framebuffer = (volatile u32 *)framebuffer;
    trace_width = width;
    trace_height = height;
    trace_stride = stride;
    trace_render_if_active();
}

void x250_boot_trace_use_framebuffer_api(void)
{
    trace_use_framebuffer_api = true;
}

void x250_boot_trace_panic(const char *message,
                           const struct cpu_registers *regs,
                           u64 cr2, u64 cr3)
{
    trace_render_panic(message, regs, cr2, cr3);
}
#endif
