/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deterministic framebuffer-pixel fixture for the production terminal source. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct {
    bool cursor_visible;
    u32 cursor_phys_row;
    u32 cursor_col;
    u32 drawn_codepoint;
    u32 drawn_x;
    u32 drawn_y;
    u32 drawn_foreground;
    u32 drawn_background;
    u32 cell_codepoint;
    u8 cell_foreground;
    u8 cell_background;
} presented_frame_t;

#define EDITOR_TEST_PID 7U
#define EDITOR_TEST_INPUT_CAPACITY 256U

static const char *editor_test_input;
static usize editor_test_input_length;
static usize editor_test_input_offset;
static render_measurement_t editor_test_reads[EDITOR_TEST_INPUT_CAPACITY + 1U];
static usize editor_test_read_count;
static u32 flush_calls;
static presented_frame_t presented_frames[TERMINAL_BUFFER_ROWS];
static kernel_thread_t mock_input_thread;
static bool mock_raw_input;
static bool mock_raw_input_includes_cooked;
static u32 preserving_release_count;

kernel_thread_t *thread_current(void)
{
    return &mock_input_thread;
}

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

mg_result_t terminal_clear_screen(void)
{
    terminal_clear();
    return MG_OK;
}

mg_result_t terminal_input_raw(bool enabled)
{
    (void)enabled;
    return MG_OK;
}

mg_result_t terminal_read_key_event(u32 timeout_ms,
                                    mg_terminal_key_event_t *out_event)
{
    (void)timeout_ms;
    (void)out_event;
    return MG_ERR_END_OF_FILE;
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
static u32 mock_height;
static u32 *pixel_backbuffer;
static u32 *pixel_display;
static u32 drawn_codepoint;
static u32 drawn_x;
static u32 drawn_y;
static u32 drawn_foreground;
static u32 drawn_background;
static u64 mock_time;
static u32 flushed_start[TERMINAL_BUFFER_ROWS];
static u32 flushed_height[TERMINAL_BUFFER_ROWS];

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}
u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock);
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE)) { }
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    assert(lock && __atomic_load_n(&lock->value, __ATOMIC_RELAXED));
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

void mutex_init(mutex_t *mutex)
{
    spinlock_init(&mutex->state_lock);
    mutex->owner = NULL;
    mutex->wait_head = NULL;
    mutex->wait_tail = NULL;
}

bool mutex_lock(mutex_t *mutex)
{
    if (!mutex) return false;
    u64 flags = spin_lock_irqsave(&mutex->state_lock);
    if (mutex->owner) {
        spin_unlock_irqrestore(&mutex->state_lock, flags);
        return false;
    }
    mutex->owner = (struct kernel_thread *)(uintptr_t)1U;
    spin_unlock_irqrestore(&mutex->state_lock, flags);
    return true;
}

bool mutex_unlock(mutex_t *mutex)
{
    if (!mutex) return false;
    u64 flags = spin_lock_irqsave(&mutex->state_lock);
    if (mutex->owner != (struct kernel_thread *)(uintptr_t)1U) {
        spin_unlock_irqrestore(&mutex->state_lock, flags);
        return false;
    }
    mutex->owner = NULL;
    spin_unlock_irqrestore(&mutex->state_lock, flags);
    return true;
}

void *kmalloc(usize size) { return malloc(size); }
void kfree(void *pointer) { free(pointer); }

u64 timer_uptime_ms(void) { return ++mock_time; }
u32 font_width(void) { return 8U; }
u32 font_height(void) { return 16U; }
u32 framebuffer_pitch(void) { return mock_width; }
void framebuffer_fill_rows(u32 start, u32 count, u32 color)
{
    if (start >= mock_height) return;
    if (count > mock_height - start) count = mock_height - start;
    for (usize pixel = (usize)start * mock_width;
         pixel < (usize)(start + count) * mock_width; pixel++)
        pixel_backbuffer[pixel] = color;
}
void framebuffer_copy_rows(u32 destination, u32 source, u32 count)
{
    assert(destination <= mock_height && source <= mock_height);
    assert(count <= mock_height - destination && count <= mock_height - source);
    memmove(pixel_backbuffer + (usize)destination * mock_width,
            pixel_backbuffer + (usize)source * mock_width,
            (usize)count * mock_width * sizeof(u32));
}
static void record_flush(u32 start, u32 count)
{
    if (flush_calls < TERMINAL_BUFFER_ROWS) {
        presented_frame_t *frame = &presented_frames[flush_calls];
        u32 column = terminal_cursor_display_column();
        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row,
                                                  column);
        flushed_start[flush_calls] = start;
        flushed_height[flush_calls] = count;
        frame->cursor_visible = terminal.cursor_visible;
        frame->cursor_phys_row = terminal.cursor_phys_row;
        frame->cursor_col = terminal.cursor_col;
        frame->drawn_codepoint = drawn_codepoint;
        frame->drawn_x = drawn_x;
        frame->drawn_y = drawn_y;
        frame->drawn_foreground = drawn_foreground;
        frame->drawn_background = drawn_background;
        frame->cell_codepoint = cell ? cell->codepoint : 0;
        frame->cell_foreground = cell ? cell->fg_color : 0;
        frame->cell_background = cell ? cell->bg_color : 0;
    }
    flush_calls++;
}
void framebuffer_flush_rows(u32 start, u32 count)
{
    assert(start <= mock_height && count <= mock_height - start);
    memcpy(pixel_display + (usize)start * mock_width,
           pixel_backbuffer + (usize)start * mock_width,
           (usize)count * mock_width * sizeof(u32));
    record_flush(start, count);
}
void framebuffer_flush_rect(u32 x, u32 y, u32 width, u32 height)
{
    assert(x <= mock_width && y <= mock_height);
    assert(width <= mock_width - x && height <= mock_height - y);
    for (u32 row = 0; row < height; row++)
        memcpy(pixel_display + (usize)(y + row) * mock_width + x,
               pixel_backbuffer + (usize)(y + row) * mock_width + x,
               width * sizeof(u32));
    record_flush(y, height);
}
bool console_set_raw_input(bool enabled, struct kernel_thread *owner)
{
    (void)owner;
    mock_raw_input = enabled;
    if (enabled) mock_raw_input_includes_cooked = false;
    return true;
}

bool console_set_raw_input_with_cooked_bytes(bool enabled,
                                             struct kernel_thread *owner)
{
    (void)owner;
    mock_raw_input = enabled;
    mock_raw_input_includes_cooked = enabled;
    return true;
}

bool console_set_raw_input_preserving_text(bool enabled,
                                           struct kernel_thread *owner)
{
    (void)owner;
    mock_raw_input = enabled;
    if (!enabled) {
        mock_raw_input_includes_cooked = false;
        preserving_release_count++;
    }
    return true;
}

i64 console_read_key_event(u32 timeout_ms, terminal_key_event_t *event)
{
    (void)timeout_ms;
    (void)event;
    return MG_ERR_WOULD_BLOCK;
}
/* A deterministic cell glyph model makes foreground/background inversion and
 * stale overlay pixels observable. Font lookup itself is qualified elsewhere. */
static bool glyph_ink(u32 codepoint, u32 x, u32 y)
{
    return codepoint != ' ' && x > 0U && x < 7U && y > 1U && y < 14U &&
           (x == 1U || y == 2U || ((codepoint >> ((x + y) % 7U)) & 1U));
}

void draw_codepoint(u32 codepoint, u32 x, u32 y, u32 foreground,
                    u32 background)
{
    drawn_codepoint = codepoint;
    drawn_x = x;
    drawn_y = y;
    drawn_foreground = foreground;
    drawn_background = background;
    for (u32 dy = 0; dy < font_height() && y + dy < mock_height; dy++)
        for (u32 dx = 0; dx < font_width() && x + dx < mock_width; dx++)
            pixel_backbuffer[(usize)(y + dy) * mock_width + x + dx] =
                glyph_ink(codepoint, dx, dy) ? foreground : background;
}

static void initialize(u32 width, u32 height)
{
    BOOT_INFO boot = {0};

    free(pixel_backbuffer);
    free(pixel_display);
    pixel_backbuffer = calloc((usize)width * height, sizeof(u32));
    pixel_display = calloc((usize)width * height, sizeof(u32));
    assert(pixel_backbuffer && pixel_display);
    mock_width = width;
    mock_height = height;
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

static void assert_pixels_match_cells(void);

static void test_startup_cursor_hidden_until_input(void)
{
    initialize(640U, 240U);
    assert(!terminal.cursor_enabled);
    assert(!terminal.cursor_visible);

    assert(terminal_write_for_process(1U, "boot", 4U) == 4);
    assert(!terminal.cursor_enabled);
    assert(!terminal.cursor_visible);
    terminal_clear();
    assert(!terminal.cursor_enabled);
    assert(!terminal.cursor_visible);

    assert(terminal_set_cursor_visible(true));
    assert(terminal.cursor_enabled);
    assert(terminal.cursor_visible);
    assert(terminal_set_cursor_visible(false));
    assert(!terminal.cursor_enabled);
    assert(!terminal.cursor_visible);
}

static void test_terminal_clear_erases_retained_session_rows(void)
{
    char output[1200];
    for (u32 index = 0; index < sizeof(output) / 2U; index++) {
        output[index * 2U] = 'X';
        output[index * 2U + 1U] = '\n';
    }
    for (u32 visible = 0; visible < 2U; visible++) {
        initialize(116U, 92U); /* Twelve columns by four visible rows. */
        assert(terminal_set_cursor_visible(visible != 0U));
        assert(terminal_write_for_process(7U, output, sizeof(output)) ==
               (i64)sizeof(output));
        assert(terminal.top_row_idx != 0U);
        bool retained_content = false;
        for (u32 row = 0; row < TERMINAL_BUFFER_ROWS; row++) {
            terminal_cell_t *cell = terminal_cell_at(row, 0U);
            if (cell && cell->codepoint != ' ') retained_content = true;
        }
        assert(retained_content);

        terminal_clear();
        assert(terminal.top_row_idx == 0U &&
               terminal.cursor_phys_row == 0U && terminal.cursor_col == 0U);
        for (u32 row = 0; row < TERMINAL_BUFFER_ROWS; row++) {
            for (u32 column = 0; column < terminal.visible_cols; column++) {
                terminal_cell_t *cell = terminal_cell_at(row, column);
                assert(cell && cell->codepoint == ' ' &&
                       cell->bg_color == terminal.bg_color);
            }
        }
        assert_pixels_match_cells();
    }
}

static terminal_cell_t *screen_cell(u32 row, u32 column)
{
    return terminal_cell_at(screen_to_phys_row(row), column);
}

static void assert_pixels_match_cells(void)
{
    for (u32 row = 0; row < terminal.visible_rows; row++) {
        for (u32 column = 0; column < terminal.visible_cols; column++) {
            terminal_cell_t *cell = screen_cell(row, column);
            u32 foreground = terminal_color_pixel(cell->fg_color);
            u32 background = terminal_color_pixel(cell->bg_color);
            if (terminal.cursor_visible &&
                row == phys_to_screen_row(terminal.cursor_phys_row) &&
                column == terminal_cursor_display_column()) {
                u32 swap = foreground;
                foreground = background;
                background = swap;
            }
            for (u32 dy = 0; dy < font_height(); dy++) {
                for (u32 dx = 0; dx < font_width(); dx++) {
                    u32 x = TERMINAL_MARGIN_X + column * font_width() + dx;
                    u32 y = TERMINAL_MARGIN_Y + row * terminal_line_height() + dy;
                    if (x >= mock_width || y >= mock_height) continue;
                    u32 expected = glyph_ink(cell->codepoint ? cell->codepoint : ' ',
                                              dx, dy) ? foreground : background;
                    usize pixel = (usize)y * mock_width + x;
                    if (pixel_backbuffer[pixel] != expected ||
                        pixel_display[pixel] != expected) {
                        fprintf(stderr, "cursor pixel mismatch at cell %u,%u pixel %u,%u: "
                                "RAM=%x display=%x expected=%x\n", row, column,
                                dx, dy, pixel_backbuffer[pixel],
                                pixel_display[pixel], expected);
                        abort();
                    }
                }
            }
        }
    }
}

static void test_scroll_cursor_pixels(void)
{
    for (u32 initial_phase = 0; initial_phase < 3U; initial_phase++) {
        initialize(116U, 92U); /* Twelve columns, four visible rows. */
        assert(terminal_set_cursor_visible(true));
        assert(terminal_write_styled_for_process(7U, "a\nb\nc\nD", 7U,
               TERMINAL_COLOR_GREEN, TERMINAL_COLOR_BLUE) == 7);
        assert(terminal_move_cursor(3U, 0U)); /* Cursor over a colored glyph. */
        assert_pixels_match_cells();
        if (initial_phase == 1U) {
            cursor_blink_deadline_ms = mock_time + 1U;
            terminal_cursor_blink_poll();
            assert(!terminal.cursor_visible && terminal.cursor_enabled);
        } else if (initial_phase == 2U) {
            assert(terminal_set_cursor_visible(false));
        }
        assert_pixels_match_cells();

        /* A real userspace write defers its scroll to the presentation worker.
         * Run a due blink between that write and the framebuffer row copy.
         * Before the fix this leaves white pixels at row 2, column 0 even
         * though the corresponding backing-cell background is blue. */
        presentation_worker_started = true;
        for (u32 scroll = 0; scroll < 8U; scroll++) {
            assert(terminal_write_for_process(7U, "\n", 1U) == 1);
            assert(presentation_pending && terminal.pending_scroll_rows == 1U);
            assert(!terminal.batch_active && !terminal.cursor_visible);
            cursor_blink_deadline_ms = mock_time + 1U;
            terminal_cursor_blink_poll();
            terminal_force_end_batch();
            assert_pixels_match_cells();
            if (scroll == 0U) {
                assert(screen_cell(2U, 0U)->codepoint == 'D');
                assert(screen_cell(2U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
                assert(screen_cell(2U, 0U)->bg_color == TERMINAL_COLOR_BLUE);
            }
            assert(terminal_write_for_process(7U, "text", 4U) == 4);
            terminal_force_end_batch();
            assert(terminal_move_cursor(3U, 1U));
            assert_pixels_match_cells();
            assert(terminal_move_cursor(3U, 4U));
            assert_pixels_match_cells();
        }
        assert(stats.full_redraw_count == 0U);

        /* Cursor APIs within a transaction must not paint into rows whose
         * scroll transform is still pending, even after nested styled writes. */
        assert(terminal_begin_batch_for_process(7U));
        u32 flushes = flush_calls;
        assert(terminal_write_styled_for_process(7U, "\nred\nblue", 9U,
               TERMINAL_COLOR_RED, TERMINAL_COLOR_BLUE) == 9);
        assert(terminal_set_cursor_visible(true));
        assert(terminal_move_cursor(3U, 3U));
        cursor_blink_deadline_ms = mock_time + 1U;
        terminal_cursor_blink_poll();
        assert(flush_calls == flushes);
        assert(terminal_end_batch_for_process(7U));
        assert_pixels_match_cells();
        assert(stats.full_redraw_count == 0U);

        /* Pending scrolls also exclude an explicit show between writes and
         * presentation. A full-viewport scroll uses the existing full redraw. */
        assert(terminal_write_for_process(7U, "\n", 1U) == 1);
        assert(terminal_set_cursor_visible(true));
        terminal_force_end_batch();
        assert_pixels_match_cells();
        assert(terminal_write_for_process(7U, "\n\n\n\n\n", 5U) == 5);
        terminal_force_end_batch();
        assert_pixels_match_cells();
        assert(stats.full_redraw_count == 1U);

        /* The immediate kernel-output path, clear, fullscreen restoration,
         * and a login-style input transition preserve the same cell pixels. */
        for (u32 scroll = 0; scroll < 3U; scroll++) {
            terminal_put_codepoint('K');
            terminal_put_codepoint('\n');
            assert_pixels_match_cells();
        }
        assert(terminal_alternate_enter_process(42U));
        assert(terminal_write_for_process(42U, "alternate\n", 10U) == 10);
        terminal_force_end_batch();
        assert_pixels_match_cells();
        assert(terminal_alternate_leave_process(42U));
        assert_pixels_match_cells();
        terminal_clear();
        assert_pixels_match_cells();
        assert(terminal_set_cursor_visible(false));
        assert(terminal_write_for_process(7U, "Username: ", 10U) == 10);
        terminal_force_end_batch();
        assert(terminal_set_cursor_visible(true));
        assert_pixels_match_cells();
        assert(terminal_write_for_process(7U, "developer\n", 10U) == 10);
        terminal_force_end_batch();
        assert_pixels_match_cells();
    }

    /* An intermediate cursor request followed by a newline must also stay
     * transient when there is no scroll: moving logical coordinates cannot
     * leave a painted cursor at the earlier position. */
    initialize(116U, 92U);
    assert(terminal_set_cursor_visible(true));
    assert(terminal_write_styled_for_process(7U, "glyph", 5U,
           TERMINAL_COLOR_GREEN, TERMINAL_COLOR_BLUE) == 5);
    assert(terminal_begin_batch_for_process(7U));
    assert(terminal_move_cursor(0U, 1U));
    assert(terminal_write_for_process(7U, "\n", 1U) == 1);
    assert(terminal_end_batch_for_process(7U));
    assert_pixels_match_cells();
}

static void test_line_editor_scroll_pixels(void)
{
    static const char prompt[] = "developer ~: ";
    static const char diagnostic[] = "Unknown command: z\n";
    mg_line_editor_t editor;
    char buffer[64];

    initialize(340U, 128U); /* Forty columns, six visible rows. */
    assert(terminal_set_cursor_visible(true));
    assert(terminal_write_for_process(EDITOR_TEST_PID, "\n\n\n\n\n", 5U) == 5);
    line_editor_init(&editor, buffer, sizeof(buffer), prompt);
    line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                 MG_TERMINAL_COLOR_BLACK);
    assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
    assert_pixels_match_cells();
    presentation_worker_started = true;
    for (u32 command = 0; command < 8U; command++) {
        set_editor_test_input("z\n");
        assert(line_editor_read_line(&editor) == 1);
        assert(terminal_write_for_process(EDITOR_TEST_PID, diagnostic,
               sizeof(diagnostic) - 1U) == (i64)sizeof(diagnostic) - 1);
        assert(terminal.pending_scroll_rows == 2U);
        /* Real loam prompt preparation positions the cursor and requests
         * visibility inside its transaction before the pending row copy.
         * The old renderer copies two intermediate cursor blocks into the
         * previous prompt, despite the backing cells remaining correct. */
        assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
        assert_pixels_match_cells();
        assert(screen_cell(3U, 0U)->codepoint == 'd');
        assert(screen_cell(3U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
        assert(screen_cell(3U, (u32)strlen(prompt))->codepoint == 'z');
        assert(screen_cell(4U, 0U)->codepoint == 'U');
        assert(stats.full_redraw_count == 0U);
    }
}

static void test_unchanged_cells_and_narrow_flush(void)
{
    initialize(640U, 240U);
    assert(terminal_write_for_process(7U, "abc", 3U) == 3);
    terminal_reset_stats();
    assert(terminal_move_cursor(0U, 0U));
    assert(terminal_write_for_process(7U, "abc", 3U) == 3);
    assert(stats.glyph_render_count == 0U);
    assert(stats.vram_bytes_copied == 0U);
    assert_pixels_match_cells();

    assert(terminal_move_cursor(0U, 1U));
    assert(terminal_write_for_process(7U, "B", 1U) == 1);
    assert(stats.glyph_render_count == 1U);
    assert(stats.vram_bytes_copied ==
           (u64)font_width() * terminal_line_height() * sizeof(u32));
    assert_pixels_match_cells();

    terminal_reset_stats();
    assert(terminal_move_cursor(0U, 1U));
    assert(terminal_write_styled_for_process(7U, "B", 1U,
           TERMINAL_COLOR_RED, TERMINAL_COLOR_BLUE) == 1);
    assert(stats.glyph_render_count == 1U); /* Same glyph, different colors. */
    assert_pixels_match_cells();
    assert(terminal_set_cursor_visible(true));
    terminal_reset_stats();
    assert(terminal_move_cursor(0U, 5U));
    assert(stats.glyph_render_count == 2U);
    assert(stats.vram_bytes_copied ==
           4ULL * font_width() * terminal_line_height() * sizeof(u32));
    assert_pixels_match_cells();

    assert(terminal_set_cursor_visible(false));
    assert(terminal_move_cursor(1U, 0U));
    assert(terminal_write_for_process(7U, "\xe2\x82\xac", 3U) == 3);
    terminal_reset_stats();
    assert(terminal_move_cursor(1U, 0U));
    assert(terminal_write_for_process(7U, "\xe2\x82\xac", 3U) == 3);
    assert(stats.glyph_render_count == 0U && stats.vram_bytes_copied == 0U);
    assert_pixels_match_cells();
}

static void assert_frame_cursor_at(u32 frame_index, u32 offset)
{
    const presented_frame_t *frame;
    u32 row_offset = offset ? (offset - 1U) / terminal.visible_cols : 0U;
    u32 expected_phys = (terminal_input.physical_row + row_offset) %
        TERMINAL_BUFFER_ROWS;
    u32 expected_column = offset ?
        (offset - 1U) % terminal.visible_cols + 1U : 0U;
    u32 display_column = expected_column < terminal.visible_cols
        ? expected_column : terminal.visible_cols - 1U;
    u32 screen_row;

    assert(frame_index < TERMINAL_BUFFER_ROWS);
    frame = &presented_frames[frame_index];
    screen_row = phys_to_screen_row(expected_phys);
    assert(frame->cursor_visible);
    assert(frame->cursor_phys_row == expected_phys);
    assert(frame->cursor_col == expected_column);
    assert(frame->drawn_x == TERMINAL_MARGIN_X +
                             display_column * font_width());
    assert(frame->drawn_y == TERMINAL_MARGIN_Y +
                             screen_row * terminal_line_height());
    assert(frame->drawn_codepoint == frame->cell_codepoint);
    assert(frame->drawn_foreground ==
           terminal_color_pixel(frame->cell_background));
    assert(frame->drawn_background ==
           terminal_color_pixel(frame->cell_foreground));
}

static void prepare_test_editor(mg_line_editor_t *editor, char *buffer,
                                usize capacity, const char *prompt)
{
    line_editor_init(editor, buffer, capacity, prompt);
    line_editor_set_prompt_style(editor, MG_TERMINAL_COLOR_GREEN,
                                 MG_TERMINAL_COLOR_BLACK);
    assert(line_editor_prepare_next_prompt(editor) == MG_OK);
    assert(terminal.cursor_visible);
}

static void test_incremental_edit_keeps_cursor_visible(void)
{
    static const char prompt[] = "developer ~: ";
    static const char initial_input[] = "abc";
    static const char backspace_input[] = "\b";
    static const char retype_input[] = "d";
    char buffer[32];
    usize prompt_cells = strlen(prompt);

    /* Exercise both visible and hidden phases of the normal idle blink.
     * Every presentation during active editing must contain the inverse
     * cursor at the new logical position. */
    for (u32 phase = 0; phase < 2U; phase++) {
        mg_line_editor_t editor;

        initialize(640U, 240U);
        prepare_test_editor(&editor, buffer, sizeof(buffer), prompt);
        if (phase == 0U) {
            cursor_blink_deadline_ms = mock_time + 1U;
            terminal_cursor_blink_poll();
            assert(!terminal.cursor_visible);
        } else {
            assert(terminal.cursor_visible);
        }

        memset(presented_frames, 0, sizeof(presented_frames));
        flush_calls = 0;
        set_editor_test_input(initial_input);
        assert(line_editor_read_line(&editor) == MG_ERR_END_OF_FILE);
        assert(strcmp(buffer, "abc") == 0);
        assert(flush_calls == 3U);
        for (u32 character = 0; character < 3U; character++)
            assert_frame_cursor_at(character,
                                   (u32)(prompt_cells + character + 1U));
        assert(terminal.cursor_visible);

        /* Idle blinking still hides the cursor after input stops. */
        cursor_blink_deadline_ms = mock_time + 1U;
        cursor_blink_pending = false;
        terminal_cursor_blink_poll();
        assert(!terminal.cursor_visible);
        assert(!presented_frames[3].cursor_visible);
        cursor_blink_deadline_ms = mock_time + 1U;
        terminal_cursor_blink_poll();
        assert(terminal.cursor_visible);
        assert_frame_cursor_at(4U, (u32)(prompt_cells + 3U));
        cursor_blink_deadline_ms = mock_time + 1U;
        terminal_cursor_blink_poll();
        assert(!terminal.cursor_visible);

        memset(presented_frames, 0, sizeof(presented_frames));
        flush_calls = 0;
        set_editor_test_input(backspace_input);
        assert(line_editor_read_line(&editor) == MG_ERR_END_OF_FILE);
        assert(strcmp(buffer, "ab") == 0);
        assert(flush_calls == 1U);
        assert_frame_cursor_at(0U, (u32)(prompt_cells + 2U));

        memset(presented_frames, 0, sizeof(presented_frames));
        flush_calls = 0;
        set_editor_test_input(retype_input);
        assert(line_editor_read_line(&editor) == MG_ERR_END_OF_FILE);
        assert(strcmp(buffer, "abd") == 0);
        assert(flush_calls == 1U);
        assert_frame_cursor_at(0U, (u32)(prompt_cells + 3U));
    }

    /* The same cursor contract holds on both sides of a wrapped row and
     * after cursor-only movement at the boundary. */
    {
        static const char wrapped_input[] = "abcde";
        static const char move_left[] = "\x1b[D";
        mg_line_editor_t editor;

        initialize(160U, 96U);
        assert(terminal.visible_cols == 17U);
        prepare_test_editor(&editor, buffer, sizeof(buffer), prompt);
        memset(presented_frames, 0, sizeof(presented_frames));
        flush_calls = 0;
        set_editor_test_input(wrapped_input);
        assert(line_editor_read_line(&editor) == MG_ERR_END_OF_FILE);
        /* Crossing the wrap restores the old cell and paints the new one
         * with two narrow copies instead of one full-width two-row copy. */
        assert(flush_calls == sizeof(wrapped_input));
        assert_frame_cursor_at(flush_calls - 1U, 18U);
        assert_pixels_match_cells();

        memset(presented_frames, 0, sizeof(presented_frames));
        flush_calls = 0;
        set_editor_test_input(move_left);
        assert(line_editor_read_line(&editor) == MG_ERR_END_OF_FILE);
        assert(flush_calls == 2U);
        assert_frame_cursor_at(flush_calls - 1U, 17U);
        assert_pixels_match_cells();
    }
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

static void test_escape_sequences_are_literal(void)
{
    static const char clear_sequence[] = "\x1b[2J";
    static const char color_sequence[] = "\x1b[7m";

    initialize(120U, 56U);
    terminal.top_row_idx = 4U;
    terminal.cursor_phys_row = 5U;
    terminal.cursor_col = 3U;
    terminal_cell_at(4U, 0U)->codepoint = 'Q';
    assert(terminal_write_for_process(EDITOR_TEST_PID, clear_sequence,
                                      sizeof(clear_sequence) - 1U) ==
           (i64)(sizeof(clear_sequence) - 1U));
    assert(terminal.top_row_idx == 4U && terminal.cursor_phys_row == 5U);
    assert(screen_cell(1U, 3U)->codepoint == 0xfffdU);
    assert(screen_cell(1U, 4U)->codepoint == '[');
    assert(screen_cell(1U, 5U)->codepoint == '2');
    assert(screen_cell(1U, 6U)->codepoint == 'J');
    assert(terminal_cell_at(4U, 0U)->codepoint == 'Q');

    initialize(120U, 56U);
    assert(terminal_write_styled_for_process(EDITOR_TEST_PID, color_sequence,
        sizeof(color_sequence) - 1U, TERMINAL_COLOR_GREEN,
        TERMINAL_COLOR_BLACK) == (i64)(sizeof(color_sequence) - 1U));
    assert(screen_cell(0U, 0U)->codepoint == 0xfffdU);
    assert(screen_cell(0U, 1U)->codepoint == '[');
    assert(screen_cell(0U, 0U)->fg_color == TERMINAL_COLOR_GREEN);
    assert(screen_cell(0U, 1U)->fg_color == TERMINAL_COLOR_GREEN);
}

static void test_text_controls_and_mgtc_stream(void)
{
    u8 records[128];
    usize length = 0U;
    terminal_control_stream_state_t stream;
    terminal_control_stream_state_t second_stream;

    #define APPEND_RECORD(type_value, payload_value, payload_size) do { \
        const u8 *local_payload = (const u8 *)(payload_value); \
        memcpy(records + length, "MGTC", 4U); \
        records[length + 4U] = MG_TERMINAL_CONTROL_VERSION; \
        records[length + 5U] = (u8)(type_value); \
        u32 local_size = (u32)(payload_size); \
        records[length + 6U] = (u8)local_size; \
        records[length + 7U] = (u8)(local_size >> 8); \
        records[length + 8U] = (u8)(local_size >> 16); \
        records[length + 9U] = (u8)(local_size >> 24); \
        if (local_size) memcpy(records + length + 10U, local_payload, local_size); \
        length += 10U + local_size; \
    } while (0)

    initialize(116U, 92U); /* Twelve columns leave room for the whole batch. */
    terminal_control_stream_init(&stream);
    terminal_control_stream_init(&second_stream);
    const u8 green[] = {TERMINAL_COLOR_GREEN};
    const u8 mang[] = {'m', 'a', 'n', 'g'};
    const u8 yellow[] = {TERMINAL_COLOR_YELLOW};
    const u8 rove[] = {'r', 'o', 'v', 'e'};
    APPEND_RECORD(MG_TERMINAL_CONTROL_FOREGROUND, green, sizeof(green));
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, mang, sizeof(mang));
    APPEND_RECORD(MG_TERMINAL_CONTROL_FOREGROUND, yellow, sizeof(yellow));
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, rove, sizeof(rove));
    APPEND_RECORD(MG_TERMINAL_CONTROL_RESET, NULL, 0U);
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    for (u32 column = 0; column < 4U; column++) {
        assert(screen_cell(0U, column)->fg_color == TERMINAL_COLOR_GREEN);
        assert(screen_cell(0U, column)->codepoint == mang[column]);
        assert(screen_cell(0U, column + 4U)->fg_color == TERMINAL_COLOR_YELLOW);
        assert(screen_cell(0U, column + 4U)->codepoint == rove[column]);
    }
    assert(stream.foreground == TERMINAL_COLOR_LIGHT_GRAY);
    assert(second_stream.foreground == TERMINAL_COLOR_LIGHT_GRAY);

    length = 0U;
    const u8 warning[] = {TERMINAL_STYLE_WARNING};
    const u8 bg_blue[] = {TERMINAL_COLOR_BLUE};
    const u8 marked[] = {'!'};
    APPEND_RECORD(MG_TERMINAL_CONTROL_STYLE, warning, sizeof(warning));
    APPEND_RECORD(MG_TERMINAL_CONTROL_BACKGROUND, bg_blue, sizeof(bg_blue));
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, marked, sizeof(marked));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    terminal_style_t warning_style = terminal_style_for_role(
        TERMINAL_STYLE_WARNING);
    assert(screen_cell(0U, 8U)->fg_color == warning_style.foreground);
    assert(screen_cell(0U, 8U)->bg_color == TERMINAL_COLOR_BLUE);
    assert(screen_cell(0U, 8U)->codepoint == '!');

    length = 0U;
    const u8 continued[] = {'?'};
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, continued, sizeof(continued));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    assert(screen_cell(0U, 9U)->codepoint == '?');
    assert(screen_cell(0U, 9U)->fg_color == warning_style.foreground);
    assert(screen_cell(0U, 9U)->bg_color == TERMINAL_COLOR_BLUE);

    /* A style-only batch is stream-local state and must not erase a live
     * completion overlay or move the terminal cursor. */
    terminal_overlay.active = true;
    terminal_overlay.owner_pid = EDITOR_TEST_PID;
    u32 cursor_before_style = terminal.cursor_col;
    length = 0U;
    const u8 foreground_only[] = {(u8)warning_style.foreground};
    APPEND_RECORD(MG_TERMINAL_CONTROL_FOREGROUND, foreground_only,
                  sizeof(foreground_only));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    assert(terminal_overlay.active);
    assert(terminal.cursor_col == cursor_before_style);
    terminal_overlay.active = false;

    /* A later bad record invalidates the whole batch before the leading text
     * can change cells or cursor state. */
    length = 0U;
    const u8 prefix[] = {'X'};
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, prefix, sizeof(prefix));
    records[length] = 'M'; records[length + 1U] = 'G';
    records[length + 2U] = 'T'; records[length + 3U] = 'C';
    records[length + 4U] = MG_TERMINAL_CONTROL_VERSION;
    records[length + 5U] = MG_TERMINAL_CONTROL_FOREGROUND;
    records[length + 6U] = 1U; records[length + 7U] = 0U;
    records[length + 8U] = 0U; records[length + 9U] = 0U;
    records[length + 10U] = TERMINAL_COLOR_COUNT;
    length += 11U;
    u32 old_cursor = terminal.cursor_col;
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == MG_ERR_BAD_ARGUMENT);
    assert(terminal.cursor_col == old_cursor);
    assert(screen_cell(0U, 10U)->codepoint == ' ');
    assert(stream.foreground == warning_style.foreground);
    assert(stream.background == TERMINAL_COLOR_BLUE);

    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, prefix, sizeof(prefix));
    records[length] = 'M'; records[length + 1U] = 'G';
    records[length + 2U] = 'T'; records[length + 3U] = 'C';
    records[length + 4U] = MG_TERMINAL_CONTROL_VERSION + 1U;
    records[length + 5U] = MG_TERMINAL_CONTROL_TEXT;
    records[length + 6U] = 1U; records[length + 7U] = 0U;
    records[length + 8U] = 0U; records[length + 9U] = 0U;
    records[length + 10U] = 'Y';
    length += 11U;
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == MG_ERR_BAD_ARGUMENT);
    assert(screen_cell(0U, 10U)->codepoint == ' ');

    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, prefix, sizeof(prefix));
    records[length] = 'M'; records[length + 1U] = 'G';
    records[length + 2U] = 'T'; records[length + 3U] = 'C';
    records[length + 4U] = MG_TERMINAL_CONTROL_VERSION;
    records[length + 5U] = MG_TERMINAL_CONTROL_TEXT;
    records[length + 6U] = 2U; records[length + 7U] = 0U;
    records[length + 8U] = 0U; records[length + 9U] = 0U;
    records[length + 10U] = 'Y';
    length += 11U; /* Declares two payload bytes but supplies one. */
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == MG_ERR_BAD_ARGUMENT);
    assert(screen_cell(0U, 10U)->codepoint == ' ');

    length = 0U;
    const u8 cursor_bad[] = {99U, 0U, 0U, 0U, 99U, 0U, 0U, 0U};
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, prefix, sizeof(prefix));
    APPEND_RECORD(MG_TERMINAL_CONTROL_CURSOR_POSITION, cursor_bad,
                  sizeof(cursor_bad));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == MG_ERR_BAD_ARGUMENT);
    assert(screen_cell(0U, 10U)->codepoint == ' ');

    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, prefix, sizeof(prefix));
    assert(terminal_alternate_enter_process(9U));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == MG_ERR_ACCESS_DENIED);
    assert(screen_cell(0U, 10U)->codepoint == ' ');
    assert(terminal_alternate_leave_process(9U));

    /* The prior stream's style is not global terminal state: a fresh handle
     * still emits with its own default colors. */
    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, marked, sizeof(marked));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID,
        &second_stream, records, length) == (i64)length);
    assert(screen_cell(0U, 10U)->fg_color == TERMINAL_COLOR_LIGHT_GRAY);

    /* Cursor and clear records use the same production locked operations as
     * the structured terminal API. Verify their effects on cells and cursor
     * state, not only their record encoding. */
    length = 0U;
    const u8 origin[] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    const u8 middle[] = {0U, 0U, 0U, 0U, 2U, 0U, 0U, 0U};
    const u8 row_one_column_one[] = {1U, 0U, 0U, 0U, 1U, 0U, 0U, 0U};
    const u8 hide[] = {0U};
    const u8 show[] = {1U};
    const u8 clear_to_end[] = {MG_TERMINAL_CONTROL_CLEAR_TO_END};
    const u8 clear_line[] = {MG_TERMINAL_CONTROL_CLEAR_LINE};
    const u8 clear_screen[] = {MG_TERMINAL_CONTROL_CLEAR_SCREEN};
    const u8 sample[] = {'A', 'B', 'C', 'D', 'E'};
    const u8 marker[] = {'Z'};
    APPEND_RECORD(MG_TERMINAL_CONTROL_CURSOR_POSITION, origin,
                  sizeof(origin));
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, sample, sizeof(sample));
    APPEND_RECORD(MG_TERMINAL_CONTROL_CURSOR_POSITION, middle,
                  sizeof(middle));
    APPEND_RECORD(MG_TERMINAL_CONTROL_CLEAR, clear_to_end,
                  sizeof(clear_to_end));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    assert(screen_cell(0U, 0U)->codepoint == 'A');
    assert(screen_cell(0U, 1U)->codepoint == 'B');
    for (u32 column = 2U; column < terminal.visible_cols; column++)
        assert(screen_cell(0U, column)->codepoint == ' ');
    assert(terminal.cursor_col == 2U);

    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_CLEAR, clear_line,
                  sizeof(clear_line));
    APPEND_RECORD(MG_TERMINAL_CONTROL_CURSOR_POSITION, row_one_column_one,
                  sizeof(row_one_column_one));
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, marker, sizeof(marker));
    APPEND_RECORD(MG_TERMINAL_CONTROL_CURSOR_VISIBILITY, hide, sizeof(hide));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    for (u32 column = 0U; column < terminal.visible_cols; column++)
        assert(screen_cell(0U, column)->codepoint == ' ');
    assert(screen_cell(1U, 1U)->codepoint == 'Z');
    assert(!terminal.cursor_enabled);

    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_CURSOR_VISIBILITY, show, sizeof(show));
    APPEND_RECORD(MG_TERMINAL_CONTROL_CLEAR, clear_screen,
                  sizeof(clear_screen));
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == (i64)length);
    assert(terminal.top_row_idx == 0U && terminal.cursor_col == 0U);
    assert(terminal.cursor_enabled);
    for (u32 row = 0U; row < terminal.visible_rows; row++)
        for (u32 column = 0U; column < terminal.visible_cols; column++)
            assert(screen_cell(row, column)->codepoint == ' ');

    length = 0U;
    APPEND_RECORD(MG_TERMINAL_CONTROL_TEXT, prefix, sizeof(prefix));
    records[length] = 'M'; records[length + 1U] = 'G';
    records[length + 2U] = 'T'; records[length + 3U] = 'C';
    records[length + 4U] = MG_TERMINAL_CONTROL_VERSION;
    records[length + 5U] = 0xffU; /* Unknown future record type. */
    records[length + 6U] = 0U; records[length + 7U] = 0U;
    records[length + 8U] = 0U; records[length + 9U] = 0U;
    length += 10U;
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        records, length) == MG_ERR_UNSUPPORTED);
    assert(terminal.cursor_col == 0U);
    assert(screen_cell(0U, 0U)->codepoint == ' ');

    u8 *oversized_batch = malloc(TERMINAL_CONTROL_MAX_BATCH + 1U);
    assert(oversized_batch);
    memset(oversized_batch, 0, TERMINAL_CONTROL_MAX_BATCH + 1U);
    assert(terminal_control_stream_write_for_process(EDITOR_TEST_PID, &stream,
        oversized_batch, TERMINAL_CONTROL_MAX_BATCH + 1U) ==
        MG_ERR_BUFFER_TOO_SMALL);
    assert(terminal.cursor_col == 0U);
    assert(screen_cell(0U, 0U)->codepoint == ' ');
    free(oversized_batch);

    #undef APPEND_RECORD

    initialize(100U, 56U);
    write_codepoint('A');
    write_codepoint('\r');
    assert(terminal.cursor_col == 0U);
    write_codepoint('X');
    write_codepoint('\b');
    assert(terminal.cursor_col == 0U);
    write_codepoint('\n');
    assert(phys_to_screen_row(terminal.cursor_phys_row) == 1U);
}

typedef struct {
    terminal_control_stream_state_t *stream;
    u64 process_id;
    u8 foreground;
    u8 character;
    i64 result;
} concurrent_control_writer_t;

static volatile u32 concurrent_writers_ready;
static volatile u32 concurrent_writers_go;

static void control_record_header(u8 *output, u8 type, u32 length)
{
    memcpy(output, "MGTC", 4U);
    output[4] = MG_TERMINAL_CONTROL_VERSION;
    output[5] = type;
    output[6] = (u8)length;
    output[7] = (u8)(length >> 8);
    output[8] = (u8)(length >> 16);
    output[9] = (u8)(length >> 24);
}

static void *concurrent_control_writer(void *argument)
{
    concurrent_control_writer_t *writer =
        (concurrent_control_writer_t *)argument;
    u8 records[28];

    control_record_header(records, MG_TERMINAL_CONTROL_FOREGROUND, 1U);
    records[10] = writer->foreground;
    control_record_header(records + 11U, MG_TERMINAL_CONTROL_TEXT, 7U);
    memset(records + 21U, writer->character, 7U);
    __atomic_add_fetch(&concurrent_writers_ready, 1U, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&concurrent_writers_go, __ATOMIC_ACQUIRE)) { }
    writer->result = terminal_control_stream_write_for_process(
        writer->process_id, writer->stream, records, sizeof(records));
    return NULL;
}

static void test_concurrent_mgtc_process_streams(void)
{
    terminal_control_stream_state_t first_stream;
    terminal_control_stream_state_t second_stream;
    concurrent_control_writer_t writers[2];
    pthread_t threads[2];

    initialize(116U, 92U);
    terminal_control_stream_init(&first_stream);
    terminal_control_stream_init(&second_stream);
    writers[0] = (concurrent_control_writer_t){
        &first_stream, 101U, TERMINAL_COLOR_GREEN, 'G', 0
    };
    writers[1] = (concurrent_control_writer_t){
        &second_stream, 102U, TERMINAL_COLOR_RED, 'R', 0
    };
    concurrent_writers_ready = 0U;
    concurrent_writers_go = 0U;
    assert(pthread_create(&threads[0], NULL, concurrent_control_writer,
                          &writers[0]) == 0);
    assert(pthread_create(&threads[1], NULL, concurrent_control_writer,
                          &writers[1]) == 0);
    while (__atomic_load_n(&concurrent_writers_ready, __ATOMIC_ACQUIRE) != 2U) { }
    __atomic_store_n(&concurrent_writers_go, 1U, __ATOMIC_RELEASE);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(writers[0].result == 28 && writers[1].result == 28);
    assert(first_stream.foreground == TERMINAL_COLOR_GREEN);
    assert(second_stream.foreground == TERMINAL_COLOR_RED);

    u8 first_character = (u8)screen_cell(0U, 0U)->codepoint;
    assert(first_character == 'G' || first_character == 'R');
    for (u32 index = 0U; index < 14U; index++) {
        u32 row = index / terminal.visible_cols;
        u32 column = index % terminal.visible_cols;
        terminal_cell_t *cell = screen_cell(row, column);
        bool first_group = index < 7U;
        u8 character = first_group ? first_character :
            (first_character == 'G' ? 'R' : 'G');
        u8 color = character == 'G' ? TERMINAL_COLOR_GREEN :
                   TERMINAL_COLOR_RED;
        assert(cell->codepoint == character);
        assert(cell->fg_color == color);
    }
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

static void test_normal_screen_key_event_ownership(void)
{
    terminal_key_event_t event;

    initialize(160U, 80U);
    mock_raw_input = false;
    mock_raw_input_includes_cooked = false;
    preserving_release_count = 0;
    assert(terminal_set_raw_input_for_process(42U, true) == MG_OK);
    assert(terminal.key_event_owner_pid == 42U && mock_raw_input);
    assert(mock_raw_input_includes_cooked);
    assert(terminal_read_key_event_for_process(43U, 0, &event) ==
           MG_ERR_ACCESS_DENIED);
    assert(terminal_set_raw_input_for_process(43U, true) == MG_ERR_BUSY);
    assert(terminal_set_raw_input_for_process(42U, false) == MG_OK);
    assert(terminal.key_event_owner_pid == 0U && !mock_raw_input);
    assert(preserving_release_count == 1U);

    assert(terminal_set_raw_input_for_process(42U, true) == MG_OK);
    assert(terminal_alternate_leave_process(42U));
    assert(terminal.key_event_owner_pid == 0U && !mock_raw_input);

    assert(terminal_alternate_enter_process(99U));
    assert(terminal_set_raw_input_for_process(42U, true) ==
           MG_ERR_ACCESS_DENIED);
    assert(terminal_set_raw_input_for_process(99U, true) == MG_OK);
    assert(mock_raw_input && !mock_raw_input_includes_cooked);
    assert(terminal_alternate_leave_process(99U));
    assert(!mock_raw_input);
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
    /* The same production fixture measured 123 glyph renders before the
     * incremental redraw implementation. Only the final cursor is painted. */
    printf("line editor append: prior=123 current=%llu (%u%% fewer), "
           "flushes=%u bytes=%llu\n",
           (unsigned long long)append_glyphs,
           (unsigned)((123U - append_glyphs) * 100U / 123U),
           append_flushes,
           (unsigned long long)(after.copied_bytes - before.copied_bytes));
    assert(append_glyphs == 18U);
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
        assert(after_insert.glyphs - before_insert.glyphs == 4U);
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
        assert(after_delete.glyphs - before_delete.glyphs == 3U);
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
        assert(after_draft.glyphs - before_draft.glyphs == 18U);
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
    if (run_all || !strcmp(arguments[1], "startup-cursor"))
        test_startup_cursor_hidden_until_input();
    if (run_all || !strcmp(arguments[1], "clear-history"))
        test_terminal_clear_erases_retained_session_rows();
    if (run_all || !strcmp(arguments[1], "ring"))
        test_ring_wrap_and_scrolling();
    if (run_all || !strcmp(arguments[1], "scroll-pixels"))
        test_scroll_cursor_pixels();
    if (run_all || !strcmp(arguments[1], "editor-scroll-pixels"))
        test_line_editor_scroll_pixels();
    if (run_all || !strcmp(arguments[1], "narrow-flush"))
        test_unchanged_cells_and_narrow_flush();
    if (run_all || !strcmp(arguments[1], "tab"))
        test_tab_stops_and_deferred_wrap();
    if (run_all || !strcmp(arguments[1], "cursor"))
        test_wrap_cursor_stays_in_last_cell();
    if (run_all || !strcmp(arguments[1], "literal-escape"))
        test_escape_sequences_are_literal();
    if (run_all || !strcmp(arguments[1], "mgtc"))
        test_text_controls_and_mgtc_stream();
    if (run_all || !strcmp(arguments[1], "mgtc-concurrent"))
        test_concurrent_mgtc_process_streams();
    if (run_all || !strcmp(arguments[1], "utf8"))
        test_utf8_across_write_boundaries();
    if (run_all || !strcmp(arguments[1], "alternate"))
        test_alternate_screen_restores_normal_ring();
    if (run_all || !strcmp(arguments[1], "input-owner"))
        test_normal_screen_key_event_ownership();
    if (run_all || !strcmp(arguments[1], "dirty"))
        test_disjoint_dirty_rows_do_not_copy_clean_rows();
    if (run_all || !strcmp(arguments[1], "input-redraw"))
        test_input_redraw_is_presented_as_one_transaction();
    if (run_all || !strcmp(arguments[1], "editor-cursor"))
        test_incremental_edit_keeps_cursor_visible();
    if (run_all || !strcmp(arguments[1], "editor-incremental"))
        test_production_line_editor_incremental_rendering();
    free(pixel_backbuffer);
    free(pixel_display);
    puts("terminal renderer: cursor/scroll pixels, session-history clearing, startup cursor ownership, geometry, wrapped input, literal escape text, MGTC batches, UTF-8, alternate screen and incremental line editing passed");
    return 0;
}
