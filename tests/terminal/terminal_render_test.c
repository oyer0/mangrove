/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deterministic framebuffer-cell fixture for the production terminal source. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <spinlock.h>
#include <timer.h>

#ifndef TERMINAL_RENDER_SOURCE
#define TERMINAL_RENDER_SOURCE "../../kernel/src/terminal.c"
#endif
#include TERMINAL_RENDER_SOURCE

static u32 mock_width;
static u32 drawn_codepoint;
static u32 drawn_x;
static u32 drawn_y;
static u32 drawn_foreground;
static u32 drawn_background;
static u64 mock_time;
static u32 flushed_start[TERMINAL_BUFFER_ROWS];
static u32 flushed_height[TERMINAL_BUFFER_ROWS];
static u32 flush_calls;

void spinlock_init(spinlock_t *lock) { lock->value = 0; }
u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock && !lock->value);
    lock->value = 1;
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    assert(lock && lock->value);
    lock->value = 0;
}

u64 timer_uptime_ms(void) { return ++mock_time; }
u32 font_width(void) { return 8U; }
u32 font_height(void) { return 16U; }
u32 framebuffer_pitch(void) { return mock_width; }
void framebuffer_fill_rows(u32 start, u32 count, u32 color)
{
    (void)start;
    (void)count;
    (void)color;
}
void framebuffer_copy_rows(u32 destination, u32 source, u32 count)
{
    (void)destination;
    (void)source;
    (void)count;
}
void framebuffer_flush_rows(u32 start, u32 count)
{
    if (flush_calls < TERMINAL_BUFFER_ROWS) {
        flushed_start[flush_calls] = start;
        flushed_height[flush_calls] = count;
    }
    flush_calls++;
}
bool console_set_raw_input(bool enabled, struct kernel_thread *owner)
{
    (void)enabled;
    (void)owner;
    return true;
}
void draw_codepoint(u32 codepoint, u32 x, u32 y, u32 foreground,
                    u32 background)
{
    drawn_codepoint = codepoint;
    drawn_x = x;
    drawn_y = y;
    drawn_foreground = foreground;
    drawn_background = background;
}

static void initialize(u32 width, u32 height)
{
    BOOT_INFO boot = {0};

    mock_width = width;
    boot.FramebufferWidth = width;
    boot.FramebufferHeight = height;
    boot.PixelsPerScanLine = width;
    terminal_init(&boot);
    terminal_reset_stats();
    flush_calls = 0;
    drawn_codepoint = 0;
    drawn_x = drawn_y = 0;
    drawn_foreground = drawn_background = 0;
}

static terminal_cell_t *screen_cell(u32 row, u32 column)
{
    return terminal_cell_at(screen_to_phys_row(row), column);
}

static void write_codepoint(u32 value)
{
    terminal_put_codepoint(value);
}

static void write_sequence(const u32 *values, usize count)
{
    for (usize index = 0; index < count; index++)
        terminal_put_codepoint(values[index]);
}

static void test_dimensions_fit_owned_ring(void)
{
    initialize(640U, 10000U);
    assert(terminal.visible_cols == 77U);
    assert(terminal.visible_rows == TERMINAL_BUFFER_ROWS);

    initialize(10U, 10U);
    assert(terminal.visible_cols == 1U);
    assert(terminal.visible_rows == 1U);
    assert(terminal.top_row_idx == 0U);
}

static void test_ring_wrap_and_scrolling(void)
{
    initialize(60U, 56U);
    assert(terminal.visible_cols == 5U && terminal.visible_rows == 2U);

    for (u32 line = 0; line < TERMINAL_BUFFER_ROWS + 8U; line++) {
        write_codepoint((u32)'A' + line % 26U);
        write_codepoint('\n');
    }

    assert(terminal.top_row_idx == 7U);
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);
    assert(screen_cell(0U, 0U)->codepoint == 'Z');
    for (u32 column = 0; column < terminal.visible_cols; column++)
        assert(screen_cell(1U, column)->codepoint == ' ');
}

static void test_tab_stops_and_deferred_wrap(void)
{
    initialize(148U, 56U);
    assert(terminal.visible_cols == 16U && terminal.visible_rows == 2U);

    write_codepoint('a');
    write_codepoint('\t');
    assert(terminal.cursor_col == 8U);
    assert(screen_cell(0U, 0U)->codepoint == 'a');
    for (u32 column = 1U; column < 8U; column++)
        assert(screen_cell(0U, column)->codepoint == ' ');

    write_codepoint('\t');
    assert(terminal.cursor_col == 16U);
    write_codepoint('b');
    assert(terminal.cursor_col == 1U);
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);
    assert(screen_cell(1U, 0U)->codepoint == 'b');
}

static void test_wrap_cursor_stays_in_last_cell(void)
{
    initialize(60U, 56U);
    write_codepoint('A');
    write_codepoint('B');
    write_codepoint('C');
    write_codepoint('D');
    write_codepoint('E');

    assert(terminal.cursor_col == terminal.visible_cols);
    assert(drawn_codepoint == 'E');
    assert(drawn_x == TERMINAL_MARGIN_X +
                      (terminal.visible_cols - 1U) * font_width());
    assert(drawn_y == TERMINAL_MARGIN_Y);

    write_codepoint('F');
    assert(terminal.cursor_col == 1U);
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);
    assert(screen_cell(1U, 0U)->codepoint == 'F');
}

static void test_csi_parameters_are_local_and_unknown_sequences_are_consumed(void)
{
    static const u32 clear_screen[] = {0x1bU, '[', '2', 'J'};
    static const u32 unparameterized_clear[] = {0x1bU, '[', 'J'};

    initialize(100U, 56U);
    terminal.top_row_idx = 4U;
    terminal.cursor_phys_row = 5U;
    terminal.cursor_col = 3U;
    terminal_cell_at(4U, 0U)->codepoint = 'Q';
    write_sequence(clear_screen, sizeof(clear_screen) / sizeof(clear_screen[0]));
    assert(terminal.top_row_idx == 0U && terminal.cursor_col == 0U);
    assert(screen_cell(0U, 0U)->codepoint == ' ');

    write_codepoint('P');
    terminal.cursor_col = 4U;
    write_sequence(unparameterized_clear,
                   sizeof(unparameterized_clear) /
                       sizeof(unparameterized_clear[0]));
    assert(terminal.cursor_col == 4U);
    assert(screen_cell(0U, 0U)->codepoint == 'P');

}

static void test_unknown_csi_is_consumed(void)
{
    static const u32 unsupported_color[] = {0x1bU, '[', '3', '1', 'm'};
    initialize(100U, 56U);
    write_sequence(unsupported_color,
                   sizeof(unsupported_color) /
                       sizeof(unsupported_color[0]));
    assert(terminal.cursor_col == 0U);
    for (u32 column = 0; column < terminal.visible_cols; column++)
        assert(screen_cell(0U, column)->codepoint == ' ');
}

static void test_supported_sgr_and_cr_backspace_lf(void)
{
    static const u32 inverse_on[] = {0x1bU, '[', '7', 'm'};
    static const u32 inverse_off[] = {0x1bU, '[', '0', 'm'};

    initialize(100U, 56U);
    write_sequence(inverse_on, sizeof(inverse_on) / sizeof(inverse_on[0]));
    write_codepoint('I');
    assert(screen_cell(0U, 0U)->fg_color == terminal.bg_color);
    assert(screen_cell(0U, 0U)->bg_color == terminal.fg_color);
    write_sequence(inverse_off,
                   sizeof(inverse_off) / sizeof(inverse_off[0]));
    write_codepoint('N');
    assert(screen_cell(0U, 1U)->fg_color == terminal.fg_color);
    assert(screen_cell(0U, 1U)->bg_color == terminal.bg_color);

    write_codepoint('\r');
    assert(terminal.cursor_col == 0U);
    write_codepoint('X');
    write_codepoint('\b');
    assert(terminal.cursor_col == 0U);
    write_codepoint('\n');
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);
}

static void test_utf8_across_write_boundaries(void)
{
    static const char first[] = {(char)0xe2};
    static const char remainder[] = {(char)0x82, (char)0xac};
    static const char malformed[] = {(char)0xe2, 'A'};

    initialize(100U, 56U);
    terminal_write_bytes(first, sizeof(first));
    assert(terminal.cursor_col == 0U);
    terminal_write_bytes(remainder, sizeof(remainder));
    assert(terminal.cursor_col == 1U);
    assert(screen_cell(0U, 0U)->codepoint == 0x20acU);

    terminal_clear();
    terminal_write_bytes(malformed, sizeof(malformed));
    assert(screen_cell(0U, 0U)->codepoint == UTF8_REPLACEMENT_CODEPOINT);
    assert(screen_cell(0U, 1U)->codepoint == 'A');
}

static void test_alternate_screen_restores_normal_ring(void)
{
    initialize(60U, 56U);
    assert(terminal_write_for_process(1U, "normal", 6U) == 6);
    assert(terminal.cursor_col == 1U);
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);

    assert(terminal_alternate_enter_process(42U));
    assert(terminal.alternate_active && terminal.alternate_owner_pid == 42U);
    assert(terminal.cursor_col == 0U);
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 0U);
    assert(screen_cell(0U, 0U)->codepoint == ' ');
    assert(terminal_write_for_process(1U, "blocked", 7U) ==
           MG_ERR_ACCESS_DENIED);
    assert(terminal_write_for_process(42U, "alternate", 9U) == 9);
    assert(screen_cell(0U, 0U)->codepoint == 'a');
    assert(!terminal_alternate_leave_process(1U));
    assert(terminal.alternate_active);

    assert(terminal_alternate_leave_process(42U));
    assert(!terminal.alternate_active && !terminal.saved_normal.valid);
    assert(terminal.cursor_col == 1U);
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);
    assert(screen_cell(0U, 0U)->codepoint == 'n');
    assert(screen_cell(0U, 1U)->codepoint == 'o');
    assert(screen_cell(0U, 4U)->codepoint == 'a');
    assert(screen_cell(1U, 0U)->codepoint == 'l');
}

static void test_disjoint_dirty_rows_do_not_copy_clean_rows(void)
{
    initialize(640U, 480U);
    terminal_reset_dirty();
    terminal_mark_dirty(1U);
    terminal_mark_dirty(terminal.visible_rows - 1U);
    terminal_flush_dirty_locked();
    assert(flush_calls == 2U);
    assert(flushed_start[0] == TERMINAL_MARGIN_Y + terminal_line_height());
    assert(flushed_height[0] == terminal_line_height());
    assert(flushed_start[1] == TERMINAL_MARGIN_Y +
           (terminal.visible_rows - 1U) * terminal_line_height());
    assert(flushed_height[1] == terminal_line_height());
    assert(stats.vram_bytes_copied ==
           2ULL * terminal_line_height() * mock_width * sizeof(u32));
    terminal_flush_dirty_locked();
    assert(flush_calls == 2U);

    flush_calls = 0;
    terminal_mark_dirty(2U);
    terminal_mark_dirty(4U);
    terminal_mark_dirty(3U);
    terminal_flush_dirty_locked();
    assert(flush_calls == 1U);
    assert(flushed_height[0] == 3U * terminal_line_height());

    flush_calls = 0;
    terminal_mark_all_dirty();
    terminal_flush_dirty_locked();
    assert(flush_calls == 1U);
    assert(flushed_height[0] == terminal.visible_rows * terminal_line_height());
}

int main(int argument_count, char **arguments)
{
    bool run_all = argument_count == 1;

    if (run_all || !strcmp(arguments[1], "geometry"))
        test_dimensions_fit_owned_ring();
    if (run_all || !strcmp(arguments[1], "ring"))
        test_ring_wrap_and_scrolling();
    if (run_all || !strcmp(arguments[1], "tab"))
        test_tab_stops_and_deferred_wrap();
    if (run_all || !strcmp(arguments[1], "cursor"))
        test_wrap_cursor_stays_in_last_cell();
    if (run_all || !strcmp(arguments[1], "csi"))
        test_csi_parameters_are_local_and_unknown_sequences_are_consumed();
    if (run_all || !strcmp(arguments[1], "unknown"))
        test_unknown_csi_is_consumed();
    if (run_all || !strcmp(arguments[1], "controls"))
        test_supported_sgr_and_cr_backspace_lf();
    if (run_all || !strcmp(arguments[1], "utf8"))
        test_utf8_across_write_boundaries();
    if (run_all || !strcmp(arguments[1], "alternate"))
        test_alternate_screen_restores_normal_ring();
    if (run_all || !strcmp(arguments[1], "dirty"))
        test_disjoint_dirty_rows_do_not_copy_clean_rows();
    puts("terminal renderer: geometry, ring wrap, tabs, cursor, CSI, UTF-8 and alternate-screen restoration passed");
    return 0;
}
