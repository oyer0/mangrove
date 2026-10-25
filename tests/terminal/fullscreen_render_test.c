/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fullscreen production application -> native terminal -> pixel fixture. */
#define main terminal_fixture_main
#define terminal_write_styled fixture_write_styled
#include "terminal_render_test.c"
#undef main
#undef terminal_write_styled

static usize application_bytes;
static usize application_writes;
static int fail_write_after = -1;

mg_result_t terminal_write_styled(const void *data, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background)
{
    if (fail_write_after == 0) {
        fail_write_after = -1;
        return MG_ERR_IO;
    }
    if (fail_write_after > 0) fail_write_after--;
    application_bytes += length;
    application_writes++;
    return fixture_write_styled(data, length, foreground, background);
}

mg_result_t terminal_cursor_move(u32 row, u32 column)
{
    return terminal_move_cursor(row, column) ? MG_OK : MG_ERR_BAD_ARGUMENT;
}

mg_result_t terminal_get_size(mg_terminal_size_t *size)
{
    *size = (mg_terminal_size_t){MG_TERMINAL_API_VERSION,
                               terminal.visible_rows, terminal.visible_cols};
    return MG_OK;
}

#if !defined(TEST_REED_RENDER)
#ifndef PAGER_RENDER_SOURCE
#define PAGER_RENDER_SOURCE "../../userspace/common/text_pager.c"
#endif
#include PAGER_RENDER_SOURCE

static void reset_application_work(void)
{
    application_bytes = application_writes = 0;
    terminal_reset_stats();
    flush_calls = 0;
}

static void assert_pager_pixels(text_pager_runtime_t *runtime)
{
    for (u32 row = 0; row < runtime->screen_rows; row++)
        for (u32 col = 0; col < runtime->screen_columns; col++) {
            text_pager_cell_t *expected = runtime->frame +
                (usize)row * runtime->screen_columns + col;
            terminal_cell_t *actual = screen_cell(row, col);
            assert(actual->codepoint == expected->codepoint);
            assert(actual->fg_color == expected->foreground);
            assert(actual->bg_color == expected->background);
        }
    assert_pixels_match_cells();
}

static void pager_present(text_pager_runtime_t *runtime)
{
    assert(pager_build_frame(runtime));
    assert(pager_flush_frame(runtime));
    assert_pager_pixels(runtime);
}

static void pager_fixture_start(text_pager_runtime_t *runtime,
                                 const char *text, u32 columns, u32 rows)
{
    memset(runtime, 0, sizeof(*runtime));
    initialize(20U + columns * 8U, 20U + rows * 18U);
    assert(terminal_alternate_enter_process(EDITOR_TEST_PID));
    assert(terminal_set_cursor_visible(false));
    assert(text_pager_document_init(&runtime->document, text, strlen(text),
                                    columns, 4U) == MG_OK);
    assert(pager_resize(runtime, rows, columns));
    runtime->title = "Fixture";
}

static void pager_fixture_end(text_pager_runtime_t *runtime)
{
    free(runtime->frame);
    free(runtime->previous_frame);
    text_pager_document_destroy(&runtime->document);
    assert(terminal_alternate_leave_process(EDITOR_TEST_PID));
    assert_pixels_match_cells();
}

static void test_pager_work(void)
{
    text_pager_runtime_t runtime;
    char *text = malloc(160000U);
    usize length = 0;
    assert(text);
    for (u32 line = 0; line < 2000U; line++) {
        int used = snprintf(text + length, 160000U - length,
            "line %04u the quick brown fox and UTF-8 \xc3\xa9\tmore text here\n", line);
        assert(used > 0 && (usize)used < 160000U - length);
        length += (usize)used;
    }
    pager_fixture_start(&runtime, text, 157U, 43U);
    pager_set_top(&runtime, 1000U);
    pager_present(&runtime);
    reset_application_work();
    pager_present(&runtime);
    assert(application_bytes == 0 && application_writes == 0);
    assert(stats.glyph_render_count == 0 && stats.vram_bytes_copied == 0);
    pager_set_top(&runtime, 1001U);
    reset_application_work();
    pager_present(&runtime);
    printf("pager one-row scroll: bytes=%zu writes=%zu glyphs=%llu copied=%llu\n",
           application_bytes, application_writes,
           (unsigned long long)stats.glyph_render_count,
           (unsigned long long)stats.vram_bytes_copied);
    assert(application_bytes < 200U); /* Old whole-row output exceeded 6 KiB. */
    assert(stats.glyph_render_count < 200U);
    assert(stats.vram_bytes_copied < 200000U);
    assert(stats.full_redraw_count == 0);
    for (const char *p = "jjjkkj"; *p; p++) {
        mg_terminal_key_event_t event = {.codepoint = (u32)*p};
        assert(!pager_handle_event(&runtime, &event));
        pager_present(&runtime);
    }
    assert(runtime.top_row == 1003U);
    pager_fixture_end(&runtime);
    free(text);
}

static void test_pager_layout_search_and_failure(void)
{
    static const char *const texts[] = {
        "", "\n", "short\n\nlast\n", "ab\tcd\xc3\xa9\nabcdefghijklmnop\nab\tcd",
        "ab aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"
    };
    static const u32 columns[] = {1U, 4U, 17U};
    static const u32 keys[] = {MG_TERMINAL_KEY_DOWN, MG_TERMINAL_KEY_DOWN,
        MG_TERMINAL_KEY_UP, MG_TERMINAL_KEY_PAGE_DOWN, MG_TERMINAL_KEY_PAGE_UP,
        MG_TERMINAL_KEY_END, MG_TERMINAL_KEY_HOME};
    for (usize t = 0; t < sizeof(texts) / sizeof(texts[0]); t++)
        for (usize c = 0; c < sizeof(columns) / sizeof(columns[0]); c++) {
            text_pager_runtime_t runtime;
            pager_fixture_start(&runtime, texts[t], columns[c], 4U);
            pager_present(&runtime);
            for (usize k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
                mg_terminal_key_event_t event = {.keycode = keys[k]};
                assert(!pager_handle_event(&runtime, &event));
                pager_present(&runtime);
            }
            pager_begin_search(&runtime);
            for (const char *p = "ab"; *p; p++) {
                mg_terminal_key_event_t event = {.codepoint = (u32)*p};
                assert(!pager_handle_event(&runtime, &event));
                pager_present(&runtime);
            }
            for (usize k = 0; k < 4U; k++) {
                mg_terminal_key_event_t event = {
                    .keycode = k == 0 ? MG_TERMINAL_KEY_ENTER :
                               k == 3 ? MG_TERMINAL_KEY_ESCAPE : 0U,
                    .codepoint = k == 1 ? 'n' : k == 2 ? 'b' : 0U};
                bool closing = pager_handle_event(&runtime, &event);
                assert(!closing || k == 3U); /* Escape also closes a no-match view. */
                pager_present(&runtime);
            }
            pager_fixture_end(&runtime);
        }
    {
        text_pager_runtime_t runtime;
        pager_fixture_start(&runtime, "old\nother\nlast", 17U, 4U);
        pager_present(&runtime);
        runtime.top_row = 1U;
        assert(pager_build_frame(&runtime));
        fail_write_after = 1;
        assert(!pager_flush_frame(&runtime));
        assert(!terminal.batch_active && batch_depth == 0U);
        assert(pager_flush_frame(&runtime)); /* Complete retry repairs partial output. */
        assert_pager_pixels(&runtime);
        pager_fixture_end(&runtime);
    }
}

#else

#include "../../userspace/reed/buffer.h"
static usize layout_decodes, layout_bounds;
static bool counted_codepoint(const reed_buffer_t *b, usize p, u32 *cp, usize *next)
{
    layout_decodes++;
    return reed_buffer_codepoint(b, p, cp, next);
}
static bool counted_bounds(const reed_buffer_t *b, usize line, usize *start, usize *end)
{
    layout_bounds++;
    return reed_buffer_line_bounds(b, line, start, end);
}
#define reed_buffer_codepoint counted_codepoint
#define reed_buffer_line_bounds counted_bounds
#ifndef REED_RENDER_SOURCE
#define REED_RENDER_SOURCE "../../userspace/reed/editor.c"
#endif
#include REED_RENDER_SOURCE
#undef reed_buffer_codepoint
#undef reed_buffer_line_bounds

static void reed_fixture_start(reed_editor_t *editor, const char *text,
                                u32 columns, u32 rows)
{
    initialize(20U + columns * 8U, 20U + rows * 18U);
    assert(terminal_alternate_enter_process(EDITOR_TEST_PID));
    memset(editor, 0, sizeof(*editor));
    assert(reed_buffer_init(&editor->buffer));
    assert(reed_buffer_replace(&editor->buffer, text, strlen(text)));
    reed_config_defaults(&editor->config);
    editor->mode = REED_MODE_NORMAL;
    editor->screen_rows = rows;
    editor->screen_columns = columns;
    editor->frame = calloc(REED_MAX_SCREEN_ROWS * REED_MAX_SCREEN_COLUMNS,
                            sizeof(*editor->frame));
    editor->presented_frame = calloc(REED_MAX_SCREEN_ROWS * REED_MAX_SCREEN_COLUMNS,
                                      sizeof(*editor->presented_frame));
    assert(editor->frame && editor->presented_frame);
    set_cursor(editor, 0U);
}
static void reed_present(reed_editor_t *editor)
{
    assert(editor_render(editor));
    for (u32 row = 0; row < editor->frame_rows; row++)
        for (u32 col = 0; col < editor->frame_columns; col++) {
            reed_frame_cell_t *expected = editor->presented_frame +
                (usize)row * editor->frame_columns + col;
            terminal_cell_t *actual = screen_cell(row, col);
            assert(actual->codepoint == expected->codepoint);
            assert(actual->fg_color == expected->foreground);
            assert(actual->bg_color == expected->background);
        }
    assert_pixels_match_cells();
}
static void reset_reed_work(void)
{
    layout_decodes = layout_bounds = application_bytes = application_writes = 0;
    terminal_reset_stats();
    flush_calls = 0;
}
static void reed_fixture_end(reed_editor_t *editor)
{
    editor_destroy_frames(editor);
    reed_buffer_destroy(&editor->buffer);
    assert(terminal_alternate_leave_process(EDITOR_TEST_PID));
    assert_pixels_match_cells();
}
static void test_reed_work(void)
{
    reed_editor_t editor;
    char *text = malloc(160000U);
    usize length = 0, start, end;
    assert(text);
    for (u32 line = 0; line < 2000U; line++) {
        int used = snprintf(text + length, 160000U - length,
            "line %04u the quick brown fox and UTF-8 \xc3\xa9\tmore text here\n", line);
        assert(used > 0 && (usize)used < 160000U - length);
        length += (usize)used;
    }
    reed_fixture_start(&editor, text, 157U, 43U);
    assert(reed_buffer_line_bounds(&editor.buffer, 1041U, &start, &end));
    set_cursor(&editor, start);
    editor.top_visual_row = 1000U;
    reset_reed_work();
    reed_present(&editor);
    assert(layout_bounds <= 4U); /* Old rendering repeated 46 prefix scans. */
    assert(layout_decodes < 150000U);
    reset_reed_work();
    reed_present(&editor);
    assert(application_bytes == 0 && stats.glyph_render_count == 2U);
    assert(layout_bounds <= 4U && layout_decodes < 150000U);
    move_vertical(&editor, true);
    reset_reed_work();
    reed_present(&editor);
    assert(editor.top_visual_row == 1001U);
    printf("Reed one-row scroll: decodes=%zu bounds=%zu bytes=%zu glyphs=%llu copied=%llu\n",
           layout_decodes, layout_bounds, application_bytes,
           (unsigned long long)stats.glyph_render_count,
           (unsigned long long)stats.vram_bytes_copied);
    assert(application_bytes < 200U && stats.glyph_render_count < 202U);
    assert(stats.vram_bytes_copied < 200000U && stats.full_redraw_count == 0);
    for (const char *p = "jjjkkj"; *p; p++) {
        move_vertical(&editor, *p == 'j');
        reed_present(&editor);
    }
    assert(reed_buffer_line_at(&editor.buffer, reed_buffer_cursor(&editor.buffer)) == 1044U);
    usize insertion = reed_buffer_cursor(&editor.buffer);
    usize before_edit = reed_buffer_length(&editor.buffer);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = 0x0cU}); /* i */
    dispatch_insert(&editor, &(mg_terminal_key_event_t){.codepoint = 'X'});
    assert(reed_buffer_length(&editor.buffer) == before_edit + 1U);
    u32 cp; usize next;
    assert(reed_buffer_codepoint(&editor.buffer, insertion, &cp, &next) && cp == 'X');
    dispatch_insert(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_ESCAPE});
    reed_present(&editor);
    start_search(&editor);
    assert(input_insert_bytes(&editor, "line", 4U));
    reed_present(&editor);
    accept_search(&editor);
    dispatch_search(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_N});
    reed_present(&editor);
    dispatch_search(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_B});
    dispatch_search(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_ESCAPE});
    reed_present(&editor);
    reed_fixture_end(&editor);
    free(text);
}
static void test_reed_narrow_and_long_layout(void)
{
    static const u32 columns[] = {1U, 4U, 17U};
    const char *text = "ab\tcd\xc3\xa9 abcdefghijklmnopqrstuvwxyz\n\nend\n";
    for (usize c = 0; c < sizeof(columns) / sizeof(columns[0]); c++)
        for (unsigned wrap = 0; wrap < 2U; wrap++) {
            reed_editor_t editor;
            reed_fixture_start(&editor, text, columns[c], 5U);
            editor.config.wrap = wrap;
            reed_present(&editor);
            for (unsigned i = 0; i < 8U; i++) {
                move_vertical(&editor, true);
                reed_present(&editor);
            }
            action_page(&editor, false);
            reed_present(&editor);
            action_page(&editor, true);
            reed_present(&editor);
            editor.mode = REED_MODE_SELECTION;
            editor.selection = REED_SELECTION_CHARACTER;
            editor.selection_anchor = 0U;
            reed_present(&editor);
            set_cursor(&editor, reed_buffer_length(&editor.buffer));
            usize eof = reed_buffer_cursor(&editor.buffer);
            move_vertical(&editor, true);
            assert(reed_buffer_cursor(&editor.buffer) == eof);
            reed_present(&editor);
            reed_fixture_end(&editor);
        }
    {
        reed_editor_t editor;
        char *long_line = malloc(100001U);
        assert(long_line);
        memset(long_line, 'a', 100000U);long_line[100000U] = '\0';
        reed_fixture_start(&editor, long_line, 17U, 5U);
        reset_reed_work();reed_present(&editor);
        /* Locating the line scans its extent once. Each visual row stops
         * decoding at its right edge instead of repainting the entire line. */
        assert(layout_decodes < 101000U);
        reed_fixture_end(&editor);free(long_line);
    }
}
#endif

int main(void)
{
#if defined(TEST_REED_RENDER)
    test_reed_work();
    test_reed_narrow_and_long_layout();
#else
    test_pager_work();
    test_pager_layout_search_and_failure();
#endif
    free(pixel_backbuffer);
    free(pixel_display);
    puts("fullscreen production rendering: work bounds, UTF-8, wrapping, search, errors and pixels passed");
    return 0;
}
