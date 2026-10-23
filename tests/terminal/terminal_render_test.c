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
#include <mg/line_editor.h>
#include <mg/object.h>

typedef struct {
    u64 glyphs;
    u64 copied_bytes;
    u32 flushes;
} render_measurement_t;

#define EDITOR_TEST_PID 7U
#define EDITOR_TEST_INPUT_CAPACITY 256U

static const char *editor_test_input;
static usize editor_test_input_length;
static usize editor_test_input_offset;
static render_measurement_t editor_test_reads[EDITOR_TEST_INPUT_CAPACITY + 1U];
static usize editor_test_read_count;
static u32 flush_calls;

static render_measurement_t render_measurement(void)
{
    render_measurement_t result = {
        stats.glyph_render_count,
        stats.vram_bytes_copied,
        flush_calls,
    };
    return result;
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    usize count;
    if (handle != MG_STDIN_HANDLE || !buffer || length != 1U)
        return MG_ERR_BAD_ARGUMENT;
    if (editor_test_read_count < EDITOR_TEST_INPUT_CAPACITY + 1U)
        editor_test_reads[editor_test_read_count++] = render_measurement();
    if (editor_test_input_offset >= editor_test_input_length) return 0;
    count = editor_test_input_length - editor_test_input_offset;
    if (count > length) count = length;
    memcpy(buffer, editor_test_input + editor_test_input_offset, count);
    editor_test_input_offset += count;
    return (mg_result_t)count;
}

mg_result_t object_write_all(mg_handle_t handle, const void *buffer,
                             usize length)
{
    if (handle != MG_STDOUT_HANDLE) return MG_ERR_BAD_ARGUMENT;
    return (mg_result_t)terminal_write_for_process(
        EDITOR_TEST_PID, buffer, length);
}

mg_result_t terminal_update_begin(void)
{
    return terminal_begin_batch_for_process(EDITOR_TEST_PID)
        ? MG_OK : MG_ERR_ACCESS_DENIED;
}

mg_result_t terminal_update_end(void)
{
    return terminal_end_batch_for_process(EDITOR_TEST_PID)
        ? MG_OK : MG_ERR_ACCESS_DENIED;
}

mg_result_t terminal_prepare_input_line(void)
{
    return terminal_prepare_input_line_for_process(EDITOR_TEST_PID)
        ? MG_OK : MG_ERR_ACCESS_DENIED;
}

mg_result_t terminal_input_cursor(u32 offset, u32 extent)
{
    return terminal_input_cursor_for_process(EDITOR_TEST_PID, offset, extent)
        ? MG_OK : MG_ERR_ACCESS_DENIED;
}

mg_result_t terminal_write_styled(const void *buffer, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background)
{
    return (mg_result_t)terminal_write_styled_for_process(
        EDITOR_TEST_PID, buffer, length, foreground, background);
}

mg_result_t terminal_cursor_set_visible(bool visible)
{
    return terminal_set_cursor_visible(visible) ? MG_OK : MG_ERR_ACCESS_DENIED;
}

mg_result_t terminal_overlay_clear(void)
{
    return terminal_overlay_clear_for_process(EDITOR_TEST_PID)
        ? MG_OK : MG_ERR_ACCESS_DENIED;
}

mg_result_t terminal_clear_line(void)
{
    return terminal_clear_current_line() ? MG_OK : MG_ERR_ACCESS_DENIED;
}

static void set_editor_test_input(const char *input)
{
    editor_test_input = input;
    editor_test_input_length = strlen(input);
    editor_test_input_offset = 0;
    editor_test_read_count = 0;
}

static mg_result_t replace_with_completion(mg_line_editor_t *editor,
                                            void *context)
{
    const char *replacement = context;
    usize length = strlen(replacement);
    if (length >= editor->capacity) return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(editor->buffer, replacement, length + 1U);
    editor->length = length;
    editor->cursor = length;
    return MG_OK;
}

static mg_result_t replace_after_clearing_prompt(mg_line_editor_t *editor,
                                                 void *context)
{
    mg_result_t result = line_editor_clear_line(editor);
    if (result < 0) return result;
    return replace_with_completion(editor, context);
}

static u32 mock_width;
static u32 drawn_codepoint;
static u32 drawn_x;
static u32 drawn_y;
static u32 drawn_foreground;
static u32 drawn_background;
static u64 mock_time;
static u32 flushed_start[TERMINAL_BUFFER_ROWS];
static u32 flushed_height[TERMINAL_BUFFER_ROWS];

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

static void test_input_redraw_is_presented_as_one_transaction(void)
{
    static const char prompt[] = "developer ~: ";
    static const char input[] = "x";
    const usize prompt_length = sizeof(prompt) - 1U;

    initialize(400U, 160U);
    assert(terminal_prepare_input_line_for_process(7U));
    assert(terminal_write_styled_for_process(
               7U, prompt, prompt_length, TERMINAL_COLOR_GREEN,
               TERMINAL_COLOR_BLACK) == (i64)prompt_length);
    assert(terminal_input_cursor_for_process(7U, (u32)prompt_length,
                                             (u32)prompt_length));

    flush_calls = 0;
    assert(terminal_begin_batch_for_process(7U));
    assert(terminal_input_cursor_for_process(7U, 0U,
                                             (u32)(prompt_length + 1U)));
    assert(flush_calls == 0U);
    assert(terminal_write_styled_for_process(
               7U, prompt, prompt_length, TERMINAL_COLOR_GREEN,
               TERMINAL_COLOR_BLACK) == (i64)prompt_length);
    assert(terminal_write_for_process(7U, input, sizeof(input) - 1U) ==
           (i64)(sizeof(input) - 1U));
    assert(terminal_input_cursor_for_process(
        7U, (u32)(prompt_length + 1U), (u32)(prompt_length + 1U)));
    assert(flush_calls == 0U);
    assert(terminal_end_batch_for_process(7U));

    assert(flush_calls == 1U);
    assert(terminal.cursor_col == prompt_length + 1U);
    assert(screen_cell(0U, 0U)->codepoint == 'd');
    assert(screen_cell(0U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
    assert(screen_cell(0U, prompt_length)->codepoint == 'x');
}

static void test_production_line_editor_incremental_rendering(void)
{
    static const char prompt[] = "developer ~: ";
    static const char typed_input[] = "abcdef\n";
    char buffer[64];
    mg_line_editor_t editor;
    render_measurement_t before;
    render_measurement_t after;
    u64 append_glyphs;
    u32 append_flushes;

    initialize(640U, 240U);
    line_editor_init(&editor, buffer, sizeof(buffer), prompt);
    line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                 MG_TERMINAL_COLOR_BLACK);
    set_editor_test_input(typed_input);
    assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
    terminal_reset_stats();
    flush_calls = 0;
    before = render_measurement();
    assert(line_editor_read_line(&editor) == 6);
    /* The input fixture records the renderer state immediately before each
     * byte is consumed.  The seventh sample follows the sixth typed byte. */
    assert(editor_test_read_count == sizeof(typed_input) - 1U);
    after = editor_test_reads[editor_test_read_count - 1U];
    append_glyphs = after.glyphs - before.glyphs;
    append_flushes = after.flushes - before.flushes;
    /* The same production fixture measured 123 glyph renders immediately
     * before the incremental redraw implementation. */
    printf("line editor append: prior=123 current=%llu (%u%% fewer), "
           "flushes=%u bytes=%llu\n",
           (unsigned long long)append_glyphs,
           (unsigned)((123U - append_glyphs) * 100U / 123U),
           append_flushes,
           (unsigned long long)(after.copied_bytes - before.copied_bytes));
    assert(append_glyphs * 100U <= 123U * 15U);
    assert(append_flushes == 6U);
    assert(strcmp(buffer, "abcdef") == 0);

    {
        static const char cursor_input[] = "abc\x1b[D\n";
        render_measurement_t before_move;
        render_measurement_t after_move;
        initialize(640U, 240U);
        line_editor_init(&editor, buffer, sizeof(buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        set_editor_test_input(cursor_input);
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        terminal_reset_stats();
        flush_calls = 0;
        assert(line_editor_read_line(&editor) == 3);
        assert(editor_test_read_count == sizeof(cursor_input) - 1U);
        before_move = editor_test_reads[5];
        after_move = editor_test_reads[6];
        printf("line editor cursor move: glyphs=%llu flushes=%u\n",
               (unsigned long long)(after_move.glyphs - before_move.glyphs),
               after_move.flushes - before_move.flushes);
        assert(after_move.glyphs - before_move.glyphs == 2U);
        assert(after_move.flushes - before_move.flushes == 1U);
        assert(strcmp(buffer, "abc") == 0);
    }

    {
        static const char middle_input[] = "ac\x1b[Db\n";
        render_measurement_t before_insert;
        render_measurement_t after_insert;
        initialize(640U, 240U);
        line_editor_init(&editor, buffer, sizeof(buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        set_editor_test_input(middle_input);
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        terminal_reset_stats();
        flush_calls = 0;
        assert(line_editor_read_line(&editor) == 3);
        assert(editor_test_read_count == sizeof(middle_input) - 1U);
        before_insert = editor_test_reads[5];
        after_insert = editor_test_reads[6];
        assert(after_insert.glyphs - before_insert.glyphs == 6U);
        assert(after_insert.flushes - before_insert.flushes == 1U);
        assert(strcmp(buffer, "abc") == 0);
        assert(screen_cell(0U, (u32)strlen(prompt))->codepoint == 'a');
        assert(screen_cell(0U, (u32)strlen(prompt) + 1U)->codepoint == 'b');
        assert(screen_cell(0U, (u32)strlen(prompt) + 2U)->codepoint == 'c');
    }

    {
        static const char backspace_input[] = "abc\b\n";
        render_measurement_t before_delete;
        render_measurement_t after_delete;
        initialize(640U, 240U);
        line_editor_init(&editor, buffer, sizeof(buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        set_editor_test_input(backspace_input);
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        terminal_reset_stats();
        flush_calls = 0;
        assert(line_editor_read_line(&editor) == 2);
        before_delete = editor_test_reads[3];
        after_delete = editor_test_reads[4];
        assert(after_delete.glyphs - before_delete.glyphs == 5U);
        assert(after_delete.flushes - before_delete.flushes == 1U);
        assert(strcmp(buffer, "ab") == 0);
        assert(screen_cell(0U, (u32)strlen(prompt) + 2U)->codepoint == ' ');
    }

    {
        static const char history_input[] = {
            'd', 'r', 'a', 'f', 't', EDITOR_ACTION_ESCAPE,
            EDITOR_ACTION_CSI, EDITOR_ACTION_MARKER,
            EDITOR_ACTION_ENCODE(EDITOR_ACTION_HISTORY_PREVIOUS),
            EDITOR_ACTION_ESCAPE, EDITOR_ACTION_CSI, EDITOR_ACTION_MARKER,
            EDITOR_ACTION_ENCODE(EDITOR_ACTION_HISTORY_NEXT), '\n'
        };
        static char history_storage[4U * 32U];
        char draft_storage[32];
        mg_line_history_t history;
        render_measurement_t before_history;
        render_measurement_t after_history;
        render_measurement_t before_draft;
        render_measurement_t after_draft;
        initialize(640U, 240U);
        line_editor_init(&editor, buffer, sizeof(buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        line_editor_history_init(&history, history_storage, 32U, 4U);
        assert(line_editor_history_add_text(&history, "historic-command"));
        line_editor_history_set_draft(&history, draft_storage,
                                      sizeof(draft_storage));
        line_editor_set_history(&editor, &history);
        editor_test_input = history_input;
        editor_test_input_length = sizeof(history_input);
        editor_test_input_offset = 0;
        editor_test_read_count = 0;
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        terminal_reset_stats();
        flush_calls = 0;
        assert(line_editor_read_line(&editor) == 5);
        assert(editor_test_read_count == sizeof(history_input));
        before_history = editor_test_reads[8];
        after_history = editor_test_reads[9];
        before_draft = editor_test_reads[12];
        after_draft = editor_test_reads[13];
        assert(after_history.glyphs - before_history.glyphs == 18U);
        assert(after_history.flushes - before_history.flushes == 1U);
        assert(after_draft.glyphs - before_draft.glyphs == 20U);
        assert(after_draft.flushes - before_draft.flushes == 1U);
        assert(strcmp(buffer, "draft") == 0);
        assert(screen_cell(0U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
    }

    {
        static const char completion_input[] = "x\t\n";
        render_measurement_t before_completion;
        render_measurement_t after_completion;
        initialize(640U, 240U);
        line_editor_init(&editor, buffer, sizeof(buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        line_editor_set_completion(&editor, replace_with_completion,
                                   (void *)"completed-path");
        set_editor_test_input(completion_input);
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        terminal_reset_stats();
        flush_calls = 0;
        assert(line_editor_read_line(&editor) == 14);
        before_completion = editor_test_reads[1];
        after_completion = editor_test_reads[2];
        assert(after_completion.glyphs - before_completion.glyphs == 16U);
        assert(after_completion.flushes - before_completion.flushes == 1U);
        assert(strcmp(buffer, "completed-path") == 0);
        assert(screen_cell(0U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
    }

    {
        static const char completion_input[] = "x\t\n";
        initialize(640U, 240U);
        line_editor_init(&editor, buffer, sizeof(buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        line_editor_set_completion(&editor, replace_after_clearing_prompt,
                                   (void *)"completed-path");
        set_editor_test_input(completion_input);
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        assert(line_editor_read_line(&editor) == 14);
        assert(strcmp(buffer, "completed-path") == 0);
        assert(screen_cell(0U, 0U)->codepoint == 'd');
        assert(screen_cell(0U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
        assert(screen_cell(0U, (u32)strlen(prompt))->codepoint == 'c');
    }

    {
        char long_input[82];
        char long_buffer[128];
        initialize(640U, 240U);
        memset(long_input, 'x', 80U);
        long_input[80] = '\n';
        long_input[81] = '\0';
        line_editor_init(&editor, long_buffer, sizeof(long_buffer), prompt);
        line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                     MG_TERMINAL_COLOR_BLACK);
        set_editor_test_input(long_input);
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        assert(line_editor_read_line(&editor) == 80);
        assert(editor.length == 80U);
        assert(screen_cell(1U, 0U)->codepoint == 'x');
        assert(screen_cell(1U, 15U)->codepoint == 'x');
    }
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
    if (run_all || !strcmp(arguments[1], "input-redraw"))
        test_input_redraw_is_presented_as_one_transaction();
    if (run_all || !strcmp(arguments[1], "editor-incremental"))
        test_production_line_editor_incremental_rendering();
    puts("terminal renderer: geometry, wrapped input, CSI, UTF-8, alternate screen and incremental line editing passed");
    return 0;
}
