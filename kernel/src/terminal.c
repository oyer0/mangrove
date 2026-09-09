/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <terminal.h>
#include <font.h>
#include <framebuffer.h>
#include <timer.h>
#include <utf8.h>
#include <console.h>
#include <scheduler.h>
#include <spinlock.h>
#include <mangrove_errors.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define TERMINAL_MARGIN_X 10
#define TERMINAL_MARGIN_Y 10
#define TERMINAL_LINE_SPACING 2

#define TERMINAL_FG_COLOR 0x0B6623
#define TERMINAL_BG_COLOR 0xFFFFFF

#define TERMINAL_BUFFER_ROWS 512
#define TERMINAL_MAX_COLS    256
#define TERMINAL_CURSOR_BLINK_INTERVAL_MS 500ULL
#define TERMINAL_PRESENTATION_INTERVAL_MS 16ULL

#define TERMINAL_ESCAPE_NONE 0U
#define TERMINAL_ESCAPE_SEEN 1U
#define TERMINAL_ESCAPE_CSI  2U
#define TERMINAL_ESCAPE_SGR  3U

typedef struct {
    u32 codepoint;
    u32 fg_color;
    u32 bg_color;
} terminal_cell_t;

typedef struct {
    bool valid;
    u32 top_row_idx;
    u32 cursor_phys_row;
    u32 cursor_col;
    u32 fg_color;
    u32 bg_color;
    bool cursor_visible;
    bool cursor_enabled;
    bool vram_valid;
    bool inverse_video;
    u8 escape_state;
    char escape_parameter;
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

    u32 fg_color;
    u32 bg_color;

    u32 visible_cols;       // Visible columns count
    u32 visible_rows;       // Visible rows count
    u32 width;
    u32 height;

    bool cursor_visible;
    bool cursor_enabled;
    bool vram_valid;        // Indicates if VRAM cache is synchronized with ring buffer
    bool inverse_video;
    u8 escape_state;
    char escape_parameter;
    utf8_decoder_t utf8_decoder;

    /* Dirty tracking & hybrid scroll batching */
    u32 dirty_min_row;      // Minimum dirty screen row (inclusive)
    u32 dirty_max_row;      // Maximum dirty screen row (inclusive)
    bool batch_active;      // True when inside begin_batch/end_batch
    u32 pending_scroll_rows; // Accumulated pending scroll rows during batch

    bool alternate_active;
    u64 alternate_owner_pid;
    terminal_saved_state_t saved_normal;
} terminal_t;

static terminal_t terminal;
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

static void terminal_clear_phys_row(u32 phys_row, u32 bg_color) {
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
static void terminal_mark_dirty(u32 screen_row) {
    if (screen_row >= terminal.visible_rows) return;
    if (screen_row < terminal.dirty_min_row)
        terminal.dirty_min_row = screen_row;
    if (screen_row > terminal.dirty_max_row)
        terminal.dirty_max_row = screen_row;
}

/* Mark all visible rows as dirty */
static void terminal_mark_all_dirty(void) {
    terminal.dirty_min_row = 0;
    terminal.dirty_max_row = terminal.visible_rows - 1;
}

/* Reset dirty tracking */
static void terminal_reset_dirty(void) {
    terminal.dirty_min_row = terminal.visible_rows;
    terminal.dirty_max_row = 0;
}

/* Flush dirty rows from backbuffer to VRAM */
static void terminal_flush_dirty_locked(void) {
    if (terminal.dirty_min_row > terminal.dirty_max_row) return;

    u32 line_h = terminal_line_height();
    u32 start_y = TERMINAL_MARGIN_Y + (terminal.dirty_min_row * line_h);
    u32 end_y = TERMINAL_MARGIN_Y + ((terminal.dirty_max_row + 1) * line_h);

    if (end_y > terminal.height) end_y = terminal.height;
    if (start_y >= end_y) return;

    u32 flush_h = end_y - start_y;
    framebuffer_flush_rows(start_y, flush_h);

    stats.vram_flush_count++;
    stats.vram_bytes_copied += (u64)flush_h * framebuffer_pitch() * sizeof(u32);

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

    draw_codepoint(codepoint, px, py, fg, bg);
    stats.glyph_render_count++;
    terminal_mark_dirty(screen_row);
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

        draw_codepoint(codepoint, px, py, fg, bg);
        stats.glyph_render_count++;
    }
    terminal_mark_dirty(screen_row);
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

    terminal.fg_color = TERMINAL_FG_COLOR;
    terminal.bg_color = TERMINAL_BG_COLOR;

    terminal.width  = BootInfo->FramebufferWidth;
    terminal.height = BootInfo->FramebufferHeight;

    terminal.visible_cols = (terminal.width - (TERMINAL_MARGIN_X * 2)) / font_width();
    if (terminal.visible_cols > TERMINAL_MAX_COLS) {
        terminal.visible_cols = TERMINAL_MAX_COLS;
    }

    u32 line_h = terminal_line_height();
    terminal.visible_rows = (terminal.height - (TERMINAL_MARGIN_Y * 2)) / line_h;

    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;

    terminal.cursor_visible = false;
    terminal.cursor_enabled = true;
    terminal.vram_valid = false;
    terminal.inverse_video = false;
    terminal.escape_state = TERMINAL_ESCAPE_NONE;
    terminal.escape_parameter = 0;
    terminal.batch_active = false;
    terminal.pending_scroll_rows = 0;
    terminal.alternate_active = false;
    terminal.alternate_owner_pid = 0;
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
    if (!terminal.cursor_enabled || terminal.cursor_visible)
        return;

    u32 srow = phys_to_screen_row(terminal.cursor_phys_row);
    if (srow >= terminal.visible_rows)
        return;

    u32 px = TERMINAL_MARGIN_X + (terminal.cursor_col * font_width());
    u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());

    terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row,
                                             terminal.cursor_col);
    if (cell && cell->codepoint && cell->codepoint != ' ') {
        draw_codepoint(cell->codepoint, px, py, cell->bg_color,
                       cell->fg_color);
    } else {
        draw_codepoint(' ', px, py, terminal.bg_color, terminal.fg_color);
    }
    stats.glyph_render_count++;
    terminal_mark_dirty(srow);
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
        u32 px = TERMINAL_MARGIN_X + (terminal.cursor_col * font_width());
        u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());

        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, terminal.cursor_col);
        u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;
        draw_codepoint(codepoint, px, py, fg, bg);
        stats.glyph_render_count++;
        terminal_mark_dirty(srow);
    }

    terminal.cursor_visible = false;
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
        terminal.bg_color
    );

    u32 bottom_py = TERMINAL_MARGIN_Y + copy_height;
    for (u32 c = 0; c < terminal.visible_cols; c++) {
        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, c);
        u32 px = TERMINAL_MARGIN_X + (c * font_width());
        u32 codepoint = (cell && cell->codepoint) ? cell->codepoint : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;
        draw_codepoint(codepoint, px, bottom_py, fg, bg);
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

static void terminal_put_codepoint_locked(u32 codepoint) {
    if (!terminal.batch_active) terminal_cursor_hide_locked();

    if (terminal.escape_state == TERMINAL_ESCAPE_SEEN) {
        terminal.escape_state = codepoint == '[' ? TERMINAL_ESCAPE_CSI
                                                  : TERMINAL_ESCAPE_NONE;
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }
    if (terminal.escape_state == TERMINAL_ESCAPE_CSI) {
        if (codepoint == '0' || codepoint == '7') {
            terminal.escape_parameter = (char)codepoint;
            terminal.escape_state = TERMINAL_ESCAPE_SGR;
        } else if (codepoint == '2') {
            terminal.escape_parameter = '2';
        } else if (codepoint == 'J' && terminal.escape_parameter == '2') {
            terminal_clear_locked();
            terminal.escape_state = TERMINAL_ESCAPE_NONE;
        } else {
            terminal.escape_state = TERMINAL_ESCAPE_NONE;
        }
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }
    if (terminal.escape_state == TERMINAL_ESCAPE_SGR) {
        if (codepoint == 'm') {
            terminal.inverse_video = terminal.escape_parameter == '7';
        }
        terminal.escape_state = TERMINAL_ESCAPE_NONE;
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }
    if (codepoint == 0x1BU) {
        terminal.escape_state = TERMINAL_ESCAPE_SEEN;
        if (!terminal.batch_active) {
            terminal_cursor_show_locked();
            terminal_flush_dirty_locked();
        }
        return;
    }

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

    if (terminal.cursor_col >= terminal.visible_cols) {
        terminal_newline();
    }

    /* Track in text buffer */
    terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, terminal.cursor_col);
    if (cell) {
        cell->codepoint = codepoint;
        cell->fg_color = terminal.inverse_video ? terminal.bg_color
                                                : terminal.fg_color;
        cell->bg_color = terminal.inverse_video ? terminal.fg_color
                                                : terminal.bg_color;
    }

    /* Render cell into RAM backbuffer */
    if (!terminal.batch_active || terminal.pending_scroll_rows == 0) {
        terminal_render_cell(terminal.cursor_phys_row, terminal.cursor_col);
    }

    terminal.cursor_col++;

    /* Flush immediately if not in batch mode */
    if (!terminal.batch_active) {
        terminal_cursor_show_locked();
        terminal_flush_dirty_locked();
    }
}

void terminal_put_codepoint(u32 codepoint)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_put_codepoint_locked(codepoint);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static void terminal_putc_locked(char c) {
    utf8_decode_result_t result = utf8_decoder_feed(&terminal.utf8_decoder,
                                                     (u8)c);
    for (u8 index = 0; index < result.count; index++)
        terminal_put_codepoint_locked(result.codepoints[index]);
}

void terminal_putc(char c)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_putc_locked(c);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_write_bytes(const char *buffer, u64 length)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal_begin_batch_locked();
    for (u64 i = 0; i < length; i++)
        terminal_putc_locked(buffer[i]);
    terminal_end_batch_locked(true);
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_write(const char *str) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (str) {
        terminal_begin_batch_locked();
        while (*str)
            terminal_putc_locked(*str++);
        terminal_end_batch_locked(false);
    }

    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

static void terminal_clear_locked(void) {
    terminal_cursor_hide_locked();

    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;
    terminal.inverse_video = false;
    terminal.escape_state = TERMINAL_ESCAPE_NONE;
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
    framebuffer_fill_rows(0, terminal.height, terminal.bg_color);
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

void terminal_set_color(u32 color) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal.fg_color = color;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

void terminal_set_background(u32 color) {
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    terminal.bg_color = color;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
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
            terminal.bg_color
        );

        u32 start_screen_row = terminal.visible_rows - count;
        for (u32 r = start_screen_row; r < terminal.visible_rows; r++)
            terminal_render_screen_row(r);

        terminal.pending_scroll_rows = 0;
        terminal_mark_all_dirty();
        terminal_cursor_show_locked();
        terminal_flush_dirty_locked();
    } else {
        terminal.pending_scroll_rows = 0;
        terminal_redraw_locked();
    }
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
    saved->inverse_video = terminal.inverse_video;
    saved->escape_state = terminal.escape_state;
    saved->escape_parameter = terminal.escape_parameter;
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
    terminal.inverse_video = saved.inverse_video;
    terminal.escape_state = saved.escape_state;
    terminal.escape_parameter = saved.escape_parameter;
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

    if (!process_id || terminal.alternate_active) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }

    terminal_force_end_batch_locked();
    terminal_save_normal_state_locked();
    terminal_cursor_hide_locked();
    (void)console_set_raw_input(false, NULL);

    terminal.alternate_active = true;
    terminal.alternate_owner_pid = process_id;
    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;
    terminal.cursor_visible = false;
    terminal.cursor_enabled = true;
    terminal.inverse_video = false;
    terminal.escape_state = TERMINAL_ESCAPE_NONE;
    terminal.escape_parameter = 0;
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

    if (!terminal_process_is_owner_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_force_end_batch_locked();
    terminal_cursor_hide_locked();
    (void)console_set_raw_input(false, NULL);
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
    terminal_cursor_hide_locked();
    (void)console_set_raw_input(false, NULL);
    bool restored = terminal_restore_normal_state_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return restored;
}

bool terminal_move_cursor(u32 row, u32 column)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (row >= terminal.visible_rows || column >= terminal.visible_cols)
    {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_cursor_hide_locked();
    terminal.cursor_phys_row = screen_to_phys_row(row);
    terminal.cursor_col = column;
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

bool terminal_set_cursor_visible(bool visible)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

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

bool terminal_clear_current_line(void)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    u32 row = phys_to_screen_row(terminal.cursor_phys_row);
    if (terminal.visible_cols == 0) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_cursor_hide_locked();
    terminal_clear_cell_range(row, 0, terminal.visible_cols - 1U);
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

bool terminal_clear_current_to_end(void)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    u32 row = phys_to_screen_row(terminal.cursor_phys_row);
    if (terminal.visible_cols == 0) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_cursor_hide_locked();
    terminal_clear_cell_range(row, terminal.cursor_col,
                              terminal.visible_cols - 1U);
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

bool terminal_clear_cells(u32 first_row, u32 first_column,
                          u32 last_row, u32 last_column)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);

    if (first_row > last_row || last_row >= terminal.visible_rows ||
        first_column > last_column || last_column >= terminal.visible_cols)
    {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
    terminal_cursor_hide_locked();
    for (u32 row = first_row; row <= last_row; row++) {
        u32 first = row == first_row ? first_column : 0;
        u32 last = row == last_row ? last_column : terminal.visible_cols - 1U;
        terminal_clear_cell_range(row, first, last);
    }
    terminal_cursor_show_locked();
    if (!terminal.batch_active) terminal_flush_dirty_locked();
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return true;
}

void terminal_get_dimensions(u32 *rows, u32 *columns)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    if (rows) *rows = terminal.visible_rows;
    if (columns) *columns = terminal.visible_cols;
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
}

i64 terminal_set_raw_input_for_process(u64 process_id, bool enabled)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    i64 result;

    if (!terminal.alternate_active ||
        !terminal_process_is_owner_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return MG_ERR_ACCESS_DENIED;
    }
    if (enabled) {
        if (!console_set_raw_input(true, thread_current()))
            result = MG_ERR_BUSY;
        else
            result = MG_OK;
    } else {
        if (!console_set_raw_input(false, NULL))
            result = MG_ERR_BUSY;
        else
            result = MG_OK;
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);
    return result;
}

i64 terminal_read_key_for_process(u64 process_id, u32 timeout_ms, u32 *key)
{
    i64 value;
    u64 saved_flags;

    saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!key || !terminal.alternate_active ||
        !terminal_process_is_owner_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return MG_ERR_ACCESS_DENIED;
    }
    spin_unlock_irqrestore(&terminal_lock, saved_flags);

    /* A terminal key wait may sleep.  Present the prompt/output before
     * entering that wait, without retaining the terminal spinlock. */
    terminal_force_end_batch();
    value = console_read_key(timeout_ms);
    if (value < 0) return value;
    *key = (u32)value;
    return MG_OK;
}

bool terminal_begin_batch_for_process(u64 process_id)
{
    u64 saved_flags = spin_lock_irqsave(&terminal_lock);
    if (!terminal_process_controls_locked(process_id)) {
        spin_unlock_irqrestore(&terminal_lock, saved_flags);
        return false;
    }
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
