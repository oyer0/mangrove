/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Verify that a line-editor redraw is presented as one terminal update. */
#include <assert.h>
#include <mg/line_editor.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <string.h>

#define TEST_SCREEN_CAPACITY 128U

static char screen[TEST_SCREEN_CAPACITY];
static bool green_cells[TEST_SCREEN_CAPACITY];
static usize cursor_offset;
static usize screen_length;
static usize input_offset;
static unsigned update_depth;
static unsigned presentations;
static bool cursor_enabled = true;
static bool cursor_visible;
static bool green_prompt_cursor_presented_at_start;
static unsigned presentations_after_character;
static bool prompt_cursor_flash_after_character;
static const char input[] = { 'x', '\n' };

static void present(void)
{
    presentations++;
    if (cursor_visible && cursor_offset == 0 && screen_length &&
        screen[0] == 'd' && green_cells[0])
        green_prompt_cursor_presented_at_start = true;
}

static void mock_write(const char *bytes, usize length, bool green)
{
    for (usize index = 0; index < length; index++) {
        if (cursor_offset >= TEST_SCREEN_CAPACITY) break;
        screen[cursor_offset] = bytes[index];
        green_cells[cursor_offset] = green;
        cursor_offset++;
        if (cursor_offset > screen_length) screen_length = cursor_offset;
    }
}

static void mock_update_begin(void)
{
    if (update_depth++ == 0) cursor_visible = false;
}

static void mock_update_end(void)
{
    assert(update_depth > 0);
    if (--update_depth == 0) {
        cursor_visible = cursor_enabled;
        present();
    }
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    (void)handle;
    assert(buffer && length == 1U);
    if (input_offset == sizeof(input)) return MG_ERR_END_OF_FILE;
    if (input_offset == 1U) {
        presentations_after_character = presentations;
        prompt_cursor_flash_after_character =
            green_prompt_cursor_presented_at_start;
    }
    *(char *)buffer = input[input_offset++];
    return 1;
}

mg_result_t object_write_all(mg_handle_t handle, const void *buffer,
                             usize length)
{
    (void)handle;
    if (!length) return MG_OK;
    if (length == 1U && *(const char *)buffer == '\n') return MG_OK;
    mock_update_begin();
    mock_write(buffer, length, false);
    mock_update_end();
    return (mg_result_t)length;
}

mg_result_t terminal_prepare_input_line(void)
{
    cursor_offset = screen_length;
    if (cursor_offset) {
        screen[cursor_offset] = '\0';
        cursor_offset++;
    }
    return MG_OK;
}

mg_result_t terminal_input_cursor(u32 offset, u32 extent)
{
    (void)extent;
    cursor_offset = offset;
    cursor_visible = cursor_enabled;
    if (!update_depth) present();
    return MG_OK;
}

mg_result_t terminal_cursor_set_visible(bool visible)
{
    cursor_enabled = visible;
    cursor_visible = visible;
    if (!update_depth) present();
    return MG_OK;
}

mg_result_t terminal_update_begin(void)
{
    mock_update_begin();
    return MG_OK;
}

mg_result_t terminal_update_end(void)
{
    mock_update_end();
    return MG_OK;
}

mg_result_t terminal_write_styled(const void *buffer, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background)
{
    bool own_update = update_depth == 0;
    (void)background;
    if (own_update) mock_update_begin();
    mock_write(buffer, length, foreground == MG_TERMINAL_COLOR_GREEN);
    if (own_update) mock_update_end();
    return (mg_result_t)length;
}

mg_result_t terminal_overlay_clear(void)
{
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

mg_result_t terminal_clear_screen(void)
{
    return MG_OK;
}

#include "../../libc/src/line_editor.c"

int main(void)
{
    char line[32];
    mg_line_editor_t editor;
    static const char prompt[] = "developer ~: ";

    line_editor_init(&editor, line, sizeof(line), prompt);
    line_editor_set_prompt_style(&editor, MG_TERMINAL_COLOR_GREEN,
                                 MG_TERMINAL_COLOR_BLACK);
    assert(line_editor_prepare_next_prompt(&editor) == MG_OK);
    assert(!strncmp(screen, prompt, sizeof(prompt) - 1U));

    presentations = 0;
    green_prompt_cursor_presented_at_start = false;
    presentations_after_character = 0;
    prompt_cursor_flash_after_character = false;
    assert(line_editor_read_line(&editor) == 1);
    assert(!strcmp(line, "x"));
    assert(!prompt_cursor_flash_after_character);
    assert(presentations_after_character == 1U);
    assert(cursor_offset == sizeof(prompt));
    assert(screen[sizeof(prompt) - 1U] == 'x');
    assert(update_depth == 0);
    return 0;
}
