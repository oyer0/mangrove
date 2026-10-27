/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <terminal.h>
#include <font.h>
#include <framebuffer.h>
#include <timer.h>
#include <utf8.h>
#include <console.h>
#include <scheduler.h>
#include <spinlock.h>
#include <heap.h>
#include <terminal_palette.h>
#include <mangrove_errors.h>
#include <stdbool.h>
#include <string.h>
#include <input_trace.h>
#if INPUT_TRACE_DIAGNOSTICS
#include <cpu.h>
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#define TERMINAL_MARGIN_X 10
#define TERMINAL_MARGIN_Y 10
#define TERMINAL_LINE_SPACING 2

#define TERMINAL_BUFFER_ROWS 512
#define TERMINAL_MAX_COLS    256
#define TERMINAL_TAB_WIDTH   8U
#define TERMINAL_MAX_OVERLAY_ROWS 128
#define TERMINAL_CURSOR_BLINK_INTERVAL_MS 500ULL
#define TERMINAL_PRESENTATION_INTERVAL_MS 16ULL

#define TERMINAL_CONTROL_HEADER_SIZE 10U
#define TERMINAL_CONTROL_MAX_BATCH 65536U
#define TERMINAL_CONTROL_MAX_TEXT \
    (TERMINAL_CONTROL_MAX_BATCH - TERMINAL_CONTROL_HEADER_SIZE)
#define TERMINAL_CONTROL_MAX_RECORDS \
    (TERMINAL_CONTROL_MAX_BATCH / TERMINAL_CONTROL_HEADER_SIZE)

typedef struct {
    u32 codepoint;
    u8 fg_color;
    u8 bg_color;
} terminal_cell_t;

_Static_assert(sizeof(terminal_cell_t) == 8,
               "terminal cells should remain compact");

typedef struct {
    bool valid;
    u32 top_row_idx;
    u32 cursor_phys_row;
    u32 cursor_col;
    u8 fg_color;
    u8 bg_color;
    bool cursor_visible;
    bool cursor_enabled;
    bool vram_valid;
    utf8_decoder_t utf8_decoder;
    bool cursor_blink_pending;
    u64 cursor_blink_deadline_ms;
} terminal_saved_state_t;

typedef struct {
    terminal_cell_t cells[TERMINAL_BUFFER_ROWS * TERMINAL_MAX_COLS];
    terminal_cell_t alternate_cells[TERMINAL_BUFFER_ROWS * TERMINAL_MAX_COLS];

    u32 top_row_idx;        // Physical ring buffer index of top visible screen row
    u32 cursor_phys_row;    // Physical ring buffer index of current cursor row
    u32 cursor_col;         // Current cursor column (0 .. visible_cols - 1)

    u8 fg_color;
    u8 bg_color;

    u32 visible_cols;       // Visible columns count
    u32 visible_rows;       // Visible rows count
    u32 width;
    u32 height;

    bool cursor_visible;
    bool cursor_enabled;
    bool vram_valid;        // Indicates if VRAM cache is synchronized with ring buffer
    utf8_decoder_t utf8_decoder;

    /* Dirty tracking & hybrid scroll batching */
    u32 dirty_min_row;      // Minimum dirty screen row (inclusive)
    u32 dirty_max_row;      // Maximum dirty screen row (inclusive)
    bool dirty_rows[TERMINAL_BUFFER_ROWS];
    u32 dirty_left[TERMINAL_BUFFER_ROWS];
    u32 dirty_right[TERMINAL_BUFFER_ROWS]; /* Exclusive pixel boundary. */
    bool batch_active;      // True when inside begin_batch/end_batch
    u32 pending_scroll_rows; // Accumulated pending scroll rows during batch

    bool alternate_active;
    u64 alternate_owner_pid;
    u64 key_event_owner_pid;
    terminal_saved_state_t saved_normal;
} terminal_t;

typedef struct {
    bool active;
    u64 owner_pid;
    u32 first_screen_row;
    u32 rows;
    u32 columns;
    terminal_cell_t cells[TERMINAL_MAX_OVERLAY_ROWS * TERMINAL_MAX_COLS];
} terminal_overlay_t;

static terminal_t terminal;
static struct {
    bool valid;
    u64 owner_pid;
    u32 physical_row;
    u32 extent;
} terminal_input;
static terminal_overlay_t terminal_overlay;
static terminal_stats_t stats;
static spinlock_t terminal_lock;
static bool terminal_lock_ready;
static u32 batch_depth = 0;
static bool presentation_pending;
static bool presentation_worker_started;
/* These two fields are shared only with the timer IRQ.  The IRQ sets a
 * pending bit; all framebuffer access remains in normal kernel context. */
static volatile bool cursor_blink_pending;
static volatile u64 cursor_blink_deadline_ms;
static kernel_thread_t *cursor_blink_worker;
static kernel_thread_t *presentation_worker;

static void terminal_scroll(void);
static void terminal_mark_dirty(u32 screen_row);
static void terminal_flush_dirty_locked(void);
static void terminal_cursor_render_show_locked(void);
static void terminal_cursor_restart_blink_locked(void);
static void terminal_cursor_show_locked(void);
static void terminal_cursor_hide_locked(void);
static void terminal_overlay_render_locked(void);
static void terminal_overlay_clear_locked(bool flush);
static void terminal_redraw_locked(void);
static void terminal_clear_locked(void);
static void terminal_put_codepoint_locked(u32 codepoint);
static void terminal_putc_locked(char c);
static void terminal_begin_batch_locked(void);
static void terminal_end_batch_locked(bool defer_presentation);
static void terminal_force_end_batch_locked(void);
static void terminal_present_locked(void);
static bool terminal_process_is_owner_locked(u64 process_id);
static bool terminal_process_controls_locked(u64 process_id);
static bool terminal_move_cursor_locked(u32 row, u32 column);
static void terminal_set_cursor_visible_locked(bool visible);
static bool terminal_clear_current_line_locked(void);
static bool terminal_clear_current_to_end_locked(void);

static bool terminal_color_valid(u32 color)
{
    return color < TERMINAL_COLOR_COUNT;
}

static u32 terminal_color_pixel(u8 color)
{
    terminal_style_t default_style = terminal_style_for_role(
        TERMINAL_STYLE_DEFAULT);
    return terminal_palette_rgb[terminal_color_valid(color) ? color :
                                default_style.foreground];
}

static bool terminal_cursor_deadline_reached(u64 now, u64 deadline)
{
    /* The interval is tiny compared with the u64 tick range, so signed
     * modular subtraction remains valid across the tick counter wrap. */
    return (i64)(now - deadline) >= 0;
}

void terminal_get_stats(terminal_stats_t *out_stats) {
    u64 saved_flags;

    if (!out_stats) return;
    if (!terminal_lock_ready) {
        *out_stats = stats;
        return;
    }
    saved_flags = spin_lock_irqsave(&terminal_lock);
    *out_stats = stats;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_reset_stats(void) {
    u64 saved_flags;

    if (!terminal_lock_ready) {
        stats.batch_count = 0;
        stats.full_redraw_count = 0;
        stats.glyph_render_count = 0;
        stats.ram_bytes_shifted = 0;
        stats.vram_flush_count = 0;
        stats.vram_bytes_copied = 0;
        return;
    }
    saved_flags = spin_lock_irqsave(&terminal_lock);
    stats.batch_count = 0;
    stats.full_redraw_count = 0;
    stats.glyph_render_count = 0;
    stats.ram_bytes_shifted = 0;
    stats.vram_flush_count = 0;
    stats.vram_bytes_copied = 0;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static u32 terminal_line_height(void){
    return font_height() + TERMINAL_LINE_SPACING;
}

/* Centralized helper to map physical (row, col) to a cell pointer. Returns NULL on invalid index. */
static inline terminal_cell_t *terminal_cell_at(u32 phys_row, u32 col) {
    terminal_cell_t *cells;

    if (phys_row >= TERMINAL_BUFFER_ROWS || col >= terminal.visible_cols)
        return NULL;
    cells = terminal.alternate_active ? terminal.alternate_cells
                                      : terminal.cells;
    return &cells[(phys_row * terminal.visible_cols) + col];
}

static inline u32 phys_to_screen_row(u32 phys_row) {
    return (phys_row - terminal.top_row_idx + TERMINAL_BUFFER_ROWS) % TERMINAL_BUFFER_ROWS;
}

static inline u32 screen_to_phys_row(u32 screen_row) {
    return (terminal.top_row_idx + screen_row) % TERMINAL_BUFFER_ROWS;
}

/* A full-width line leaves the cursor at the pending-wrap position one cell
 * past the grid. Render it over the last visible cell instead of in the
 * framebuffer margin. */
static inline u32 terminal_cursor_display_column(void)
{
    if (!terminal.visible_cols) return 0U;
    return terminal.cursor_col < terminal.visible_cols ?
           terminal.cursor_col : terminal.visible_cols - 1U;
}

static void terminal_clear_phys_row(u32 phys_row, u8 bg_color) {
    for (u32 c = 0; c < terminal.visible_cols; c++) {
        terminal_cell_t *cell = terminal_cell_at(phys_row, c);
        if (cell) {
            cell->codepoint = ' ';
            cell->fg_color = terminal.fg_color;
            cell->bg_color = bg_color;
        }
    }
}

/* Mark a screen row as dirty so it will be flushed to VRAM */
static void terminal_mark_dirty_span(u32 screen_row, u32 left, u32 right) {
    if (screen_row >= terminal.visible_rows || left >= terminal.width) return;
    if (right > terminal.width) right = terminal.width;
    if (left >= right) return;
    if (!terminal.dirty_rows[screen_row]) {
        terminal.dirty_left[screen_row] = left;
        terminal.dirty_right[screen_row] = right;
    } else {
        if (left < terminal.dirty_left[screen_row])
            terminal.dirty_left[screen_row] = left;
        if (right > terminal.dirty_right[screen_row])
            terminal.dirty_right[screen_row] = right;
    }
    terminal.dirty_rows[screen_row] = true;
    if (screen_row < terminal.dirty_min_row)
        terminal.dirty_min_row = screen_row;
    if (screen_row > terminal.dirty_max_row)
        terminal.dirty_max_row = screen_row;
}

static void terminal_mark_dirty(u32 screen_row) {
    terminal_mark_dirty_span(screen_row, 0U, terminal.width);
}

static void terminal_mark_cell_dirty(u32 screen_row, u32 column) {
    u32 left = TERMINAL_MARGIN_X + column * font_width();
    terminal_mark_dirty_span(screen_row, left, left + font_width());
}

/* Mark all visible rows as dirty */
static void terminal_mark_all_dirty(void) {
    terminal.dirty_min_row = 0;
    terminal.dirty_max_row = terminal.visible_rows - 1;
    for (u32 row = 0; row < terminal.visible_rows; row++) {
        terminal.dirty_rows[row] = true;
        terminal.dirty_left[row] = 0U;
        terminal.dirty_right[row] = terminal.width;
    }
}

/* Reset dirty tracking */
static void terminal_reset_dirty(void) {
    terminal.dirty_min_row = terminal.visible_rows;
    terminal.dirty_max_row = 0;
    for (u32 row = 0; row < TERMINAL_BUFFER_ROWS; row++)
        terminal.dirty_rows[row] = false;
}

/* Flush dirty rows from backbuffer to VRAM */
static void terminal_flush_dirty_locked(void) {
    if (terminal.dirty_min_row > terminal.dirty_max_row) return;

    u32 line_h = terminal_line_height();
    u32 row = terminal.dirty_min_row;
    /* A cursor near the top and a status line at the bottom must not turn
     * into a full-screen MMIO copy with IRQs masked. Copy contiguous dirty
     * runs, preserving clean rows between them. */
    while (row <= terminal.dirty_max_row) {
        if (!terminal.dirty_rows[row]) {
            row++;
            continue;
        }
        u32 first = row;
        u32 left = terminal.dirty_left[row];
        u32 right = terminal.dirty_right[row];
        do {
            row++;
        } while (row <= terminal.dirty_max_row && terminal.dirty_rows[row] &&
                 terminal.dirty_left[row] == left &&
                 terminal.dirty_right[row] == right);
        u32 start_y = TERMINAL_MARGIN_Y + first * line_h;
        u32 end_y = TERMINAL_MARGIN_Y + row * line_h;
        if (end_y > terminal.height) end_y = terminal.height;
        if (start_y >= end_y) continue;
        u32 flush_h = end_y - start_y;
        input_trace_record(INPUT_TRACE_FLUSH_BEGIN, cpu_current_index(), start_y, flush_h);
        if (left == 0U && right == terminal.width)
            framebuffer_flush_rows(start_y, flush_h);
        else
            framebuffer_flush_rect(left, start_y, right - left, flush_h);
        input_trace_record(INPUT_TRACE_FLUSH_END, cpu_current_index(), start_y, flush_h);
        stats.vram_flush_count++;
        u32 copied_width = left == 0U && right == terminal.width
            ? framebuffer_pitch() : right - left;
        stats.vram_bytes_copied += (u64)flush_h * copied_width * sizeof(u32);
    }

    terminal_reset_dirty();
}

/* Render a single cell from the ring buffer into the backbuffer */
static void terminal_render_cell(u32 phys_row, u32 col) {
    u32 screen_row = phys_to_screen_row(phys_row);
    if (screen_row >= terminal.visible_rows) return;

    u32 px = TERMINAL_MARGIN_X + (col * font_width());
    u32 py = TERMINAL_MARGIN_Y + (screen_row * terminal_line_height());

    terminal_cell_t *cell = terminal_cell_at(phys_row, col);
    u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
    u32 fg = cell ? cell->fg_color : terminal.fg_color;
    u32 bg = cell ? cell->bg_color : terminal.bg_color;

    draw_codepoint(codepoint, px, py, terminal_color_pixel((u8)fg),
                   terminal_color_pixel((u8)bg));
    stats.glyph_render_count++;
    terminal_mark_cell_dirty(screen_row, col);
}

/* Render a full screen row into the backbuffer */
static void terminal_render_screen_row(u32 screen_row) {
    if (screen_row >= terminal.visible_rows) return;

    u32 phys_r = screen_to_phys_row(screen_row);
    u32 py = TERMINAL_MARGIN_Y + (screen_row * terminal_line_height());

    for (u32 c = 0; c < terminal.visible_cols; c++) {
        terminal_cell_t *cell = terminal_cell_at(phys_r, c);
        u32 px = TERMINAL_MARGIN_X + (c * font_width());
        u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;

        draw_codepoint(codepoint, px, py, terminal_color_pixel((u8)fg),
                       terminal_color_pixel((u8)bg));
        stats.glyph_render_count++;
    }
    terminal_mark_dirty(screen_row);
}

/* Render temporary UI rows without changing the terminal ring buffer. */
static void terminal_render_overlay_row_locked(u32 screen_row)
{
    u32 overlay_row;
    u32 py;

    if (!terminal_overlay.active ||
        screen_row < terminal_overlay.first_screen_row)
        return;
    overlay_row = screen_row - terminal_overlay.first_screen_row;
    if (overlay_row >= terminal_overlay.rows ||
        screen_row >= terminal.visible_rows)
        return;

    py = TERMINAL_MARGIN_Y + (screen_row * terminal_line_height());
    for (u32 column = 0; column < terminal_overlay.columns; column++) {
        terminal_cell_t *cell = &terminal_overlay.cells[
            overlay_row * terminal_overlay.columns + column];
        u32 px = TERMINAL_MARGIN_X + (column * font_width());
        u32 codepoint = cell->codepoint ? cell->codepoint : ' ';

        draw_codepoint(codepoint, px, py,
                       terminal_color_pixel(cell->fg_color),
                       terminal_color_pixel(cell->bg_color));
        stats.glyph_render_count++;
    }
    terminal_mark_dirty(screen_row);
}

static void terminal_overlay_render_locked(void)
{
    u32 last_row;

    if (!terminal_overlay.active) return;
    last_row = terminal_overlay.first_screen_row + terminal_overlay.rows;
    if (last_row > terminal.visible_rows) last_row = terminal.visible_rows;
    for (u32 row = terminal_overlay.first_screen_row; row < last_row; row++)
        terminal_render_overlay_row_locked(row);
}

/* Canonical renderer: defines authoritative reference output for any terminal state */
static void terminal_redraw_locked(void) {
    stats.full_redraw_count++;
    terminal_cursor_hide_locked();
    for (u32 r = 0; r < terminal.visible_rows; r++) {
        terminal_render_screen_row(r);
    }
    terminal.vram_valid = true;
    terminal_cursor_show_locked();
    terminal_overlay_render_locked();
    terminal_flush_dirty_locked();
}

void terminal_redraw(void) {
    u64 saved_flags;

    if (!terminal_lock_ready) {
        terminal_redraw_locked();
        return;
    }
    saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_redraw_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_init(BOOT_INFO *BootInfo){
    (void)BootInfo;

    spinlock_init(&terminal_lock);
    terminal_lock_ready = true;
    presentation_pending = false;
    presentation_worker_started = false;
    presentation_worker = NULL;

    terminal_style_t default_style = terminal_style_for_role(
        TERMINAL_STYLE_DEFAULT);
    terminal.fg_color = (u8)default_style.foreground;
    terminal.bg_color = (u8)default_style.background;

    terminal.width  = BootInfo->FramebufferWidth;
    terminal.height = BootInfo->FramebufferHeight;

    u32 glyph_w = font_width();
    if (!glyph_w) glyph_w = 1U;
    u32 content_width = terminal.width > (TERMINAL_MARGIN_X * 2) ?
                        terminal.width - (TERMINAL_MARGIN_X * 2) : 0U;
    terminal.visible_cols = content_width / glyph_w;
    if (terminal.visible_cols > TERMINAL_MAX_COLS) {
        terminal.visible_cols = TERMINAL_MAX_COLS;
    }
    if (!terminal.visible_cols) terminal.visible_cols = 1U;

    u32 line_h = terminal_line_height();
    if (!line_h) line_h = 1U;
    u32 content_height = terminal.height > (TERMINAL_MARGIN_Y * 2) ?
                         terminal.height - (TERMINAL_MARGIN_Y * 2) : 0U;
    terminal.visible_rows = content_height / line_h;
    if (!terminal.visible_rows) terminal.visible_rows = 1U;
    if (terminal.visible_rows > TERMINAL_BUFFER_ROWS)
        terminal.visible_rows = TERMINAL_BUFFER_ROWS;

    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;

    terminal.cursor_visible = false;
    /* Startup output is noninteractive.  Keep the cursor disabled until an
     * input owner explicitly requests it through the terminal interface. */
    terminal.cursor_enabled = false;
    terminal.vram_valid = false;
    terminal.batch_active = false;
    terminal.pending_scroll_rows = 0;
    terminal.alternate_active = false;
    terminal.alternate_owner_pid = 0;
    terminal.key_event_owner_pid = 0;
    terminal_overlay.active = false;
    terminal_overlay.owner_pid = 0;
    terminal_overlay.first_screen_row = 0;
    terminal_overlay.rows = 0;
    terminal_overlay.columns = 0;
    terminal.saved_normal.valid = false;
    cursor_blink_pending = false;
    cursor_blink_deadline_ms = 0;
    terminal_reset_dirty();
    terminal_reset_stats();

    for (u32 r = 0; r < TERMINAL_BUFFER_ROWS; r++) {
        terminal_clear_phys_row(r, terminal.bg_color);
    }

    terminal_clear();
}

static void terminal_cursor_render_show_locked(void){
    /* The cursor is a pixel overlay, never part of the scrolled text. During
     * a transaction or deferred scroll, logical rows can already have moved
     * while the backbuffer still has its old layout. Wait for the transaction
     * to end and row copies to finish; otherwise they preserve a ghost. */
    if (!terminal.cursor_enabled || terminal.cursor_visible ||
        terminal.batch_active || terminal.pending_scroll_rows)
        return;

    u32 srow = phys_to_screen_row(terminal.cursor_phys_row);
    if (srow >= terminal.visible_rows)
        return;

    u32 column = terminal_cursor_display_column();
    u32 px = TERMINAL_MARGIN_X + (column * font_width());
    u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());

    terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, column);
    u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
    u8 foreground = cell ? cell->fg_color : terminal.fg_color;
    u8 background = cell ? cell->bg_color : terminal.bg_color;

    draw_codepoint(codepoint, px, py,
                   terminal_color_pixel(background),
                   terminal_color_pixel(foreground));
    stats.glyph_render_count++;
    terminal_mark_cell_dirty(srow, column);
    terminal.cursor_visible = true;
}

static void terminal_cursor_restart_blink_locked(void)
{
    u64 now;

    if (!terminal.cursor_enabled) {
        cursor_blink_pending = false;
        cursor_blink_deadline_ms = 0;
        return;
    }

    now = timer_uptime_ms();
    /* Publish the new deadline before clearing a pending old deadline.  If
     * the timer IRQ runs between these stores it observes the new interval
     * and cannot resurrect the expired one. */
    cursor_blink_deadline_ms = now + TERMINAL_CURSOR_BLINK_INTERVAL_MS;
    cursor_blink_pending = false;
}

static void terminal_cursor_show_locked(void){
    terminal_cursor_render_show_locked();
    terminal_cursor_restart_blink_locked();
}

static void terminal_cursor_hide_locked(void){
    if (!terminal.cursor_visible)
        return;

    u32 srow = phys_to_screen_row(terminal.cursor_phys_row);
    if (srow < terminal.visible_rows) {
        u32 column = terminal_cursor_display_column();
        u32 px = TERMINAL_MARGIN_X + (column * font_width());
        u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());

        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row,
                                                 column);
        u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;
        draw_codepoint(codepoint, px, py, terminal_color_pixel((u8)fg),
                       terminal_color_pixel((u8)bg));
        stats.glyph_render_count++;
        terminal_mark_cell_dirty(srow, column);
    }

    terminal.cursor_visible = false;
}

/* Restore the backing terminal rows below a temporary overlay.  The logical
 * cursor and ring-buffer state are deliberately left untouched. */
static void terminal_overlay_clear_locked(bool flush)
{
    u32 first_row;
    u32 last_row;

    if (!terminal_overlay.active) return;
    first_row = terminal_overlay.first_screen_row;
    last_row = first_row + terminal_overlay.rows;
    if (last_row > terminal.visible_rows) last_row = terminal.visible_rows;

    terminal_cursor_hide_locked();
    terminal_overlay.active = false;
    terminal_overlay.owner_pid = 0;
    terminal_overlay.rows = 0;
    terminal_overlay.columns = 0;
    for (u32 row = first_row; row < last_row; row++)
        terminal_render_screen_row(row);
    terminal_cursor_show_locked();
    if (flush && !terminal.batch_active)
        terminal_flush_dirty_locked();
}

void terminal_cursor_show(void){
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_cursor_show_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_cursor_hide(void){
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_cursor_hide_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_cursor_blink_timer_tick(void)
{
    u64 now;
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!terminal.cursor_enabled || !cursor_blink_deadline_ms) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return;
    }

    now = timer_uptime_ms();
    if (terminal_cursor_deadline_reached(now, cursor_blink_deadline_ms))
        cursor_blink_pending = true;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_cursor_blink_poll(void)
{
    u64 now;
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!terminal.cursor_enabled || terminal.batch_active ||
        presentation_pending ||
        !cursor_blink_deadline_ms) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return;
    }

    now = timer_uptime_ms();
    if (!cursor_blink_pending &&
        !terminal_cursor_deadline_reached(now, cursor_blink_deadline_ms)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return;
    }
    if (!terminal_cursor_deadline_reached(now, cursor_blink_deadline_ms)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return;
    }

    if (terminal.cursor_visible) {
        terminal_cursor_hide_locked();
    } else {
        terminal_cursor_render_show_locked();
    }

    /* Set the next deadline before clearing the pending flag so an IRQ
     * cannot leave a stale expiration behind while this redraw is running. */
    cursor_blink_deadline_ms = now + TERMINAL_CURSOR_BLINK_INTERVAL_MS;
    cursor_blink_pending = false;
    terminal_flush_dirty_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static u64 terminal_cursor_blink_wait_ms(void)
{
    u64 now;
    u64 deadline;
    u64 result;
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!terminal.cursor_enabled || !cursor_blink_deadline_ms) {
        result = TERMINAL_CURSOR_BLINK_INTERVAL_MS;
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return result;
    }
    if (terminal.batch_active) {
        result = 10U;
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return result;
    }

    now = timer_uptime_ms();
    deadline = cursor_blink_deadline_ms;
    result = terminal_cursor_deadline_reached(now, deadline) ? 1U :
             deadline - now;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return result;
}

static void terminal_cursor_blink_worker_entry(void *argument)
{
    (void)argument;
    for (;;) {
        u64 wait_ms;

        terminal_cursor_blink_poll();
        wait_ms = terminal_cursor_blink_wait_ms();
        if (!wait_ms)
            wait_ms = 1U;
        if (!scheduler_sleep(wait_ms))
            (void)scheduler_yield();
    }
}

bool terminal_cursor_blink_start(void)
{
    if (cursor_blink_worker)
        return true;
    cursor_blink_worker = thread_create_with_priority(
        "terminal-blink", terminal_cursor_blink_worker_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);
    return cursor_blink_worker != NULL;
}

static void terminal_presentation_worker_entry(void *argument)
{
    (void)argument;

    for (;;) {
        u64 saved_flags = spin_lock_irqsave(&terminal_lock);

        /* An explicit transaction owns presentation until its outer end. */
        if (presentation_pending && batch_depth == 0)
            terminal_present_locked();
        spin_unlock_irqrestore(&terminal_lock, saved_flags);

        /* One scheduler tick is the bounded coalescing window.  Writes that
         * arrive farther apart remain independently visible to stream users. */
        if (!scheduler_sleep(TERMINAL_PRESENTATION_INTERVAL_MS))
            (void)scheduler_yield();
    }
}

bool terminal_presentation_start(void)
{
    if (presentation_worker_started)
        return true;

    presentation_worker = thread_create_with_priority(
        "terminal-present", terminal_presentation_worker_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);
    if (!presentation_worker)
        return false;

    presentation_worker_started = true;
    return true;
}

static void terminal_scroll(void) {
    /* 1. Advance ring buffer indices in O(1) time */
    terminal.top_row_idx = (terminal.top_row_idx + 1) % TERMINAL_BUFFER_ROWS;
    terminal.cursor_phys_row = (terminal.top_row_idx + terminal.visible_rows - 1) % TERMINAL_BUFFER_ROWS;

    /* 2. Clear new bottom row in text ring buffer */
    terminal_clear_phys_row(terminal.cursor_phys_row, terminal.bg_color);

    if (terminal.batch_active) {
        /* Hybrid strategy rule 3: Accumulate pending_scroll_rows inside batch */
        terminal.pending_scroll_rows++;
        return;
    }

    /* Hybrid strategy rule 2: Single scroll in non-batch mode */
    u32 line_h = terminal_line_height();
    u32 copy_height = (terminal.visible_rows - 1) * line_h;

    framebuffer_copy_rows(
        TERMINAL_MARGIN_Y,
        TERMINAL_MARGIN_Y + line_h,
        copy_height
    );
    stats.ram_bytes_shifted += (u64)copy_height * framebuffer_pitch() * sizeof(u32);

    framebuffer_fill_rows(
        TERMINAL_MARGIN_Y + copy_height,
        line_h,
        terminal_color_pixel(terminal.bg_color)
    );

    u32 bottom_py = TERMINAL_MARGIN_Y + copy_height;
    for (u32 c = 0; c < terminal.visible_cols; c++) {
        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, c);
        u32 px = TERMINAL_MARGIN_X + (c * font_width());
        u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;
        draw_codepoint(codepoint, px, bottom_py,
                       terminal_color_pixel((u8)fg),
                       terminal_color_pixel((u8)bg));
        stats.glyph_render_count++;
    }

    terminal_mark_all_dirty();
}

static void terminal_newline(void) {
    terminal.cursor_col = 0;

    u32 current_srow = phys_to_screen_row(terminal.cursor_phys_row);

    if (current_srow >= terminal.visible_rows - 1) {
        terminal_scroll();
    } else {
        terminal.cursor_phys_row = (terminal.cursor_phys_row + 1) % TERMINAL_BUFFER_ROWS;
    }
}

static void terminal_put_codepoint_with_colors_locked(u32 codepoint,
                                                       u8 foreground,
                                                       u8 background) {
    if (!terminal.batch_active) terminal_cursor_hide_locked();

    /* ESC is data on ordinary text streams. Render it visibly as the Unicode
     * replacement glyph rather than interpreting an ANSI/CSI sequence. */
    if (codepoint == 0x1BU) codepoint = 0xfffdU;

    if (codepoint == '\r') {
        terminal.cursor_col = 0;
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }

    if (codepoint == '\b') {
        if (terminal.cursor_col > 0) terminal.cursor_col--;
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }

    if (codepoint == '\n') {
        terminal_newline();
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }

    if (codepoint == '\t') {
        bool own_batch = !terminal.batch_active;
        if (terminal.cursor_col >= terminal.visible_cols)
            terminal_newline();
        u32 spaces = TERMINAL_TAB_WIDTH -
                     (terminal.cursor_col % TERMINAL_TAB_WIDTH);
        if (own_batch) terminal_begin_batch_locked();
        while (spaces--)
            terminal_put_codepoint_with_colors_locked(' ', foreground,
                                                       background);
        if (own_batch) terminal_end_batch_locked(false);
        return;
    }

    if (terminal.cursor_col >= terminal.visible_cols) {
        terminal_newline();
    }

    /* Track in text buffer */
    terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, terminal.cursor_col);
    u8 cell_foreground = foreground;
    u8 cell_background = background;
    bool changed = !cell || cell->codepoint != codepoint ||
                   cell->fg_color != cell_foreground ||
                   cell->bg_color != cell_background;
    if (cell && changed) {
        cell->codepoint = codepoint;
        cell->fg_color = cell_foreground;
        cell->bg_color = cell_background;
    }

    /* Render cell into RAM backbuffer */
    if (changed && (!terminal.batch_active || terminal.pending_scroll_rows == 0)) {
        terminal_render_cell(terminal.cursor_phys_row, terminal.cursor_col);
    }

    terminal.cursor_col++;

    /* Flush immediately if not in batch mode */
    if (!terminal.batch_active) {
        terminal_cursor_show_locked();
        terminal_flush_dirty_locked();
    }
}

static void terminal_put_codepoint_locked(u32 codepoint)
{
    terminal_put_codepoint_with_colors_locked(codepoint,
                                              terminal.fg_color,
                                              terminal.bg_color);
}

void terminal_put_codepoint(u32 codepoint)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_overlay_clear_locked(true);
    terminal_put_codepoint_locked(codepoint);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static void terminal_putc_locked(char c) {
    utf8_decode_result_t result = utf8_decoder_feed(&terminal.utf8_decoder,
                                                     (u8)c);
    for (u8 index = 0; index < result.count; index++)
        terminal_put_codepoint_locked(result.codepoints[index]);
}

static void terminal_putc_with_colors_locked(char c, u8 foreground,
                                             u8 background)
{
    utf8_decode_result_t result = utf8_decoder_feed(&terminal.utf8_decoder,
                                                     (u8)c);
    for (u8 index = 0; index < result.count; index++)
        terminal_put_codepoint_with_colors_locked(result.codepoints[index],
                                                  foreground, background);
}

void terminal_putc(char c)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_overlay_clear_locked(true);
    terminal_putc_locked(c);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_write_bytes(const char *buffer, u64 length)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_overlay_clear_locked(true);
    terminal_begin_batch_locked();
    for (u64 i = 0; i < length; i++)
        terminal_putc_locked(buffer[i]);
    terminal_end_batch_locked(true);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_write(const char *str) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (str) {
        terminal_overlay_clear_locked(true);
        terminal_begin_batch_locked();
        while (*str)
            terminal_putc_locked(*str++);
        terminal_end_batch_locked(false);
    }

    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static void terminal_clear_locked(void) {
    terminal_overlay_clear_locked(false);
    terminal_cursor_hide_locked();

    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;
    terminal.pending_scroll_rows = 0;
    utf8_decoder_init(&terminal.utf8_decoder);

    for (u32 r = 0; r < TERMINAL_BUFFER_ROWS; r++) {
        terminal_clear_phys_row(r, terminal.bg_color);
    }

    /* Clearing is a terminal-state update.  framebuffer_clear() also
     * presents immediately, which would expose a blank intermediate frame
     * when a caller is inside a terminal update transaction.  Fill the RAM
     * target here and let the normal dirty-row flush at update_end() present
     * the completed state. */
    framebuffer_fill_rows(0, terminal.height,
                          terminal_color_pixel(terminal.bg_color));
    terminal.vram_valid = true;
    terminal.cursor_visible = false;
    terminal_reset_dirty();
    terminal_mark_all_dirty();
    if (!terminal.batch_active) {
        terminal_cursor_show_locked();
        terminal_flush_dirty_locked();
    }
}

void terminal_clear(void) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_clear_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static void terminal_set_colors_locked(terminal_color_t foreground,
                                       terminal_color_t background)
{
    terminal.fg_color = (u8)foreground;
    terminal.bg_color = (u8)background;
}

void terminal_palette_set_foreground(terminal_color_t color)
{
    u64 saved_flags;

    if (!terminal_color_valid(color)) return;
    saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal.fg_color = (u8)color;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_palette_set_background(terminal_color_t color)
{
    u64 saved_flags;

    if (!terminal_color_valid(color)) return;
    saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal.bg_color = (u8)color;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_palette_set_colors(terminal_color_t foreground,
                                 terminal_color_t background)
{
    u64 saved_flags;

    if (!terminal_color_valid(foreground) ||
        !terminal_color_valid(background)) return;
    saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_set_colors_locked(foreground, background);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_palette_reset_colors(void)
{
    terminal_style_t default_style = terminal_style_for_role(
        TERMINAL_STYLE_DEFAULT);
    terminal_palette_set_colors(default_style.foreground,
                                default_style.background);
}

void terminal_cursor_enable(void) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal.cursor_enabled = true;
    terminal_cursor_show_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_cursor_disable(void) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_cursor_hide_locked();
    terminal.cursor_enabled = false;
    cursor_blink_pending = false;
    cursor_blink_deadline_ms = 0;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static void terminal_begin_batch_locked(void) {
    if (terminal.alternate_active)
        input_trace_record(INPUT_TRACE_BATCH, 0, 0, 0);
    batch_depth++;
    if (batch_depth == 1) {
        stats.batch_count++;
        terminal_cursor_hide_locked();
    }
    terminal.batch_active = true;
}

static void terminal_present_locked(void)
{
    if (batch_depth != 0)
        return;

    presentation_pending = false;
    if (terminal.pending_scroll_rows == 0) {
        terminal_cursor_show_locked();
        terminal_overlay_render_locked();
        terminal_flush_dirty_locked();
    } else if (terminal.pending_scroll_rows < terminal.visible_rows) {
        u32 line_h = terminal_line_height();
        u32 count = terminal.pending_scroll_rows;
        u32 shift_pixels = count * line_h;
        u32 keep_height = (terminal.visible_rows - count) * line_h;

        framebuffer_copy_rows(
            TERMINAL_MARGIN_Y,
            TERMINAL_MARGIN_Y + shift_pixels,
            keep_height
        );
        stats.ram_bytes_shifted += (u64)keep_height * framebuffer_pitch() * sizeof(u32);
        framebuffer_fill_rows(
            TERMINAL_MARGIN_Y + keep_height,
            shift_pixels,
            terminal_color_pixel(terminal.bg_color)
        );

        u32 start_screen_row = terminal.visible_rows - count;
        for (u32 r = start_screen_row; r < terminal.visible_rows; r++)
            terminal_render_screen_row(r);

        terminal.pending_scroll_rows = 0;
        terminal_mark_all_dirty();
        terminal_cursor_show_locked();
        terminal_overlay_render_locked();
        terminal_flush_dirty_locked();
    } else {
        terminal.pending_scroll_rows = 0;
        terminal_redraw_locked();
    }
    if (terminal.alternate_active)
        input_trace_record(INPUT_TRACE_PRESENT, 0,
                           (terminal.cursor_phys_row + TERMINAL_BUFFER_ROWS -
                            terminal.top_row_idx) % TERMINAL_BUFFER_ROWS,
                           terminal.cursor_col);
}

static void terminal_end_batch_locked(bool defer_presentation) {
    if (batch_depth > 0) {
        batch_depth--;
    }
    if (defer_presentation)
        presentation_pending = true;
    if (batch_depth != 0)
        return;

    terminal.batch_active = false;

    if (!defer_presentation || !presentation_worker_started)
        terminal_present_locked();
}

static void terminal_force_end_batch_locked(void) {
    if (!terminal.batch_active && !presentation_pending)
        return;
    batch_depth = 0;
    terminal.batch_active = false;
    terminal_present_locked();
}

void terminal_begin_batch(void) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_begin_batch_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_end_batch(void) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_end_batch_locked(false);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_force_end_batch(void) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_force_end_batch_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static bool terminal_process_is_owner_locked(u64 process_id)
{
    return terminal.alternate_active && process_id != 0 &&
           terminal.alternate_owner_pid == process_id;
}

static bool terminal_process_controls_locked(u64 process_id)
{
    return !terminal.alternate_active ||
           terminal_process_is_owner_locked(process_id);
}

bool terminal_process_controls(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool allowed = terminal_process_controls_locked(process_id);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return allowed;
}

bool terminal_process_output_allowed(u64 process_id)
{
    return terminal_process_controls(process_id);
}

bool terminal_process_input_allowed(u64 process_id)
{
    return terminal_process_controls(process_id);
}

static i64 terminal_write_colors_locked(u64 process_id, const char *buffer,
                                        u64 length,
                                        terminal_color_t foreground,
                                        terminal_color_t background)
{
    if (!terminal_process_controls_locked(process_id)) {
        return MG_ERR_ACCESS_DENIED;
    }
    terminal_overlay_clear_locked(true);
    terminal_begin_batch_locked();
    for (u64 index = 0; index < length; index++)
        terminal_putc_with_colors_locked(buffer[index], (u8)foreground,
                                         (u8)background);
    terminal_end_batch_locked(true);
    return (i64)length;
}

i64 terminal_write_for_process(u64 process_id, const char *buffer,
                               u64 length)
{
    u64 saved_flags;
    i64 result;

    if (!process_id || (length && !buffer))
        return MG_ERR_BAD_ARGUMENT;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!terminal_process_controls_locked(process_id)) {
        result = MG_ERR_ACCESS_DENIED;
    } else {
        terminal_overlay_clear_locked(true);
        terminal_begin_batch_locked();
        for (u64 index = 0; index < length; index++)
            terminal_putc_locked(buffer[index]);
        terminal_end_batch_locked(true);
        result = (i64)length;
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return result;
}

i64 terminal_write_styled_for_process(u64 process_id, const char *buffer,
                                      u64 length,
                                      terminal_color_t foreground,
                                      terminal_color_t background)
{
    u64 saved_flags;
    i64 result;

    if (!process_id || (length && !buffer) ||
        !terminal_color_valid(foreground) ||
        !terminal_color_valid(background))
        return MG_ERR_BAD_ARGUMENT;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    result = terminal_write_colors_locked(process_id, buffer, length,
                                          foreground, background);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return result;
}

i64 terminal_write_semantic_for_process(u64 process_id, const char *buffer,
                                         u64 length,
                                         terminal_style_role_t role)
{
    u64 saved_flags;
    terminal_style_t style;
    i64 result;

    if (!process_id || (length && !buffer) || role >= TERMINAL_STYLE_COUNT)
        return MG_ERR_BAD_ARGUMENT;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    style = terminal_style_for_role(role);
    result = terminal_write_colors_locked(process_id, buffer, length,
                                          style.foreground, style.background);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return result;
}

typedef struct {
    u8 type;
    u8 foreground;
    u8 background;
    u8 value;
    u32 row;
    u32 column;
    const u8 *text;
    u32 text_length;
} terminal_control_operation_t;

static u32 terminal_control_read_le32(const u8 *bytes)
{
    return (u32)bytes[0] | (u32)bytes[1] << 8 |
           (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
}

static bool terminal_control_read_record(const u8 *bytes, u64 length,
                                         u64 *offset, u8 *type,
                                         const u8 **payload,
                                         u32 *payload_length)
{
    u64 remaining;
    u32 size;

    if (!bytes || !offset || !type || !payload || !payload_length ||
        *offset > length)
        return false;
    remaining = length - *offset;
    if (remaining < TERMINAL_CONTROL_HEADER_SIZE ||
        memcmp(bytes + *offset, "MGTC", 4U) != 0)
        return false;
    if (bytes[*offset + 4U] != MG_TERMINAL_CONTROL_VERSION)
        return false;
    size = terminal_control_read_le32(bytes + *offset + 6U);
    if ((u64)size > remaining - TERMINAL_CONTROL_HEADER_SIZE)
        return false;
    *type = bytes[*offset + 5U];
    *payload_length = size;
    *payload = bytes + *offset + TERMINAL_CONTROL_HEADER_SIZE;
    *offset += TERMINAL_CONTROL_HEADER_SIZE + size;
    return true;
}

static bool terminal_control_style_role(u8 role, u8 *foreground,
                                        u8 *background)
{
    terminal_style_t style;

    if (!foreground || !background || role >= TERMINAL_STYLE_COUNT)
        return false;
    style = terminal_style_for_role((terminal_style_role_t)role);
    *foreground = (u8)style.foreground;
    *background = (u8)style.background;
    return true;
}

static i64 terminal_control_validate(const u8 *bytes, u64 length,
                                     u8 initial_foreground,
                                     u8 initial_background,
                                     u64 *out_record_count,
                                     u64 *out_operation_count,
                                     u8 *out_foreground,
                                     u8 *out_background)
{
    u64 offset = 0;
    u64 records = 0;
    u64 operations = 0;
    u8 foreground = initial_foreground;
    u8 background = initial_background;

    while (offset < length) {
        const u8 *payload;
        u32 payload_length;
        u8 type;

        if (!terminal_control_read_record(bytes, length, &offset, &type,
                                          &payload, &payload_length))
            return MG_ERR_BAD_ARGUMENT;
        if (++records > TERMINAL_CONTROL_MAX_RECORDS)
            return MG_ERR_BUFFER_TOO_SMALL;

        switch (type) {
            case MG_TERMINAL_CONTROL_TEXT:
                if (payload_length) operations++;
                break;
            case MG_TERMINAL_CONTROL_FOREGROUND:
                if (payload_length != 1U ||
                    payload[0] >= TERMINAL_COLOR_COUNT)
                    return MG_ERR_BAD_ARGUMENT;
                foreground = payload[0];
                break;
            case MG_TERMINAL_CONTROL_BACKGROUND:
                if (payload_length != 1U ||
                    payload[0] >= TERMINAL_COLOR_COUNT)
                    return MG_ERR_BAD_ARGUMENT;
                background = payload[0];
                break;
            case MG_TERMINAL_CONTROL_STYLE:
                if (payload_length != 1U ||
                    !terminal_control_style_role(payload[0], &foreground,
                                                 &background))
                    return MG_ERR_BAD_ARGUMENT;
                break;
            case MG_TERMINAL_CONTROL_RESET: {
                terminal_style_t style = terminal_style_for_role(
                    TERMINAL_STYLE_DEFAULT);
                if (payload_length != 0U) return MG_ERR_BAD_ARGUMENT;
                foreground = (u8)style.foreground;
                background = (u8)style.background;
                break;
            }
            case MG_TERMINAL_CONTROL_CURSOR_POSITION:
                if (payload_length != 8U) return MG_ERR_BAD_ARGUMENT;
                operations++;
                break;
            case MG_TERMINAL_CONTROL_CURSOR_VISIBILITY:
                if (payload_length != 1U || payload[0] > 1U)
                    return MG_ERR_BAD_ARGUMENT;
                operations++;
                break;
            case MG_TERMINAL_CONTROL_CLEAR:
                if (payload_length != 1U ||
                    payload[0] < MG_TERMINAL_CONTROL_CLEAR_LINE ||
                    payload[0] > MG_TERMINAL_CONTROL_CLEAR_SCREEN)
                    return MG_ERR_BAD_ARGUMENT;
                operations++;
                break;
            default:
                return MG_ERR_UNSUPPORTED;
        }
    }

    if (out_record_count) *out_record_count = records;
    if (out_operation_count) *out_operation_count = operations;
    if (out_foreground) *out_foreground = foreground;
    if (out_background) *out_background = background;
    return MG_OK;
}

static bool terminal_control_build_plan(
    const u8 *bytes, u64 length, u8 initial_foreground,
    u8 initial_background, terminal_control_operation_t *operations,
    u64 operation_capacity, u64 *out_operation_count)
{
    u64 offset = 0;
    u64 count = 0;
    u8 foreground = initial_foreground;
    u8 background = initial_background;

    while (offset < length) {
        const u8 *payload;
        u32 payload_length;
        u8 type;
        terminal_control_operation_t *operation;

        if (!terminal_control_read_record(bytes, length, &offset, &type,
                                          &payload, &payload_length))
            return false;
        switch (type) {
            case MG_TERMINAL_CONTROL_TEXT:
                if (!payload_length) break;
                if (count >= operation_capacity) return false;
                operation = &operations[count++];
                *operation = (terminal_control_operation_t){0};
                operation->type = type;
                operation->foreground = foreground;
                operation->background = background;
                operation->text = payload;
                operation->text_length = payload_length;
                break;
            case MG_TERMINAL_CONTROL_FOREGROUND:
                foreground = payload[0];
                break;
            case MG_TERMINAL_CONTROL_BACKGROUND:
                background = payload[0];
                break;
            case MG_TERMINAL_CONTROL_STYLE:
                if (!terminal_control_style_role(payload[0], &foreground,
                                                 &background))
                    return false;
                break;
            case MG_TERMINAL_CONTROL_RESET: {
                terminal_style_t style = terminal_style_for_role(
                    TERMINAL_STYLE_DEFAULT);
                foreground = (u8)style.foreground;
                background = (u8)style.background;
                break;
            }
            case MG_TERMINAL_CONTROL_CURSOR_POSITION:
            case MG_TERMINAL_CONTROL_CURSOR_VISIBILITY:
            case MG_TERMINAL_CONTROL_CLEAR:
                if (count >= operation_capacity) return false;
                operation = &operations[count++];
                *operation = (terminal_control_operation_t){0};
                operation->type = type;
                if (type == MG_TERMINAL_CONTROL_CURSOR_POSITION) {
                    operation->row = terminal_control_read_le32(payload);
                    operation->column = terminal_control_read_le32(
                        payload + 4U);
                } else {
                    operation->value = payload[0];
                }
                break;
            default:
                return false;
        }
    }
    *out_operation_count = count;
    return true;
}

void terminal_control_stream_init(terminal_control_stream_state_t *state)
{
    terminal_style_t style;

    if (!state) return;
    mutex_init(&state->write_lock);
    style = terminal_style_for_role(TERMINAL_STYLE_DEFAULT);
    state->foreground = (u8)style.foreground;
    state->background = (u8)style.background;
}

i64 terminal_control_stream_write_for_process(
    u64 process_id, terminal_control_stream_state_t *state,
    const void *records, u64 length)
{
    u8 *copy = NULL;
    terminal_control_operation_t *operations = NULL;
    u64 record_count = 0;
    u64 operation_count = 0;
    u64 built_count = 0;
    u8 final_foreground;
    u8 final_background;
    i64 result;
    u64 saved_flags;

    if (!process_id || !state || (length && !records))
        return MG_ERR_BAD_ARGUMENT;
    if (length > TERMINAL_CONTROL_MAX_BATCH)
        return MG_ERR_BUFFER_TOO_SMALL;
    if (!length) return 0;

    copy = (u8 *)kmalloc((usize)length);
    if (!copy) return MG_ERR_NO_MEMORY;
    memcpy(copy, records, (usize)length);

    if (!mutex_lock(&state->write_lock)) {
        kfree(copy);
        return MG_ERR_BUSY;
    }
    result = terminal_control_validate(copy, length, state->foreground,
        state->background, &record_count, &operation_count,
        &final_foreground, &final_background);
    if (result != MG_OK) goto cleanup;
    if (operation_count > record_count ||
        operation_count > (~(u64)0 / sizeof(*operations)) ||
        operation_count > (~(usize)0 / sizeof(*operations))) {
        result = MG_ERR_BAD_ARGUMENT;
        goto cleanup;
    }
    if (operation_count) {
        operations = (terminal_control_operation_t *)kmalloc(
            (usize)operation_count * sizeof(*operations));
        if (!operations) {
            result = MG_ERR_NO_MEMORY;
            goto cleanup;
        }
        if (!terminal_control_build_plan(copy, length, state->foreground,
                state->background, operations, operation_count, &built_count) ||
            built_count != operation_count) {
            result = MG_ERR_BAD_ARGUMENT;
            goto cleanup;
        }
    }

    saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!terminal_process_controls_locked(process_id)) {
        result = MG_ERR_ACCESS_DENIED;
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        goto cleanup;
    }
    for (u64 index = 0; index < operation_count; index++) {
        terminal_control_operation_t *operation = &operations[index];
        if (operation->type == MG_TERMINAL_CONTROL_CURSOR_POSITION &&
            (operation->row >= terminal.visible_rows ||
             operation->column >= terminal.visible_cols)) {
            result = MG_ERR_BAD_ARGUMENT;
            spin_unlock_irqrestore(&terminal_lock, saved_flags);
            goto cleanup;
        }
    }

    /* Formatting records only change this handle's private state. Do not
     * disturb a completion overlay or schedule a framebuffer update unless
     * the batch contains an operation that actually affects the screen. */
    if (!operation_count) {
        state->foreground = final_foreground;
        state->background = final_background;
        result = (i64)length;
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        goto cleanup;
    }

    terminal_overlay_clear_locked(false);
    terminal_begin_batch_locked();
    for (u64 index = 0; index < operation_count; index++) {
        terminal_control_operation_t *operation = &operations[index];
        switch (operation->type) {
            case MG_TERMINAL_CONTROL_TEXT:
                for (u32 byte = 0; byte < operation->text_length; byte++)
                    terminal_putc_with_colors_locked(
                        (char)operation->text[byte], operation->foreground,
                        operation->background);
                break;
            case MG_TERMINAL_CONTROL_CURSOR_POSITION:
                (void)terminal_move_cursor_locked(operation->row,
                                                   operation->column);
                break;
            case MG_TERMINAL_CONTROL_CURSOR_VISIBILITY:
                terminal_set_cursor_visible_locked(operation->value != 0U);
                break;
            case MG_TERMINAL_CONTROL_CLEAR:
                if (operation->value == MG_TERMINAL_CONTROL_CLEAR_LINE)
                    (void)terminal_clear_current_line_locked();
                else if (operation->value ==
                         MG_TERMINAL_CONTROL_CLEAR_TO_END)
                    (void)terminal_clear_current_to_end_locked();
                else
                    terminal_clear_locked();
                break;
            default:
                break;
        }
    }
    terminal_end_batch_locked(true);
    state->foreground = final_foreground;
    state->background = final_background;
    result = (i64)length;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);

cleanup:
    if (operations) kfree(operations);
    (void)mutex_unlock(&state->write_lock);
    kfree(copy);
    return result;
}

static void terminal_save_normal_state_locked(void)
{
    terminal_saved_state_t *saved = &terminal.saved_normal;

    saved->valid = true;
    saved->top_row_idx = terminal.top_row_idx;
    saved->cursor_phys_row = terminal.cursor_phys_row;
    saved->cursor_col = terminal.cursor_col;
    saved->fg_color = terminal.fg_color;
    saved->bg_color = terminal.bg_color;
    saved->cursor_visible = terminal.cursor_visible;
    saved->cursor_enabled = terminal.cursor_enabled;
    saved->vram_valid = terminal.vram_valid;
    saved->utf8_decoder = terminal.utf8_decoder;
    saved->cursor_blink_pending = cursor_blink_pending;
    saved->cursor_blink_deadline_ms = cursor_blink_deadline_ms;
}

static bool terminal_restore_normal_state_locked(void)
{
    terminal_saved_state_t saved;

    if (!terminal.saved_normal.valid) return false;
    saved = terminal.saved_normal;

    terminal.alternate_active = false;
    terminal.alternate_owner_pid = 0;
    terminal.saved_normal.valid = false;
    terminal.top_row_idx = saved.top_row_idx;
    terminal.cursor_phys_row = saved.cursor_phys_row;
    terminal.cursor_col = saved.cursor_col;
    terminal.fg_color = saved.fg_color;
    terminal.bg_color = saved.bg_color;
    terminal.cursor_enabled = saved.cursor_enabled;
    terminal.utf8_decoder = saved.utf8_decoder;
    terminal.cursor_visible = false;
    terminal.vram_valid = false;
    terminal.pending_scroll_rows = 0;
    batch_depth = 0;
    terminal.batch_active = false;
    terminal_reset_dirty();

    /* The framebuffer currently contains the alternate screen.  Redraw the
     * retained normal ring before restoring the exact cursor blink state. */
    terminal_redraw_locked();
    if (saved.cursor_enabled && !saved.cursor_visible)
        terminal_cursor_hide_locked();
    cursor_blink_pending = saved.cursor_blink_pending;
    cursor_blink_deadline_ms = saved.cursor_blink_deadline_ms;
    if (saved.cursor_visible && !terminal.cursor_visible)
        terminal_cursor_show_locked();
    if (!saved.cursor_enabled)
        terminal.cursor_visible = false;
    if (saved.cursor_enabled && !saved.cursor_visible)
        terminal_flush_dirty_locked();
    return true;
}

bool terminal_alternate_enter_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!process_id || terminal.alternate_active ||
        terminal.key_event_owner_pid != 0) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }

    terminal_force_end_batch_locked();
    terminal_save_normal_state_locked();
    terminal_cursor_hide_locked();
    (void)console_set_raw_input(false, NULL);
    terminal.key_event_owner_pid = 0;

    terminal.alternate_active = true;
    terminal.alternate_owner_pid = process_id;
    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;
    terminal.cursor_visible = false;
    terminal.cursor_enabled = true;
    terminal.pending_scroll_rows = 0;
    terminal.vram_valid = false;
    terminal_reset_dirty();
    utf8_decoder_init(&terminal.utf8_decoder);

    /* terminal_clear() selects the active cell store, so this initializes
     * only the alternate ring and leaves normal scrollback untouched. */
    terminal_clear_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

bool terminal_alternate_leave_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!terminal.alternate_active && process_id != 0 &&
        terminal.key_event_owner_pid == process_id) {
        bool released = console_set_raw_input(false, NULL);
        if (released) terminal.key_event_owner_pid = 0;
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return released;
    }
    if (!terminal_process_is_owner_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_force_end_batch_locked();
    terminal_overlay_clear_locked(false);
    terminal_cursor_hide_locked();
    (void)console_set_raw_input(false, NULL);
    terminal.key_event_owner_pid = 0;
    bool restored = terminal_restore_normal_state_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return restored;
}

bool terminal_alternate_abort(void)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!terminal.alternate_active) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_force_end_batch_locked();
    terminal_overlay_clear_locked(false);
    terminal_cursor_hide_locked();
    (void)console_set_raw_input(false, NULL);
    terminal.key_event_owner_pid = 0;
    bool restored = terminal_restore_normal_state_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return restored;
}

static bool terminal_move_cursor_locked(u32 row, u32 column)
{
    if (row >= terminal.visible_rows || column >= terminal.visible_cols)
        return false;
    terminal_overlay_clear_locked(true);
    terminal_cursor_hide_locked();
    terminal.cursor_phys_row = screen_to_phys_row(row);
    terminal.cursor_col = column;
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    return true;
}

bool terminal_move_cursor(u32 row, u32 column)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool moved = terminal_move_cursor_locked(row, column);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return moved;
}

static void terminal_set_cursor_visible_locked(bool visible)
{
    terminal_overlay_clear_locked(true);
    if (visible) {
        terminal.cursor_enabled = true;
        terminal_cursor_show_locked();
    } else {
        terminal_cursor_hide_locked();
        terminal.cursor_enabled = false;
        cursor_blink_pending = false;
        cursor_blink_deadline_ms = 0;
    }
    if (!terminal.batch_active) terminal_flush_dirty_locked();
}

bool terminal_set_cursor_visible(bool visible)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_set_cursor_visible_locked(visible);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

static void terminal_clear_cell_range(u32 screen_row, u32 first_column,
                                      u32 last_column)
{
    u32 phys_row;

    if (screen_row >= terminal.visible_rows ||
        first_column > last_column || last_column >= terminal.visible_cols)
        return;
    phys_row = screen_to_phys_row(screen_row);
    for (u32 column = first_column; column <= last_column; column++) {
        terminal_cell_t *cell = terminal_cell_at(phys_row, column);
        if (!cell) continue;
        cell->codepoint = ' ';
        cell->fg_color = terminal.fg_color;
        cell->bg_color = terminal.bg_color;
    }
    terminal_render_screen_row(screen_row);
}

static bool terminal_clear_current_line_locked(void)
{
    u32 row = phys_to_screen_row(terminal.cursor_phys_row);
    if (terminal.visible_cols == 0) return false;
    terminal_overlay_clear_locked(true);
    terminal_cursor_hide_locked();
    terminal_clear_cell_range(row, 0, terminal.visible_cols - 1U);
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    return true;
}

bool terminal_clear_current_line(void)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool cleared = terminal_clear_current_line_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return cleared;
}

static bool terminal_clear_current_to_end_locked(void)
{
    u32 row = phys_to_screen_row(terminal.cursor_phys_row);
    if (terminal.visible_cols == 0) return false;
    terminal_overlay_clear_locked(true);
    terminal_cursor_hide_locked();
    terminal_clear_cell_range(row, terminal.cursor_col,
                              terminal.visible_cols - 1U);
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    return true;
}

bool terminal_clear_current_to_end(void)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool cleared = terminal_clear_current_to_end_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return cleared;
}

static bool terminal_clear_cells_locked(u32 first_row, u32 first_column,
                                        u32 last_row, u32 last_column)
{
    if (first_row > last_row || last_row >= terminal.visible_rows ||
        first_column > last_column || last_column >= terminal.visible_cols)
        return false;
    terminal_overlay_clear_locked(true);
    terminal_cursor_hide_locked();
    for (u32 row = first_row; row <= last_row; row++) {
        u32 first = row == first_row ? first_column : 0;
        u32 last = row == last_row ? last_column : terminal.visible_cols - 1U;
        terminal_clear_cell_range(row, first, last);
    }
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    return true;
}

bool terminal_clear_cells(u32 first_row, u32 first_column,
                          u32 last_row, u32 last_column)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool cleared = terminal_clear_cells_locked(first_row, first_column,
                                               last_row, last_column);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return cleared;
}

void terminal_get_dimensions(u32 *rows, u32 *columns)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    if (rows) *rows = terminal.visible_rows;
    if (columns) *columns = terminal.visible_cols;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

bool terminal_overlay_set_for_process(u64 process_id, u32 rows, u32 columns,
                                      const terminal_overlay_cell_t *cells)
{
    u64 saved_flags;
    u32 first_row;
    u32 visible_rows;
    u32 cursor_row;
    u32 cursor_phys_row;
    u32 input_rows = 1U;

    if (!process_id || !cells || !rows || !columns ||
        rows > TERMINAL_MAX_OVERLAY_ROWS || columns > TERMINAL_MAX_COLS)
        return false;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!terminal_process_controls_locked(process_id) ||
        columns > terminal.visible_cols) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }

    for (u64 index = 0; index < (u64)rows * columns; index++) {
        if (cells[index].reserved ||
            !terminal_color_valid(cells[index].foreground) ||
            !terminal_color_valid(cells[index].background)) {
            spin_unlock_irqrestore(&terminal_lock, saved_flags);
            return false;
        }
    }

    cursor_row = phys_to_screen_row(terminal.cursor_phys_row);
    if (cursor_row >= terminal.visible_rows || terminal.visible_rows < 2U) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    if (terminal_input.valid && terminal_input.owner_pid == process_id) {
        u32 anchor_row = phys_to_screen_row(terminal_input.physical_row);
        input_rows = terminal_input.extent ?
            (terminal_input.extent - 1U) / terminal.visible_cols + 1U : 1U;
        if (anchor_row >= terminal.visible_rows ||
            input_rows >= terminal.visible_rows ||
            anchor_row + input_rows > terminal.visible_rows) {
            spin_unlock_irqrestore(&terminal_lock, saved_flags);
            return false;
        }
        cursor_row = anchor_row + input_rows - 1U;
    }
    terminal_overlay_clear_locked(false);
    terminal_cursor_hide_locked();
    /* Reserve display space below the input by scrolling its backing row
     * upward. terminal_scroll advances the output cursor to the new bottom;
     * restore the input's physical row so editing continues at the prompt. */
    if (rows > terminal.visible_rows - input_rows) rows = terminal.visible_rows - input_rows;
    visible_rows = terminal.visible_rows - cursor_row - 1U;
    cursor_phys_row = terminal.cursor_phys_row;
    while (visible_rows < rows) {
        terminal_scroll();
        visible_rows++;
        cursor_row--;
    }
    terminal.cursor_phys_row = cursor_phys_row;
    first_row = cursor_row + 1U;
    terminal_overlay.active = true;
    terminal_overlay.owner_pid = process_id;
    terminal_overlay.first_screen_row = first_row;
    terminal_overlay.rows = rows;
    terminal_overlay.columns = columns;
    for (u64 index = 0; index < (u64)rows * columns; index++) {
        terminal_overlay.cells[index].codepoint = cells[index].codepoint;
        terminal_overlay.cells[index].fg_color = cells[index].foreground;
        terminal_overlay.cells[index].bg_color = cells[index].background;
    }
    terminal_overlay_render_locked();
    terminal_cursor_show_locked();
    if (!terminal.batch_active)
        terminal_flush_dirty_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

bool terminal_overlay_clear_for_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool allowed = terminal_process_controls_locked(process_id);

    if (allowed) terminal_overlay_clear_locked(true);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return allowed;
}

bool terminal_prepare_input_line_for_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (!terminal_process_controls_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    /* The terminal cursor is authoritative for committed output.  A prompt
     * redraw uses an anchored row, so first move to a new display row
     * only when the preceding writer left the cursor partway across one. */
    if (terminal.cursor_col != 0) {
        terminal_cursor_hide_locked();
        terminal_newline();
        terminal_cursor_show_locked();
        if (!terminal.batch_active)
            terminal_flush_dirty_locked();
    }
    terminal_input.valid = true;
    terminal_input.owner_pid = process_id;
    terminal_input.physical_row = terminal.cursor_phys_row;
    terminal_input.extent = 0;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

void terminal_get_capability_mask(u32 *capabilities)
{
    u64 saved_flags;

    if (!capabilities) return;
    saved_flags = spin_lock_irqsave(&terminal_lock);
    *capabilities = TERMINAL_CAP_STYLED_OUTPUT;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

i64 terminal_set_raw_input_for_process(u64 process_id, bool enabled)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    i64 result;

    if (!process_id || (terminal.alternate_active &&
        !terminal_process_is_owner_locked(process_id))) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return MG_ERR_ACCESS_DENIED;
    }
    if (enabled) {
        if (!terminal.alternate_active && terminal.key_event_owner_pid != 0 &&
            terminal.key_event_owner_pid != process_id) {
            spin_unlock_irqrestore(&terminal_lock, saved_flags);
            return MG_ERR_BUSY;
        }
        bool raw_enabled = terminal.alternate_active ?
            console_set_raw_input(true, thread_current()) :
            console_set_raw_input_with_cooked_bytes(true, thread_current());
        if (!raw_enabled)
            result = MG_ERR_BUSY;
        else {
            if (!terminal.alternate_active)
                terminal.key_event_owner_pid = process_id;
            result = MG_OK;
        }
    } else {
        if (!terminal.alternate_active &&
            terminal.key_event_owner_pid != process_id) {
            spin_unlock_irqrestore(&terminal_lock, saved_flags);
            return MG_ERR_ACCESS_DENIED;
        }
        bool released = terminal.alternate_active ?
            console_set_raw_input(false, NULL) :
            console_set_raw_input_preserving_text(false, NULL);
        if (!released)
            result = MG_ERR_BUSY;
        else {
            if (!terminal.alternate_active)
                terminal.key_event_owner_pid = 0;
            result = MG_OK;
        }
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return result;
}

i64 terminal_read_key_for_process(u64 process_id, u32 timeout_ms, u32 *key)
{
    terminal_key_event_t event;
    i64 value;
    u64 saved_flags;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    bool allowed = terminal.alternate_active ?
        terminal_process_is_owner_locked(process_id) :
        (process_id != 0 && terminal.key_event_owner_pid == process_id);
    if (!key || !allowed) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return MG_ERR_ACCESS_DENIED;
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);

    /* A terminal key wait may sleep.  Present the prompt/output before
     * entering that wait, without retaining the terminal spinlock. */
    terminal_force_end_batch();
    value = console_read_key_event(timeout_ms, &event);
    if (value < 0) return value;
    *key = event.codepoint ? event.codepoint : event.keycode;
    return MG_OK;
}

i64 terminal_read_key_event_for_process(u64 process_id, u32 timeout_ms,
                                        terminal_key_event_t *event)
{
    i64 value;
    u64 saved_flags;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    bool allowed = terminal.alternate_active ?
        terminal_process_is_owner_locked(process_id) :
        (process_id != 0 && terminal.key_event_owner_pid == process_id);
    if (!event || !allowed) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return MG_ERR_ACCESS_DENIED;
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);

    terminal_force_end_batch();
    value = console_read_key_event(timeout_ms, event);
    if (value < 0) return value;
    return MG_OK;
}

bool terminal_begin_batch_for_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!terminal_process_controls_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_overlay_clear_locked(true);
    terminal_begin_batch_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

bool terminal_end_batch_for_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!terminal_process_controls_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_end_batch_locked(false);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

/* A native input operation rather than changing carriage-return/backspace
 * semantics for unrelated terminal writers. The anchor survives scrolling. */
bool terminal_input_cursor_for_process(u64 process_id, u32 offset, u32 extent)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    bool allowed = terminal_process_controls_locked(process_id) &&
        terminal_input.valid && terminal_input.owner_pid == process_id &&
        terminal.visible_cols && offset <= extent &&
        extent <= terminal.visible_cols * terminal.visible_rows;
    if (allowed) {
        u32 row_offset = offset ? (offset - 1U) / terminal.visible_cols : 0;
        u32 physical = (terminal_input.physical_row + row_offset) % TERMINAL_BUFFER_ROWS;
        allowed = phys_to_screen_row(physical) < terminal.visible_rows;
        if (allowed) {
            terminal_cursor_hide_locked();
            terminal.cursor_phys_row = physical;
            terminal.cursor_col = offset ? (offset - 1U) % terminal.visible_cols + 1U : 0;
            terminal_input.extent = extent;
            terminal_cursor_show_locked();
            if (!terminal.batch_active) terminal_flush_dirty_locked();
        }
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return allowed;
}
