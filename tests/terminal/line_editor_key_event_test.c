/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise Shoot's production line editor through native key events. */
#include <assert.h>
#include <mg/line_editor.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <mg/utf8.h>
#include <string.h>

#define EVENT_CAPACITY 512U
#define OUTPUT_CAPACITY 4096U

static mg_terminal_key_event_t events[EVENT_CAPACITY];
static usize event_count;
static usize event_offset;
static char output[OUTPUT_CAPACITY];
static usize output_length;
static usize output_cursor;
static u32 last_input_offset;
static u32 last_input_extent;
static u32 cleared_cursor_offset;
static u32 cleared_cursor_extent;
static u32 clear_count;
static u32 raw_enable_count;
static u32 raw_disable_count;
static bool raw_enabled;
static u32 update_depth;

static void reset_events(void)
{
    event_count = 0;
    event_offset = 0;
}

static void push_event(u32 keycode, u32 modifiers, u32 codepoint)
{
    assert(event_count < EVENT_CAPACITY);
    events[event_count++] = (mg_terminal_key_event_t) {
        .version = MG_TERMINAL_API_VERSION,
        .keycode = keycode,
        .modifiers = modifiers,
        .codepoint = codepoint,
        .flags = 0,
    };
}

static void push_cooked_byte(u8 byte)
{
    push_event(0, 0, byte);
    events[event_count - 1U].flags = MG_TERMINAL_KEY_EVENT_COOKED_BYTE;
}

static void push_text(const char *text)
{
    for (const u8 *byte = (const u8 *)text; *byte; byte++) {
        u32 codepoint = *byte;
        if (codepoint < 0x80U) {
            push_event(0, 0, codepoint);
        } else {
            u32 value;
            usize consumed = 0;
            assert(mg_utf8_decode_one(byte, strlen((const char *)byte),
                                      &value, &consumed) == MG_UTF8_OK);
            push_event(0, 0, value);
            byte += consumed - 1U;
        }
    }
}

static void reset_output(void)
{
    memset(output, 0, sizeof(output));
    output_length = 0;
    output_cursor = 0;
    last_input_offset = 0;
    last_input_extent = 0;
    cleared_cursor_offset = 0;
    cleared_cursor_extent = 0;
    clear_count = 0;
    update_depth = 0;
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    (void)handle;
    (void)buffer;
    (void)length;
    return MG_ERR_UNSUPPORTED;
}

mg_result_t object_write_all(mg_handle_t handle, const void *buffer,
                             usize length)
{
    assert(handle == MG_STDOUT_HANDLE);
    assert(output_cursor + length < sizeof(output));
    memcpy(output + output_cursor, buffer, length);
    output_cursor += length;
    if (output_cursor > output_length) output_length = output_cursor;
    output[output_length] = '\0';
    return (mg_result_t)length;
}

mg_result_t terminal_input_raw(bool enabled)
{
    assert(enabled != raw_enabled);
    raw_enabled = enabled;
    if (enabled) raw_enable_count++;
    else raw_disable_count++;
    return MG_OK;
}

mg_result_t terminal_read_key_event(u32 timeout_ms,
                                    mg_terminal_key_event_t *out_event)
{
    assert(timeout_ms == MG_TERMINAL_WAIT_FOREVER);
    assert(raw_enabled && out_event);
    if (event_offset == event_count) return MG_ERR_WOULD_BLOCK;
    *out_event = events[event_offset++];
    return MG_OK;
}

mg_result_t terminal_prepare_input_line(void) { return MG_OK; }

mg_result_t terminal_input_cursor(u32 offset, u32 extent)
{
    output_cursor = offset;
    last_input_offset = offset;
    last_input_extent = extent;
    return MG_OK;
}

mg_result_t terminal_cursor_set_visible(bool visible)
{
    (void)visible;
    return MG_OK;
}

mg_result_t terminal_update_begin(void)
{
    update_depth++;
    return MG_OK;
}

mg_result_t terminal_update_end(void)
{
    assert(update_depth > 0);
    update_depth--;
    if (update_depth == 0 && clear_count) {
        cleared_cursor_offset = last_input_offset;
        cleared_cursor_extent = last_input_extent;
    }
    return MG_OK;
}

mg_result_t terminal_clear_screen(void)
{
    clear_count++;
    output_cursor = 0;
    output_length = 0;
    output[0] = '\0';
    return MG_OK;
}

mg_result_t terminal_write_styled(const void *buffer, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background)
{
    (void)foreground;
    (void)background;
    return object_write_all(MG_STDOUT_HANDLE, buffer, length);
}

mg_result_t terminal_overlay_clear(void) { return MG_OK; }
mg_result_t terminal_clear_line(void) { return MG_OK; }

#include "../../libc/src/line_editor.c"

static void init_editor(mg_line_editor_t *editor, char *buffer, usize capacity,
                        const char *prompt)
{
    line_editor_init(editor, buffer, capacity, prompt);
    line_editor_set_key_event_input(editor, true);
}

static void test_character_keys_and_nonmodal_editing(void)
{
    char buffer[64];
    mg_line_editor_t editor;

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    push_text("abc");
    push_event(MG_TERMINAL_KEY_LEFT, MG_TERMINAL_MODIFIER_LEFT_SHIFT, 0);
    push_event(0, 0, 'x');
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    push_event(MG_TERMINAL_KEY_DELETE, 0, 0);
    push_event(MG_TERMINAL_KEY_END, 0, 0);
    push_event(MG_TERMINAL_KEY_BACKSPACE, 0, '\b');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');

    assert(line_editor_read_line(&editor) == 2);
    assert(!strcmp(buffer, "bx"));
    assert(raw_enable_count == 1 && raw_disable_count == 1 && !raw_enabled);
    assert(update_depth == 0);
    for (editor_action_t retired = (editor_action_t)7;
         retired <= (editor_action_t)12; retired++)
        assert(!line_editor_apply_action(&editor, retired));
}

static void test_word_motion_and_deletion(void)
{
    char buffer[64];
    mg_line_editor_t editor;

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("one/foo.bar z");
    push_event(MG_TERMINAL_KEY_LEFT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_LEFT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_RIGHT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 13);
    assert(!strcmp(buffer, "one/foo.bar z"));
    assert(editor.cursor == 11U);

    /* Starting strictly inside a word skips it and lands on a different
     * word. Punctuation separates words like whitespace. */
    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("one/foo.bar z");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    for (u32 index = 0; index < 5U; index++)
        push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_LEFT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 13);
    assert(editor.cursor == 0U);

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("one/foo.bar z");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    for (u32 index = 0; index < 5U; index++)
        push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_RIGHT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 13);
    assert(editor.cursor == 11U);

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("one/foo.bar z");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    for (u32 index = 0; index < 5U; index++)
        push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_BACKSPACE, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 8);
    assert(!strcmp(buffer, "oo.bar z"));
    assert(editor.cursor == 0U);

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("one/foo.bar z");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    for (u32 index = 0; index < 5U; index++)
        push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_DELETE, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 7);
    assert(!strcmp(buffer, "one/f z"));
    assert(editor.cursor == 5U);

    /* A separator-only command has no word destination, so movement and
     * deletion stop at the corresponding command boundary. */
    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("...");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_LEFT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 3);
    assert(editor.cursor == 0U && !strcmp(buffer, "..."));

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("...");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_RIGHT, MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 3);
    assert(editor.cursor == 3U && !strcmp(buffer, "..."));
}

static mg_result_t complete_command(mg_line_editor_t *editor, void *context)
{
    const char *text = context;
    usize length = strlen(text);
    assert(length < editor->capacity);
    memcpy(editor->buffer, text, length + 1U);
    editor->length = length;
    editor->cursor = length;
    return MG_OK;
}

static void test_history_completion_cancel_eof_and_clear(void)
{
    char buffer[64];
    char history_storage[4U * 32U];
    char history_draft[64];
    mg_line_editor_t editor;
    mg_line_history_t history;

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    line_editor_history_init(&history, history_storage, 32U, 4U);
    line_editor_history_set_automatic_recording(&history, false);
    line_editor_history_set_draft(&history, history_draft,
                                  sizeof(history_draft));
    line_editor_set_history(&editor, &history);
    assert(line_editor_history_add_text(&history, "older command"));
    push_text("draft");
    push_event(MG_TERMINAL_KEY_UP, 0, 0);
    push_event(MG_TERMINAL_KEY_DOWN, 0, 0);
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 5);
    assert(!strcmp(buffer, "draft"));

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    line_editor_set_completion(&editor, complete_command, "built-in");
    push_text("b");
    push_event(MG_TERMINAL_KEY_TAB, 0, '\t');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 8);
    assert(!strcmp(buffer, "built-in"));

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    push_text("partial");
    push_event(0x06U, MG_TERMINAL_MODIFIER_LEFT_CTRL, 'c');
    assert(line_editor_read_line(&editor) == MG_ERR_CANCELLED);
    assert(buffer[0] == '\0' && editor.length == 0 && !editor.prompt_drawn);
    assert(strstr(output, "^C\n") != NULL);

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    push_event(0x07U, MG_TERMINAL_MODIFIER_LEFT_CTRL, 'd');
    assert(line_editor_read_line(&editor) == MG_ERR_END_OF_FILE);

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    push_text("kept");
    push_event(0x07U, MG_TERMINAL_MODIFIER_LEFT_CTRL, 'd');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 4);
    assert(!strcmp(buffer, "kept"));

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    push_text("keep");
    push_event(MG_TERMINAL_KEY_LEFT, 0, 0);
    push_event(0x0fU, MG_TERMINAL_MODIFIER_LEFT_CTRL, 'l');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 4);
    assert(!strcmp(buffer, "keep"));
    assert(clear_count == 1U);
    assert(strstr(output, "s> keep") != NULL);
    assert(cleared_cursor_offset == 6U && cleared_cursor_extent == 7U);
}

static void test_ctrl_d_nonempty_and_modifier_suppression(void)
{
    char buffer[64];
    mg_line_editor_t editor;

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("abcd");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    push_event(0x07U, MG_TERMINAL_MODIFIER_LEFT_CTRL, 'd');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 3);
    assert(!strcmp(buffer, "bcd"));

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_event(0x06U, MG_TERMINAL_MODIFIER_LEFT_CTRL |
                       MG_TERMINAL_MODIFIER_LEFT_ALT, 'c');
    push_event(0, MG_TERMINAL_MODIFIER_LEFT_ALT, 'q');
    push_event(0, 0, 'r');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 1);
    assert(!strcmp(buffer, "r"));

    /* These shortcuts bind the exact Ctrl chord. Adding Shift does not
     * cancel the line, trigger EOF, or clear the terminal. */
    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("kept");
    push_event(0x06U, MG_TERMINAL_MODIFIER_LEFT_CTRL |
                       MG_TERMINAL_MODIFIER_LEFT_SHIFT, 'C');
    push_event(0x0fU, MG_TERMINAL_MODIFIER_LEFT_CTRL |
                       MG_TERMINAL_MODIFIER_LEFT_SHIFT, 'L');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 4);
    assert(!strcmp(buffer, "kept") && clear_count == 0);
}

static void test_obsolete_alt_aliases_are_ignored(void)
{
    static const u32 aliases[] = {0x0bU, 0x0fU, 0x05U, 0x1aU, 0x04U, 0x08U};
    char buffer[32];

    for (usize index = 0; index < sizeof(aliases) / sizeof(aliases[0]);
         index++) {
        mg_line_editor_t editor;
        reset_events();
        reset_output();
        init_editor(&editor, buffer, sizeof(buffer), "");
        push_text("abcd");
        push_event(MG_TERMINAL_KEY_HOME, 0, 0);
        push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
        push_event(aliases[index], MG_TERMINAL_MODIFIER_LEFT_ALT, 0);
        push_event(0, 0, 'x');
        push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
        assert(line_editor_read_line(&editor) == 5);
        assert(!strcmp(buffer, "axbcd"));
    }

    /* Shift+Alt home-row navigation aliases used to create line selection;
     * the normal shell editor now treats both combinations as unbound. */
    for (usize index = 0; index < 2U; index++) {
        mg_line_editor_t editor;
        reset_events();
        reset_output();
        init_editor(&editor, buffer, sizeof(buffer), "");
        push_text("abcd");
        push_event(MG_TERMINAL_KEY_HOME, 0, 0);
        push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
        push_event(index == 0 ? 0x0bU : 0x0fU,
                   MG_TERMINAL_MODIFIER_LEFT_ALT |
                       MG_TERMINAL_MODIFIER_LEFT_SHIFT, 0);
        push_event(0, 0, 'x');
        push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
        assert(line_editor_read_line(&editor) == 5);
        assert(!strcmp(buffer, "axbcd"));
    }

    reset_events();
    reset_output();
    mg_line_editor_t editor;
    init_editor(&editor, buffer, sizeof(buffer), "");
    push_text("abcd");
    push_event(MG_TERMINAL_KEY_HOME, 0, 0);
    push_event(MG_TERMINAL_KEY_RIGHT, 0, 0);
    push_event(MG_TERMINAL_KEY_LEFT,
               MG_TERMINAL_MODIFIER_LEFT_ALT |
                   MG_TERMINAL_MODIFIER_LEFT_SHIFT,
               0);
    push_event(0, 0, 'x');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 5);
    assert(!strcmp(buffer, "axbcd"));
}

static void test_utf8_and_wrapped_extent(void)
{
    char buffer[160];
    mg_line_editor_t editor;

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "> ");
    push_text("\xc3\xa9");
    push_event(MG_TERMINAL_KEY_BACKSPACE, 0, '\b');
    push_text("\xc3\xa9");
    for (u32 index = 0; index < 95U; index++) push_event(0, 0, 'x');
    push_event(MG_TERMINAL_KEY_LEFT, 0, 0);
    push_event(0, 0, 'z');
    push_event(MG_TERMINAL_KEY_ENTER, 0, '\n');
    assert(line_editor_read_line(&editor) == 98);
    assert((u8)buffer[0] == 0xc3U && (u8)buffer[1] == 0xa9U);
    assert(buffer[96] == 'z');
    assert(last_input_extent == 99U);
    assert(update_depth == 0 && !raw_enabled);
}

static void test_queued_cooked_text_precedes_key_events(void)
{
    char buffer[32];
    mg_line_editor_t editor;

    reset_events();
    reset_output();
    init_editor(&editor, buffer, sizeof(buffer), "s> ");
    push_cooked_byte('t');
    push_cooked_byte('y');
    push_cooked_byte('p');
    push_cooked_byte('e');
    push_cooked_byte('d');
    push_cooked_byte('\n');
    assert(line_editor_read_line(&editor) == 5);
    assert(!strcmp(buffer, "typed"));
}

int main(void)
{
    test_character_keys_and_nonmodal_editing();
    test_word_motion_and_deletion();
    test_history_completion_cancel_eof_and_clear();
    test_ctrl_d_nonempty_and_modifier_suppression();
    test_obsolete_alt_aliases_are_ignored();
    test_utf8_and_wrapped_extent();
    test_queued_cooked_text_precedes_key_events();
    return 0;
}
