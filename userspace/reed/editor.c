/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "editor.h"
#include "buffer.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "../common/path.h"

#define REED_INPUT_CAPACITY       512U
#define REED_STATUS_CAPACITY      512U
#define REED_MAX_SCREEN_COLUMNS  256U
#define REED_MAX_SCREEN_ROWS      128U

#define HID_KEY_A                 0x04U
#define HID_KEY_B                 0x05U
#define HID_KEY_D                 0x07U
#define HID_KEY_E                 0x08U
#define HID_KEY_H                 0x0BU
#define HID_KEY_I                 0x0CU
#define HID_KEY_J                 0x0DU
#define HID_KEY_K                 0x0EU
#define HID_KEY_L                 0x0FU
#define HID_KEY_N                 0x11U
#define HID_KEY_O                 0x12U
#define HID_KEY_Q                 0x14U
#define HID_KEY_S                 0x16U
#define HID_KEY_U                 0x18U
#define HID_KEY_W                 0x1AU
#define HID_KEY_X                 0x1BU
#define HID_KEY_SEMICOLON         0x33U
#define HID_KEY_SLASH             0x38U
#define HID_KEY_ESCAPE            0x29U
#define HID_KEY_ENTER             0x28U
#define HID_KEY_BACKSPACE         0x2AU
#define HID_KEY_TAB               0x2BU
#define HID_KEY_DELETE            0x4CU
#define HID_KEY_HOME              0x4AU
#define HID_KEY_END               0x4DU
#define HID_KEY_PAGE_UP           0x4BU
#define HID_KEY_PAGE_DOWN         0x4EU
#define HID_KEY_RIGHT             0x4FU
#define HID_KEY_LEFT              0x50U
#define HID_KEY_DOWN              0x51U
#define HID_KEY_UP                0x52U

typedef enum {
    REED_MODE_NORMAL,
    REED_MODE_INSERT,
    REED_MODE_COMMAND,
    REED_MODE_SEARCH,
} reed_mode_t;

typedef enum {
    REED_PROMPT_NONE,
    REED_PROMPT_COMMAND,
    REED_PROMPT_SEARCH_FORWARD,
    REED_PROMPT_SEARCH_BACKWARD,
    REED_PROMPT_CONFIRM,
    REED_PROMPT_SAVE_PATH,
} reed_prompt_t;

typedef enum {
    REED_ACTION_NONE,
    REED_ACTION_MOVE_LEFT,
    REED_ACTION_MOVE_DOWN,
    REED_ACTION_MOVE_UP,
    REED_ACTION_MOVE_RIGHT,
    REED_ACTION_LINE_START,
    REED_ACTION_LINE_END,
    REED_ACTION_LINE_START_INSERT,
    REED_ACTION_LINE_END_INSERT,
    REED_ACTION_WORD_END,
    REED_ACTION_WORD_START,
    REED_ACTION_PREVIOUS_WORD_START,
    REED_ACTION_PREVIOUS_WORD_END,
    REED_ACTION_INSERT,
    REED_ACTION_OPEN_BELOW,
    REED_ACTION_OPEN_ABOVE,
    REED_ACTION_DELETE_CHAR,
    REED_ACTION_DELETE_OPERATOR,
    REED_ACTION_UNDO,
    REED_ACTION_REDO,
    REED_ACTION_SAVE,
    REED_ACTION_QUIT,
    REED_ACTION_COMMAND,
    REED_ACTION_SEARCH_FORWARD,
    REED_ACTION_SEARCH_BACKWARD,
    REED_ACTION_REPEAT_SEARCH,
    REED_ACTION_REPEAT_SEARCH_REVERSE,
    REED_ACTION_PAGE_UP,
    REED_ACTION_PAGE_DOWN,
} reed_action_t;

typedef struct {
    u32 keycode;
    bool shift;
    bool alt;
    bool control;
    reed_action_t action;
} reed_key_binding_t;

typedef struct {
    u32 codepoint;
    u8 foreground;
    u8 background;
    u16 reserved;
} reed_frame_cell_t;

typedef struct reed_editor {
    reed_buffer_t buffer;
    mg_handle_t file_handle;
    bool existing_file;
    bool save_uncertain;
    reed_mode_t mode;
    reed_prompt_t prompt;
    bool running;
    bool alternate_entered;
    bool raw_input;
    bool insert_transaction;
    bool insert_recorded;
    reed_action_t pending_action;
    usize desired_column;
    usize top_line;
    usize horizontal_column;
    u32 screen_rows;
    u32 screen_columns;
    char input[REED_INPUT_CAPACITY];
    usize input_length;
    usize input_cursor;
    char status[REED_STATUS_CAPACITY];
    char search_text[REED_INPUT_CAPACITY];
    usize search_length;
    usize search_match;
    bool search_active;
    bool search_forward;
    reed_frame_cell_t *frame;
    reed_frame_cell_t *presented_frame;
    u32 frame_rows;
    u32 frame_columns;
    bool frame_valid;
} reed_editor_t;

static bool editor_render(reed_editor_t *editor);
static char lower_ascii(char value);

static const reed_key_binding_t normal_keymap[] = {
    { HID_KEY_H, false, false, false, REED_ACTION_MOVE_LEFT },
    { HID_KEY_J, false, false, false, REED_ACTION_MOVE_DOWN },
    { HID_KEY_K, false, false, false, REED_ACTION_MOVE_UP },
    { HID_KEY_L, false, false, false, REED_ACTION_MOVE_RIGHT },
    { HID_KEY_A, false, false, false, REED_ACTION_LINE_START },
    { HID_KEY_E, false, false, false, REED_ACTION_LINE_END },
    { HID_KEY_A, false, true, false, REED_ACTION_LINE_START_INSERT },
    { HID_KEY_E, false, true, false, REED_ACTION_LINE_END_INSERT },
    { HID_KEY_W, false, false, false, REED_ACTION_WORD_END },
    { HID_KEY_W, false, true, false, REED_ACTION_WORD_START },
    { HID_KEY_B, false, false, false, REED_ACTION_PREVIOUS_WORD_START },
    { HID_KEY_B, false, true, false, REED_ACTION_PREVIOUS_WORD_END },
    { HID_KEY_I, false, false, false, REED_ACTION_INSERT },
    { HID_KEY_O, false, false, false, REED_ACTION_OPEN_BELOW },
    { HID_KEY_O, false, true, false, REED_ACTION_OPEN_ABOVE },
    { HID_KEY_X, false, false, false, REED_ACTION_DELETE_CHAR },
    { HID_KEY_D, false, false, false, REED_ACTION_DELETE_OPERATOR },
    { HID_KEY_U, false, false, false, REED_ACTION_UNDO },
    { HID_KEY_U, false, true, false, REED_ACTION_REDO },
    { HID_KEY_S, false, true, false, REED_ACTION_SAVE },
    { HID_KEY_Q, false, true, false, REED_ACTION_QUIT },
    { HID_KEY_SLASH, false, false, false, REED_ACTION_COMMAND },
    { HID_KEY_SEMICOLON, false, false, false, REED_ACTION_SEARCH_FORWARD },
    { HID_KEY_SEMICOLON, true, false, false, REED_ACTION_SEARCH_BACKWARD },
    { HID_KEY_N, false, false, false, REED_ACTION_REPEAT_SEARCH },
    { HID_KEY_N, false, true, false, REED_ACTION_REPEAT_SEARCH_REVERSE },
    { HID_KEY_PAGE_UP, false, false, false, REED_ACTION_PAGE_UP },
    { HID_KEY_PAGE_DOWN, false, false, false, REED_ACTION_PAGE_DOWN },
};

static bool has_shift(u32 modifiers)
{
    return (modifiers & (MG_TERMINAL_MODIFIER_LEFT_SHIFT |
                         MG_TERMINAL_MODIFIER_RIGHT_SHIFT)) != 0;
}

static bool has_alt(u32 modifiers)
{
    return (modifiers & (MG_TERMINAL_MODIFIER_LEFT_ALT |
                         MG_TERMINAL_MODIFIER_RIGHT_ALT)) != 0;
}

static bool has_control(u32 modifiers)
{
    return (modifiers & (MG_TERMINAL_MODIFIER_LEFT_CTRL |
                         MG_TERMINAL_MODIFIER_RIGHT_CTRL)) != 0;
}

static bool key_is_plain(const mg_terminal_key_event_t *event)
{
    return event && !has_shift(event->modifiers) && !has_alt(event->modifiers) &&
           !has_control(event->modifiers) &&
           !(event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_GUI |
                                 MG_TERMINAL_MODIFIER_RIGHT_GUI));
}

static reed_action_t normal_action(const mg_terminal_key_event_t *event)
{
    bool shift;
    bool alt;
    bool control;

    if (!event || (event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_GUI |
                                       MG_TERMINAL_MODIFIER_RIGHT_GUI)))
        return REED_ACTION_NONE;
    shift = has_shift(event->modifiers);
    alt = has_alt(event->modifiers);
    control = has_control(event->modifiers);
    for (usize index = 0; index < sizeof(normal_keymap) /
                              sizeof(normal_keymap[0]); index++) {
        const reed_key_binding_t *binding = &normal_keymap[index];
        if (binding->keycode == event->keycode &&
            binding->shift == shift && binding->alt == alt &&
            binding->control == control)
            return binding->action;
    }
    return REED_ACTION_NONE;
}

static void set_status(reed_editor_t *editor, const char *text)
{
    usize length;

    if (!editor || !text) return;
    length = strlen(text);
    if (length >= sizeof(editor->status)) length = sizeof(editor->status) - 1U;
    memcpy(editor->status, text, length);
    editor->status[length] = '\0';
}

static void editor_invalidate_frame(reed_editor_t *editor)
{
    if (editor) editor->frame_valid = false;
}

static void editor_destroy_frames(reed_editor_t *editor)
{
    if (!editor) return;
    free(editor->frame);
    free(editor->presented_frame);
    editor->frame = NULL;
    editor->presented_frame = NULL;
    editor->frame_rows = 0;
    editor->frame_columns = 0;
    editor->frame_valid = false;
}

static void reset_input(reed_editor_t *editor)
{
    if (!editor) return;
    editor->input_length = 0;
    editor->input_cursor = 0;
    editor->input[0] = '\0';
}

static bool input_insert_bytes(reed_editor_t *editor, const char *bytes,
                               usize length)
{
    if (!editor || !bytes || !length ||
        length > sizeof(editor->input) - 1U - editor->input_length)
        return false;
    memmove(editor->input + editor->input_cursor + length,
            editor->input + editor->input_cursor,
            editor->input_length - editor->input_cursor + 1U);
    memcpy(editor->input + editor->input_cursor, bytes, length);
    editor->input_length += length;
    editor->input_cursor += length;
    return true;
}

static usize input_next_boundary(const reed_editor_t *editor, usize position)
{
    u8 first;
    usize count = 1;

    if (!editor || position >= editor->input_length) return editor
        ? editor->input_length : 0;
    first = (u8)editor->input[position];
    if (first >= 0xC2U && first <= 0xDFU) count = 2;
    else if (first >= 0xE0U && first <= 0xEFU) count = 3;
    else if (first >= 0xF0U && first <= 0xF4U) count = 4;
    if (count > editor->input_length - position) return position + 1U;
    for (usize index = 1; index < count; index++)
        if (((u8)editor->input[position + index] & 0xC0U) != 0x80U)
            return position + 1U;
    return position + count;
}

static usize input_previous_boundary(const reed_editor_t *editor,
                                     usize position)
{
    if (!editor || !position || position > editor->input_length) return 0;
    position--;
    while (position && ((u8)editor->input[position] & 0xC0U) == 0x80U)
        position--;
    return position;
}

static void input_delete_left(reed_editor_t *editor)
{
    usize previous;

    if (!editor || !editor->input_cursor) return;
    previous = input_previous_boundary(editor, editor->input_cursor);
    memmove(editor->input + previous,
            editor->input + editor->input_cursor,
            editor->input_length - editor->input_cursor + 1U);
    editor->input_length -= editor->input_cursor - previous;
    editor->input_cursor = previous;
}

static void input_delete_right(reed_editor_t *editor)
{
    usize next;

    if (!editor || editor->input_cursor >= editor->input_length) return;
    next = input_next_boundary(editor, editor->input_cursor);
    memmove(editor->input + editor->input_cursor,
            editor->input + next,
            editor->input_length - next + 1U);
    editor->input_length -= next - editor->input_cursor;
}

static bool encode_codepoint(u32 codepoint, char output[4], usize *out_length)
{
    if (!output || !out_length || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) return false;
    if (codepoint < 0x80U) {
        output[0] = (char)codepoint;
        *out_length = 1;
    } else if (codepoint < 0x800U) {
        output[0] = (char)(0xC0U | (codepoint >> 6));
        output[1] = (char)(0x80U | (codepoint & 0x3FU));
        *out_length = 2;
    } else if (codepoint < 0x10000U) {
        output[0] = (char)(0xE0U | (codepoint >> 12));
        output[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[2] = (char)(0x80U | (codepoint & 0x3FU));
        *out_length = 3;
    } else {
        output[0] = (char)(0xF0U | (codepoint >> 18));
        output[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        output[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[3] = (char)(0x80U | (codepoint & 0x3FU));
        *out_length = 4;
    }
    return true;
}

static bool resolve_path(const char *input, char *output, usize capacity)
{
    char expanded[REED_MAX_PATH_BYTES];

    if (!input || !output || !command_expand_home_path(input, true,
                                                        expanded,
                                                        sizeof(expanded)))
        return false;
    return command_resolve_path(expanded, output, capacity);
}

static mg_result_t load_file_text(mg_handle_t handle, u64 known_size,
                                  char **out_text, usize *out_length,
                                  bool *out_final_newline)
{
    mg_file_lines_state_t state;
    mg_result_t result;
    char *text;
    usize capacity;
    usize length;

    if (!out_text || !out_length || !out_final_newline)
        return MG_ERR_BAD_ARGUMENT;
    if (known_size > REED_MAX_BUFFER_BYTES)
        return MG_ERR_BUFFER_TOO_SMALL;
    capacity = known_size ? (usize)known_size : 1U;
    for (;;) {
        text = (char *)malloc(capacity + 1U);
        if (!text) return MG_ERR_NO_MEMORY;
        result = file_snapshot(handle, text, capacity, &state);
        if (result != MG_ERR_BUFFER_TOO_SMALL) break;
        free(text);
        if (capacity == REED_MAX_BUFFER_BYTES)
            return MG_ERR_BUFFER_TOO_SMALL;
        capacity = capacity > REED_MAX_BUFFER_BYTES / 2U
            ? REED_MAX_BUFFER_BYTES : capacity * 2U;
    }
    if (result != MG_OK) {
        free(text);
        return result;
    }
    if (state.length > capacity) {
        free(text);
        return MG_ERR_IO;
    }
    length = state.length;
    text[length] = '\0';
    for (usize index = 0; index < length; index++) {
        if (text[index] == '\0') {
            free(text);
            return MG_ERR_BAD_ARGUMENT;
        }
    }
    if (!reed_buffer_valid_utf8(text, length)) {
        free(text);
        return MG_ERR_BAD_ARGUMENT;
    }
    *out_final_newline = length != 0 && text[length - 1U] == '\n';
    if (*out_final_newline) {
        length--;
        text[length] = '\0';
    }
    *out_text = text;
    *out_length = length;
    return MG_OK;
}

static mg_result_t save_existing_file(reed_editor_t *editor)
{
    mg_result_t result;
    char *text = NULL;
    char *saved_text = NULL;
    usize length = 0;
    usize saved_length = 0;
    usize file_length;

    if (!editor || !editor->existing_file) return MG_ERR_UNSUPPORTED;
    file_length = reed_buffer_length(&editor->buffer) +
        (reed_buffer_has_final_newline(&editor->buffer) ? 1U : 0U);
    if (file_length > MG_FILE_REPLACE_MAX_BYTES)
        return MG_ERR_REPLACEMENT_TOO_LARGE;
    if (!reed_buffer_copy_file_allocated(&editor->buffer, &text, &length) ||
        !reed_buffer_copy_allocated(&editor->buffer, &saved_text,
                                    &saved_length)) {
        free(text);
        free(saved_text);
        return MG_ERR_NO_MEMORY;
    }
    result = file_replace(editor->file_handle, text, length);
    if (result == MG_OK)
        reed_buffer_commit_saved(&editor->buffer, saved_text, saved_length);
    else
        free(saved_text);
    free(text);
    return result;
}

static mg_result_t save_created_file(reed_editor_t *editor, const char *path)
{
    char *text = NULL, *saved_text = NULL, *path_copy = NULL;
    usize length = 0, saved_length = 0, path_length, file_length;
    mg_handle_t new_handle = 0;
    mg_handle_t old_handle;
    bool old_existing;
    mg_result_t result;

    if (!editor || !path || path[0] != '/') return MG_ERR_BAD_ARGUMENT;
    path_length = strlen(path);
    if (!path_length || path_length >= REED_MAX_PATH_BYTES)
        return MG_ERR_BAD_ARGUMENT;
    file_length = reed_buffer_length(&editor->buffer) +
        (reed_buffer_has_final_newline(&editor->buffer) ? 1U : 0U);
    if (file_length > MG_FILE_REPLACE_MAX_BYTES)
        return MG_ERR_REPLACEMENT_TOO_LARGE;
    path_copy = (char *)malloc(path_length + 1U);
    if (!path_copy) return MG_ERR_NO_MEMORY;
    memcpy(path_copy, path, path_length + 1U);
    if (!reed_buffer_copy_file_allocated(&editor->buffer, &text, &length) ||
        !reed_buffer_copy_allocated(&editor->buffer, &saved_text,
                                    &saved_length)) {
        result = MG_ERR_NO_MEMORY;
        goto failure;
    }
    result = file_create_contents(path_copy, text, length, &new_handle);
    if (result != MG_OK) goto failure;
    if (!new_handle) {
        result = MG_ERR_UNCERTAIN;
        goto failure;
    }

    old_handle = editor->file_handle;
    old_existing = editor->existing_file;
    editor->file_handle = new_handle;
    editor->existing_file = true;
    reed_buffer_commit_path(&editor->buffer, path_copy, path_length);
    reed_buffer_commit_saved(&editor->buffer, saved_text, saved_length);
    free(text);
    if (old_existing) (void)handle_close(old_handle);
    return MG_OK;

failure:
    free(text);
    free(saved_text);
    free(path_copy);
    return result;
}

static mg_result_t editor_open_path(reed_editor_t *editor, const char *path)
{
    char *text = NULL;
    usize length = 0;
    bool final_newline = false;
    reed_buffer_t next = {0};
    reed_buffer_t previous;
    mg_handle_t next_handle = 0;
    bool next_existing = false;
    mg_path_info_t info;
    mg_result_t result;

    if (!editor || !path) return MG_ERR_BAD_ARGUMENT;
    result = path_info(path, &info);
    if (result == MG_OK) {
        if (info.type != MG_PATH_TYPE_FILE) return MG_ERR_UNSUPPORTED;
        result = file_open(path, MG_OPEN_RDWR);
        if (result_is_error(result)) return result;
        next_handle = (mg_handle_t)result;
        next_existing = true;
        result = load_file_text(next_handle, info.size, &text, &length,
                                &final_newline);
        if (result != MG_OK) goto failed;
    } else if (result == MG_ERR_NOT_FOUND) {
        text = (char *)malloc(1);
        if (!text) return MG_ERR_NO_MEMORY;
        text[0] = '\0';
        length = 0;
        final_newline = true;
    } else {
        return result;
    }
    if (!reed_buffer_init(&next) ||
        !reed_buffer_replace(&next, text, length) ||
        !reed_buffer_set_final_newline(&next, final_newline) ||
        !reed_buffer_set_path(&next, path) ||
        !reed_buffer_mark_saved(&next)) {
        result = MG_ERR_NO_MEMORY;
        goto failed;
    }
    free(text);
    previous = editor->buffer;
    editor->buffer = next;
    reed_buffer_destroy(&previous);
    if (editor->existing_file) (void)handle_close(editor->file_handle);
    editor->file_handle = next_handle;
    editor->existing_file = next_existing;
    editor->save_uncertain = false;
    editor->desired_column = 0;
    editor->top_line = 0;
    editor->horizontal_column = 0;
    editor->search_active = false;
    editor->search_length = 0;
    editor->pending_action = REED_ACTION_NONE;
    editor_invalidate_frame(editor);
    set_status(editor, "Opened.");
    return MG_OK;
failed:
    reed_buffer_destroy(&next);
    free(text);
    if (next_existing) (void)handle_close(next_handle);
    return result;
}

static const char *mode_name(reed_mode_t mode)
{
    switch (mode) {
        case REED_MODE_INSERT: return "INSERT";
        case REED_MODE_COMMAND: return "COMMAND";
        case REED_MODE_SEARCH: return "SEARCH";
        case REED_MODE_NORMAL:
        default: return "NORMAL";
    }
}

static bool write_style(const char *text, usize length,
                        mg_terminal_color_t foreground,
                        mg_terminal_color_t background)
{
    return terminal_write_styled(text, length, foreground, background) ==
           (mg_result_t)length;
}

static bool query_matches_at(const reed_editor_t *editor, usize position)
{
    char candidate[REED_INPUT_CAPACITY];
    usize length;

    length = editor ? reed_buffer_length(&editor->buffer) : 0;
    if (!editor || !editor->search_active || !editor->search_length ||
        editor->search_length > length ||
        position > length - editor->search_length ||
        editor->search_length >= sizeof(candidate)) return false;
    if (!reed_buffer_copy_slice(&editor->buffer, position, candidate,
                                editor->search_length)) return false;
    return memcmp(candidate, editor->search_text, editor->search_length) == 0;
}

static bool is_word_byte(u8 byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte >= 0x80U;
}

static char buffer_byte_at(const reed_buffer_t *buffer, usize position)
{
    char byte;

    if (!buffer || !reed_buffer_copy_slice(buffer, position, &byte, 1))
        return '\0';
    return byte;
}

static usize word_end_next(const reed_buffer_t *buffer, usize position)
{
    usize length = reed_buffer_length(buffer);

    while (position < length && is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_next_boundary(buffer, position);
    while (position < length &&
           !is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_next_boundary(buffer, position);
    while (position < length && is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_next_boundary(buffer, position);
    return position;
}

static usize word_start_next(const reed_buffer_t *buffer, usize position)
{
    usize length = reed_buffer_length(buffer);

    while (position < length && is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_next_boundary(buffer, position);
    while (position < length &&
           !is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_next_boundary(buffer, position);
    return position;
}

static usize previous_word_start(const reed_buffer_t *buffer, usize position)
{
    if (!buffer || !position) return 0;
    position = reed_buffer_previous_boundary(buffer, position);
    while (position && !is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_previous_boundary(buffer, position);
    while (position) {
        usize previous = reed_buffer_previous_boundary(buffer, position);
        if (!is_word_byte((u8)buffer_byte_at(buffer, previous))) break;
        position = previous;
    }
    return position;
}

static usize previous_word_end(const reed_buffer_t *buffer, usize position)
{
    usize end;

    if (!buffer || !position) return 0;
    position = reed_buffer_previous_boundary(buffer, position);
    while (position && !is_word_byte((u8)buffer_byte_at(buffer, position)))
        position = reed_buffer_previous_boundary(buffer, position);
    end = reed_buffer_next_boundary(buffer, position);
    while (end < reed_buffer_length(buffer) &&
           is_word_byte((u8)buffer_byte_at(buffer, end)))
        end = reed_buffer_next_boundary(buffer, end);
    return end;
}

static void set_cursor(reed_editor_t *editor, usize position)
{
    if (!editor) return;
    if (reed_buffer_cursor_set(&editor->buffer, position))
        editor->desired_column = reed_buffer_line_column(&editor->buffer,
                                                         position);
}

static void move_vertical(reed_editor_t *editor, bool down)
{
    usize current_line;
    usize target_line;
    usize line_count;
    usize position;

    if (!editor) return;
    current_line = reed_buffer_line_at(&editor->buffer,
                                       reed_buffer_cursor(&editor->buffer));
    line_count = reed_buffer_line_count(&editor->buffer);
    if (down) {
        if (current_line + 1U >= line_count) return;
        target_line = current_line + 1U;
    } else {
        if (!current_line) return;
        target_line = current_line - 1U;
    }
    if (reed_buffer_position_for_line_column(&editor->buffer, target_line,
                                             editor->desired_column,
                                             &position))
        (void)reed_buffer_cursor_set(&editor->buffer, position);
}

static void move_line_start(reed_editor_t *editor)
{
    usize line = reed_buffer_line_at(&editor->buffer,
                                     reed_buffer_cursor(&editor->buffer));
    usize start;
    usize end;

    if (reed_buffer_line_bounds(&editor->buffer, line, &start, &end))
        set_cursor(editor, start);
}

static void move_line_end(reed_editor_t *editor, bool insert_end)
{
    usize line = reed_buffer_line_at(&editor->buffer,
                                     reed_buffer_cursor(&editor->buffer));
    usize start;
    usize end;

    if (!reed_buffer_line_bounds(&editor->buffer, line, &start, &end)) return;
    if (insert_end || start == end) set_cursor(editor, end);
    else set_cursor(editor, reed_buffer_previous_boundary(&editor->buffer, end));
}

static bool search_find_from(reed_editor_t *editor, usize start, bool forward,
                             bool *out_wrapped)
{
    usize length;
    usize found = 0;
    bool wrapped = false;

    if (!editor || !editor->search_active || !editor->search_length) return false;
    length = reed_buffer_length(&editor->buffer);
    if (forward) {
        if (start > length) start = length;
        for (usize position = start; position <= length; position++) {
            if (query_matches_at(editor, position)) {
                found = position;
                goto found_match;
            }
        }
        for (usize position = 0; position < start; position++) {
            if (query_matches_at(editor, position)) {
                found = position;
                wrapped = true;
                goto found_match;
            }
        }
    } else {
        if (start > length) start = length;
        for (usize position = start; position > 0; position--) {
            usize candidate = position - 1U;
            if (query_matches_at(editor, candidate)) {
                found = candidate;
                goto found_match;
            }
        }
        for (usize position = length; position >= start && position != 0; position--) {
            usize candidate = position - 1U;
            if (query_matches_at(editor, candidate)) {
                found = candidate;
                wrapped = true;
                goto found_match;
            }
        }
    }
    return false;

found_match:
    editor->search_match = found;
    set_cursor(editor, found);
    if (out_wrapped) *out_wrapped = wrapped;
    return true;
}

static void repeat_search(reed_editor_t *editor, bool reverse)
{
    bool wrapped = false;
    bool forward;
    usize start;

    if (!editor || !editor->search_active || !editor->search_length) {
        set_status(editor, "No active search.");
        return;
    }
    forward = editor->search_forward != reverse;
    if (forward) {
        start = editor->search_match + editor->search_length;
    } else {
        start = editor->search_match;
    }
    if (!search_find_from(editor, start, forward, &wrapped)) {
        set_status(editor, "Pattern not found.");
        return;
    }
    set_status(editor, wrapped ? "Search wrapped." : "");
}

static bool begin_edit(reed_editor_t *editor)
{
    if (!editor) return false;
    if (editor->mode == REED_MODE_INSERT && editor->insert_recorded)
        return true;
    if (!reed_buffer_record_edit(&editor->buffer)) {
        set_status(editor, "Edit history is full.");
        return false;
    }
    if (editor->mode == REED_MODE_INSERT)
        editor->insert_recorded = true;
    return true;
}

static bool insert_codepoint(reed_editor_t *editor, u32 codepoint)
{
    char encoded[4];
    usize length;
    usize position;

    if (!editor || (codepoint < 0x20U && codepoint != '\t' &&
                    codepoint != '\n') || codepoint == 0x7FU ||
        !encode_codepoint(codepoint, encoded, &length)) return false;
    if (!begin_edit(editor)) return false;
    position = reed_buffer_cursor(&editor->buffer);
    if (!reed_buffer_insert(&editor->buffer, position, encoded, length))
        return false;
    editor->desired_column = reed_buffer_line_column(&editor->buffer,
                                                     reed_buffer_cursor(
                                                         &editor->buffer));
    return true;
}

static bool delete_left(reed_editor_t *editor)
{
    usize cursor;
    usize previous;

    if (!editor) return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    if (!cursor) return false;
    previous = reed_buffer_previous_boundary(&editor->buffer, cursor);
    if (!begin_edit(editor)) return false;
    return reed_buffer_delete(&editor->buffer, previous, cursor - previous);
}

static bool delete_right(reed_editor_t *editor)
{
    usize cursor;
    usize next;

    if (!editor) return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    next = reed_buffer_next_boundary(&editor->buffer, cursor);
    if (next == cursor) return false;
    if (!begin_edit(editor)) return false;
    return reed_buffer_delete(&editor->buffer, cursor, next - cursor);
}

static bool delete_line(reed_editor_t *editor)
{
    usize start;
    usize end;
    usize length;

    if (!editor || !reed_buffer_line_bounds(&editor->buffer,
                                             reed_buffer_line_at(
                                                 &editor->buffer,
                                                 reed_buffer_cursor(
                                                     &editor->buffer)),
                                             &start, &end)) return false;
    length = reed_buffer_length(&editor->buffer);
    if (end < length) end++;
    else if (start > 0) start--;
    if (end <= start) return false;
    if (!begin_edit(editor)) return false;
    return reed_buffer_delete(&editor->buffer, start, end - start);
}

static bool delete_forward_word(reed_editor_t *editor)
{
    usize cursor;
    usize target;

    if (!editor) return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    target = word_end_next(&editor->buffer, cursor);
    if (target <= cursor) return false;
    if (!begin_edit(editor)) return false;
    return reed_buffer_delete(&editor->buffer, cursor, target - cursor);
}

static bool delete_backward_word(reed_editor_t *editor)
{
    usize cursor;
    usize target;

    if (!editor) return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    target = previous_word_start(&editor->buffer, cursor);
    if (target >= cursor) return false;
    if (!begin_edit(editor)) return false;
    return reed_buffer_delete(&editor->buffer, target, cursor - target);
}

static void finish_insert(reed_editor_t *editor)
{
    if (!editor) return;
    editor->mode = REED_MODE_NORMAL;
    editor->insert_transaction = false;
    editor->insert_recorded = false;
    editor->desired_column = reed_buffer_line_column(&editor->buffer,
                                                     reed_buffer_cursor(
                                                         &editor->buffer));
}

static void start_insert(reed_editor_t *editor)
{
    if (!editor) return;
    editor->mode = REED_MODE_INSERT;
    editor->insert_transaction = true;
    editor->insert_recorded = false;
    editor->pending_action = REED_ACTION_NONE;
    editor->desired_column = reed_buffer_line_column(&editor->buffer,
                                                     reed_buffer_cursor(
                                                         &editor->buffer));
}

static bool insert_newline(reed_editor_t *editor, bool above)
{
    usize line = reed_buffer_line_at(&editor->buffer,
                                     reed_buffer_cursor(&editor->buffer));
    usize start;
    usize end;
    const char newline = '\n';
    usize position;

    if (!editor || !reed_buffer_line_bounds(&editor->buffer, line, &start,
                                             &end)) return false;
    position = above ? start : end;
    if (!reed_buffer_record_edit(&editor->buffer)) {
        set_status(editor, "Edit history is full.");
        return false;
    }
    if (!reed_buffer_insert(&editor->buffer, position, &newline, 1)) return false;
    if (above) (void)reed_buffer_cursor_set(&editor->buffer, position);
    start_insert(editor);
    editor->insert_recorded = true;
    return true;
}

static char prompt_choice(reed_editor_t *editor)
{
    mg_terminal_key_event_t event;

    if (!editor) return 'c';
    editor->prompt = REED_PROMPT_CONFIRM;
    reset_input(editor);
    for (;;) {
        if (!editor_render(editor)) return 'c';
        if (terminal_read_key_event(MG_TERMINAL_WAIT_FOREVER, &event) != MG_OK)
            return 'c';
        if (event.keycode == HID_KEY_ESCAPE) break;
        if (!has_alt(event.modifiers) && !has_control(event.modifiers) &&
            (event.codepoint == 's' || event.codepoint == 'S' ||
             event.codepoint == 'd' || event.codepoint == 'D' ||
             event.codepoint == 'c' || event.codepoint == 'C')) {
            char choice = lower_ascii((char)event.codepoint);
            editor->prompt = REED_PROMPT_NONE;
            reset_input(editor);
            return choice;
        }
    }
    editor->prompt = REED_PROMPT_NONE;
    reset_input(editor);
    return 'c';
}

static bool prompt_save_path(reed_editor_t *editor, char *path,
                             usize capacity)
{
    mg_terminal_key_event_t event;
    bool accepted = false;

    if (!editor || !path) return false;
    editor->prompt = REED_PROMPT_SAVE_PATH;
    reset_input(editor);
    for (;;) {
        char encoded[4];
        usize encoded_length;

        if (!editor_render(editor) ||
            terminal_read_key_event(MG_TERMINAL_WAIT_FOREVER, &event) != MG_OK)
            break;
        if (event.keycode == HID_KEY_ESCAPE) break;
        if (event.keycode == HID_KEY_ENTER || event.codepoint == '\n') {
            accepted = editor->input_length != 0 &&
                resolve_path(editor->input, path, capacity);
            break;
        }
        if (event.keycode == HID_KEY_BACKSPACE) input_delete_left(editor);
        else if (event.keycode == HID_KEY_DELETE) input_delete_right(editor);
        else if (event.keycode == HID_KEY_LEFT)
            editor->input_cursor = input_previous_boundary(editor,
                                                           editor->input_cursor);
        else if (event.keycode == HID_KEY_RIGHT)
            editor->input_cursor = input_next_boundary(editor,
                                                       editor->input_cursor);
        else if (event.keycode == HID_KEY_HOME) editor->input_cursor = 0;
        else if (event.keycode == HID_KEY_END)
            editor->input_cursor = editor->input_length;
        else if (!has_alt(event.modifiers) &&
                 !has_control(event.modifiers) && event.codepoint >= 0x20U &&
                 event.codepoint != 0x7FU &&
                 encode_codepoint(event.codepoint, encoded, &encoded_length))
            (void)input_insert_bytes(editor, encoded, encoded_length);
    }
    editor->prompt = REED_PROMPT_NONE;
    reset_input(editor);
    return accepted;
}

static char lower_ascii(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static bool text_equals_ignore_case(const char *left, const char *right,
                                    usize length)
{
    if (!left || !right || strlen(right) != length) return false;
    for (usize index = 0; index < length; index++)
        if (lower_ascii(left[index]) != lower_ascii(right[index])) return false;
    return true;
}

static const char *skip_spaces(const char *text)
{
    while (text && (*text == ' ' || *text == '\t')) text++;
    return text;
}

static bool report_save_result(reed_editor_t *editor, mg_result_t result,
                               bool created)
{
    if (result == MG_OK) {
        editor->save_uncertain = false;
        set_status(editor, created ? "Saved new file." : "Saved opened file.");
        return true;
    }
    switch (result) {
        case MG_ERR_STALE_GENERATION:
            set_status(editor, "Save refused: file changed externally; edits kept.");
            break;
        case MG_ERR_UNCERTAIN:
            editor->save_uncertain = true;
            set_status(editor, "Save outcome uncertain; edits kept. Do not retry.");
            break;
        case MG_ERR_ALREADY_EXISTS:
            set_status(editor, "Save refused: destination exists; edits kept.");
            break;
        case MG_ERR_BAD_ARGUMENT:
            set_status(editor, "Save failed: invalid pathname; edits kept.");
            break;
        case MG_ERR_ACCESS_DENIED:
            set_status(editor, "Save denied: access changed; edits kept.");
            break;
        case MG_ERR_UNSUPPORTED:
            set_status(editor, "Save unsupported on this filesystem; edits kept.");
            break;
        case MG_ERR_REPLACEMENT_TOO_LARGE:
            set_status(editor, "Save refused: file exceeds 512 KiB; edits kept.");
            break;
        case MG_ERR_DEVICE_GONE:
            set_status(editor, "Save failed: device removed; edits kept.");
            break;
        case MG_ERR_NO_SPACE:
            set_status(editor, "Save failed: filesystem has no space; edits kept.");
            break;
        case MG_ERR_NO_MEMORY:
            set_status(editor, "Save failed: out of memory; edits kept.");
            break;
        default:
            set_status(editor, "Save failed; edits kept.");
            break;
    }
    return false;
}

static bool save_as_action(reed_editor_t *editor, const char *argument)
{
    char path[REED_MAX_PATH_BYTES];

    if (!editor || !argument || !*argument ||
        !resolve_path(argument, path, sizeof(path)))
        return report_save_result(editor, MG_ERR_BAD_ARGUMENT, true);
    return report_save_result(editor, save_created_file(editor, path), true);
}

static bool save_action(reed_editor_t *editor)
{
    mg_result_t result;
    const char *path;
    char prompted_path[REED_MAX_PATH_BYTES];

    if (!editor) return false;
    if (editor->save_uncertain) {
        set_status(editor, "Save blocked after uncertain outcome; use /saveas PATH or discard.");
        return false;
    }
    if (editor->existing_file)
        return report_save_result(editor, save_existing_file(editor), false);
    path = reed_buffer_path(&editor->buffer);
    if (!path) {
        if (!prompt_save_path(editor, prompted_path, sizeof(prompted_path))) {
            set_status(editor, "Save cancelled or invalid pathname; edits kept.");
            return false;
        }
        path = prompted_path;
    }
    result = save_created_file(editor, path);
    return report_save_result(editor, result, true);
}

static void execute_save_command(reed_editor_t *editor, bool quit_after)
{
    if (save_action(editor) && quit_after) editor->running = false;
}

static bool quit_action(reed_editor_t *editor)
{
    char choice;

    if (!editor) return false;
    if (!reed_buffer_is_modified(&editor->buffer) &&
        !editor->save_uncertain) {
        editor->running = false;
        return true;
    }
    choice = prompt_choice(editor);
    if (choice == 's') {
        if (!save_action(editor)) return false;
        editor->running = false;
        return true;
    }
    if (choice == 'd') {
        editor->running = false;
        return true;
    }
    set_status(editor, "Quit cancelled.");
    return false;
}

static const char *open_error_text(mg_result_t result)
{
    switch (result) {
        case MG_ERR_BUFFER_TOO_SMALL:
            return "Open failed: file exceeds Reed's 2 MiB limit.";
        case MG_ERR_BAD_ARGUMENT:
            return "Open failed: NUL or invalid UTF-8 contents.";
        case MG_ERR_ACCESS_DENIED:
            return "Open denied: read/write access required.";
        case MG_ERR_UNSUPPORTED:
            return "Open failed: regular MGFS 1.2 file required.";
        case MG_ERR_DEVICE_GONE:
            return "Open failed: device removed.";
        case MG_ERR_NO_MEMORY:
            return "Open failed: out of memory.";
        default:
            return "Open failed; current edits kept.";
    }
}

static bool open_action(reed_editor_t *editor, const char *argument)
{
    char path[REED_MAX_PATH_BYTES];
    char choice;
    mg_result_t result;

    if (!editor || !argument || !*argument ||
        !resolve_path(argument, path, sizeof(path))) {
        set_status(editor, "Open failed: invalid pathname.");
        return false;
    }
    if (reed_buffer_is_modified(&editor->buffer) ||
        editor->save_uncertain) {
        set_status(editor, "Save changes? [s] Save [d] Don't Save [c] Cancel");
        choice = prompt_choice(editor);
        if (choice == 's') {
            if (!save_action(editor)) return false;
        } else if (choice != 'd') {
            set_status(editor, "Open cancelled.");
            return false;
        }
    }
    result = editor_open_path(editor, path);
    if (result != MG_OK) {
        set_status(editor, open_error_text(result));
        return false;
    }
    return true;
}

static void start_search(reed_editor_t *editor, bool forward)
{
    if (!editor) return;
    editor->mode = REED_MODE_SEARCH;
    editor->prompt = forward ? REED_PROMPT_SEARCH_FORWARD
                             : REED_PROMPT_SEARCH_BACKWARD;
    reset_input(editor);
    editor->search_forward = forward;
}

static void accept_search(reed_editor_t *editor)
{
    bool wrapped = false;
    usize start;

    if (!editor) return;
    if (!editor->input_length) {
        set_status(editor, "Empty search.");
        editor->mode = REED_MODE_NORMAL;
        editor->prompt = REED_PROMPT_NONE;
        return;
    }
    memcpy(editor->search_text, editor->input, editor->input_length + 1U);
    editor->search_length = editor->input_length;
    editor->search_active = true;
    start = reed_buffer_cursor(&editor->buffer);
    if (!search_find_from(editor, start, editor->search_forward, &wrapped))
        set_status(editor, "Pattern not found.");
    else
        set_status(editor, wrapped ? "Search wrapped." : "");
    editor->mode = REED_MODE_NORMAL;
    editor->prompt = REED_PROMPT_NONE;
    reset_input(editor);
}

static void execute_command_line(reed_editor_t *editor)
{
    char command[REED_INPUT_CAPACITY];
    const char *cursor;
    const char *argument;

    if (!editor) return;
    memcpy(command, editor->input, editor->input_length + 1U);
    editor->prompt = REED_PROMPT_NONE;
    editor->mode = REED_MODE_NORMAL;
    reset_input(editor);
    cursor = skip_spaces(command);
    argument = cursor;
    while (*argument && *argument != ' ' && *argument != '\t') argument++;
    {
        usize name_length = (usize)(argument - cursor);
        const char *tail = skip_spaces(argument);
        if (!name_length) {
            set_status(editor, "Empty command.");
            return;
        }
        if (text_equals_ignore_case(cursor, "s", name_length) ||
            text_equals_ignore_case(cursor, "sq", name_length)) {
            if (*tail) {
                set_status(editor, "Usage: /s, /sq");
                return;
            }
            execute_save_command(editor,
                text_equals_ignore_case(cursor, "sq", name_length));
            return;
        }
        if (text_equals_ignore_case(cursor, "saveas", name_length)) {
            if (!*tail) {
                set_status(editor, "Usage: /saveas PATH");
                return;
            }
            (void)save_as_action(editor, tail);
            return;
        }
        if (text_equals_ignore_case(cursor, "q", name_length)) {
            if (*tail) {
                set_status(editor, "Usage: /q");
                return;
            }
            set_status(editor, "Save changes? [s] Save [d] Don't Save [c] Cancel");
            (void)quit_action(editor);
            return;
        }
        if (text_equals_ignore_case(cursor, "open", name_length)) {
            if (!*tail) {
                set_status(editor, "Usage: /open PATH");
                return;
            }
            (void)open_action(editor, tail);
            return;
        }
        if (text_equals_ignore_case(cursor, "find", name_length)) {
            if (!*tail) {
                set_status(editor, "Usage: /find TEXT");
                return;
            }
            if (strlen(tail) >= sizeof(editor->search_text)) {
                set_status(editor, "Search text is too long.");
                return;
            }
            strcpy(editor->search_text, tail);
            editor->search_length = strlen(tail);
            editor->search_active = true;
            editor->search_forward = true;
            if (!search_find_from(editor,
                                  reed_buffer_cursor(&editor->buffer), true,
                                  NULL))
                set_status(editor, "Pattern not found.");
            else set_status(editor, "");
            return;
        }
    }
    set_status(editor, "Unknown Reed command.");
}

static void cancel_prompt(reed_editor_t *editor)
{
    if (!editor) return;
    editor->prompt = REED_PROMPT_NONE;
    editor->mode = REED_MODE_NORMAL;
    reset_input(editor);
    set_status(editor, "Cancelled.");
}

static void handle_prompt_event(reed_editor_t *editor,
                                const mg_terminal_key_event_t *event)
{
    if (!editor || !event) return;
    if (event->keycode == HID_KEY_ESCAPE) {
        cancel_prompt(editor);
        return;
    }
    if (event->keycode == HID_KEY_ENTER || event->codepoint == '\n') {
        if (editor->mode == REED_MODE_SEARCH) accept_search(editor);
        else execute_command_line(editor);
        return;
    }
    if (event->keycode == HID_KEY_BACKSPACE || event->codepoint == '\b') {
        input_delete_left(editor);
        return;
    }
    if (event->keycode == HID_KEY_DELETE) {
        input_delete_right(editor);
        return;
    }
    if (event->keycode == HID_KEY_LEFT) {
        editor->input_cursor = input_previous_boundary(editor,
                                                        editor->input_cursor);
        return;
    }
    if (event->keycode == HID_KEY_RIGHT) {
        editor->input_cursor = input_next_boundary(editor,
                                                   editor->input_cursor);
        return;
    }
    if (event->keycode == HID_KEY_HOME) {
        editor->input_cursor = 0;
        return;
    }
    if (event->keycode == HID_KEY_END) {
        editor->input_cursor = editor->input_length;
        return;
    }
    if (!has_alt(event->modifiers) && !has_control(event->modifiers) &&
        event->codepoint >= 0x20U && event->codepoint != 0x7FU) {
        char encoded[4];
        usize length;
        if (encode_codepoint(event->codepoint, encoded, &length))
            (void)input_insert_bytes(editor, encoded, length);
    }
}

static void action_page(reed_editor_t *editor, bool down)
{
    usize line;
    usize count;
    usize amount;
    usize target;
    usize position;

    if (!editor) return;
    line = reed_buffer_line_at(&editor->buffer,
                               reed_buffer_cursor(&editor->buffer));
    count = reed_buffer_line_count(&editor->buffer);
    amount = editor->screen_rows > 2U ? editor->screen_rows - 2U : 1U;
    if (down) {
        target = line + amount;
        if (target >= count) target = count - 1U;
    } else {
        target = line > amount ? line - amount : 0;
    }
    if (reed_buffer_position_for_line_column(&editor->buffer, target,
                                             editor->desired_column,
                                             &position))
        (void)reed_buffer_cursor_set(&editor->buffer, position);
}

static void dispatch_delete_target(reed_editor_t *editor,
                                   const mg_terminal_key_event_t *event)
{
    if (!editor || !event) return;
    editor->pending_action = REED_ACTION_NONE;
    if (!key_is_plain(event)) {
        set_status(editor, "Delete needs d, w, or b.");
        return;
    }
    switch (event->keycode) {
        case HID_KEY_D: (void)delete_line(editor); break;
        case HID_KEY_W: (void)delete_forward_word(editor); break;
        case HID_KEY_B: (void)delete_backward_word(editor); break;
        default: set_status(editor, "Delete needs d, w, or b."); break;
    }
}

static void dispatch_normal(reed_editor_t *editor,
                            const mg_terminal_key_event_t *event)
{
    reed_action_t action;
    usize cursor;

    if (!editor || !event) return;
    if (event->keycode == HID_KEY_ESCAPE) {
        editor->pending_action = REED_ACTION_NONE;
        return;
    }
    if (editor->pending_action == REED_ACTION_DELETE_OPERATOR) {
        dispatch_delete_target(editor, event);
        return;
    }
    action = normal_action(event);
    switch (action) {
        case REED_ACTION_MOVE_LEFT:
            cursor = reed_buffer_cursor(&editor->buffer);
            set_cursor(editor, reed_buffer_previous_boundary(&editor->buffer,
                                                             cursor));
            break;
        case REED_ACTION_MOVE_RIGHT:
            cursor = reed_buffer_cursor(&editor->buffer);
            set_cursor(editor, reed_buffer_next_boundary(&editor->buffer,
                                                         cursor));
            break;
        case REED_ACTION_MOVE_DOWN:
            move_vertical(editor, true);
            break;
        case REED_ACTION_MOVE_UP:
            move_vertical(editor, false);
            break;
        case REED_ACTION_LINE_START:
            move_line_start(editor);
            break;
        case REED_ACTION_LINE_END:
            move_line_end(editor, false);
            break;
        case REED_ACTION_LINE_START_INSERT:
            move_line_start(editor);
            start_insert(editor);
            break;
        case REED_ACTION_LINE_END_INSERT:
            move_line_end(editor, true);
            start_insert(editor);
            break;
        case REED_ACTION_WORD_END:
            set_cursor(editor, word_end_next(&editor->buffer,
                                             reed_buffer_cursor(&editor->buffer)));
            break;
        case REED_ACTION_WORD_START:
            set_cursor(editor, word_start_next(&editor->buffer,
                                               reed_buffer_cursor(&editor->buffer)));
            break;
        case REED_ACTION_PREVIOUS_WORD_START:
            set_cursor(editor, previous_word_start(&editor->buffer,
                                                   reed_buffer_cursor(
                                                       &editor->buffer)));
            break;
        case REED_ACTION_PREVIOUS_WORD_END:
            set_cursor(editor, previous_word_end(&editor->buffer,
                                                 reed_buffer_cursor(
                                                     &editor->buffer)));
            break;
        case REED_ACTION_INSERT:
            start_insert(editor);
            break;
        case REED_ACTION_OPEN_BELOW:
            (void)insert_newline(editor, false);
            break;
        case REED_ACTION_OPEN_ABOVE:
            (void)insert_newline(editor, true);
            break;
        case REED_ACTION_DELETE_CHAR:
            (void)delete_right(editor);
            break;
        case REED_ACTION_DELETE_OPERATOR:
            editor->pending_action = REED_ACTION_DELETE_OPERATOR;
            set_status(editor, "Delete target: d, w, or b");
            break;
        case REED_ACTION_UNDO:
            if (!reed_buffer_undo(&editor->buffer)) set_status(editor, "Nothing to undo.");
            break;
        case REED_ACTION_REDO:
            if (!reed_buffer_redo(&editor->buffer)) set_status(editor, "Nothing to redo.");
            break;
        case REED_ACTION_SAVE:
            (void)save_action(editor);
            break;
        case REED_ACTION_QUIT:
            (void)quit_action(editor);
            break;
        case REED_ACTION_COMMAND:
            editor->mode = REED_MODE_COMMAND;
            editor->prompt = REED_PROMPT_COMMAND;
            reset_input(editor);
            break;
        case REED_ACTION_SEARCH_FORWARD:
            start_search(editor, true);
            break;
        case REED_ACTION_SEARCH_BACKWARD:
            start_search(editor, false);
            break;
        case REED_ACTION_REPEAT_SEARCH:
            repeat_search(editor, false);
            break;
        case REED_ACTION_REPEAT_SEARCH_REVERSE:
            repeat_search(editor, true);
            break;
        case REED_ACTION_PAGE_UP:
            action_page(editor, false);
            break;
        case REED_ACTION_PAGE_DOWN:
            action_page(editor, true);
            break;
        case REED_ACTION_NONE:
        default:
            break;
    }
    if (action == REED_ACTION_MOVE_LEFT || action == REED_ACTION_MOVE_RIGHT)
        editor->desired_column = reed_buffer_line_column(&editor->buffer,
                                                         reed_buffer_cursor(
                                                             &editor->buffer));
    if (action == REED_ACTION_UNDO || action == REED_ACTION_REDO)
        editor->desired_column = reed_buffer_line_column(&editor->buffer,
                                                         reed_buffer_cursor(
                                                             &editor->buffer));
}

static void dispatch_insert(reed_editor_t *editor,
                            const mg_terminal_key_event_t *event)
{
    usize cursor;

    if (!editor || !event) return;
    if (event->keycode == HID_KEY_ESCAPE) {
        finish_insert(editor);
        return;
    }
    if (event->keycode == HID_KEY_LEFT) {
        cursor = reed_buffer_cursor(&editor->buffer);
        set_cursor(editor, reed_buffer_previous_boundary(&editor->buffer,
                                                         cursor));
        return;
    }
    if (event->keycode == HID_KEY_RIGHT) {
        cursor = reed_buffer_cursor(&editor->buffer);
        set_cursor(editor, reed_buffer_next_boundary(&editor->buffer, cursor));
        return;
    }
    if (event->keycode == HID_KEY_UP || event->keycode == HID_KEY_DOWN) {
        move_vertical(editor, event->keycode == HID_KEY_DOWN);
        return;
    }
    if (event->keycode == HID_KEY_HOME) {
        move_line_start(editor);
        return;
    }
    if (event->keycode == HID_KEY_END) {
        move_line_end(editor, true);
        return;
    }
    if (event->keycode == HID_KEY_BACKSPACE || event->codepoint == '\b') {
        (void)delete_left(editor);
        return;
    }
    if (event->keycode == HID_KEY_DELETE) {
        (void)delete_right(editor);
        return;
    }
    if (!has_alt(event->modifiers) && !has_control(event->modifiers) &&
        (event->codepoint == '\n' || event->codepoint == '\t' ||
         event->codepoint >= 0x20U) && event->codepoint != 0x7FU) {
        (void)insert_codepoint(editor, event->codepoint);
    }
}

static bool editor_init(reed_editor_t *editor, const char *pathname)
{
    char path[REED_MAX_PATH_BYTES];
    mg_result_t result;

    if (!editor || !reed_buffer_init(&editor->buffer)) return false;
    memset((char *)editor + sizeof(editor->buffer), 0,
           sizeof(*editor) - sizeof(editor->buffer));
    editor->mode = REED_MODE_NORMAL;
    editor->running = true;
    if (pathname) {
        if (!resolve_path(pathname, path, sizeof(path))) {
            set_status(editor, "Open failed: invalid pathname.");
            reed_buffer_destroy(&editor->buffer);
            return false;
        }
        result = editor_open_path(editor, path);
        if (result != MG_OK) {
            set_status(editor, open_error_text(result));
            reed_buffer_destroy(&editor->buffer);
            return false;
        }
    } else if (!reed_buffer_mark_saved(&editor->buffer)) {
        reed_buffer_destroy(&editor->buffer);
        return false;
    }
    editor->frame = (reed_frame_cell_t *)calloc(
        (usize)REED_MAX_SCREEN_ROWS * REED_MAX_SCREEN_COLUMNS,
        sizeof(*editor->frame));
    editor->presented_frame = (reed_frame_cell_t *)calloc(
        (usize)REED_MAX_SCREEN_ROWS * REED_MAX_SCREEN_COLUMNS,
        sizeof(*editor->presented_frame));
    if (!editor->frame || !editor->presented_frame) {
        set_status(editor, "out of memory while opening editor.");
        editor_destroy_frames(editor);
        if (editor->existing_file) (void)handle_close(editor->file_handle);
        reed_buffer_destroy(&editor->buffer);
        return false;
    }
    return true;
}

int reed_run(const char *pathname)
{
    reed_editor_t *editor;
    mg_terminal_key_event_t event;
    mg_result_t result;
    int exit_status = 0;

    editor = (reed_editor_t *)calloc(1, sizeof(*editor));
    if (!editor || !editor_init(editor, pathname)) {
        const char *error = editor && editor->status[0]
            ? editor->status : "could not open buffer.";
        printf("reed: %s\n", error);
        free(editor);
        return 1;
    }
    result = terminal_alternate_enter();
    if (result != MG_OK) {
        editor_destroy_frames(editor);
        if (editor->existing_file) (void)handle_close(editor->file_handle);
        reed_buffer_destroy(&editor->buffer);
        free(editor);
        printf("reed: alternate screen unavailable.\n");
        return 1;
    }
    editor->alternate_entered = true;
    result = terminal_input_raw(true);
    if (result != MG_OK) {
        (void)terminal_alternate_leave();
        editor_destroy_frames(editor);
        if (editor->existing_file) (void)handle_close(editor->file_handle);
        reed_buffer_destroy(&editor->buffer);
        free(editor);
        printf("reed: raw keyboard input unavailable.\n");
        return 1;
    }
    editor->raw_input = true;
    (void)terminal_cursor_set_visible(true);
    while (editor->running) {
        if (!editor_render(editor)) {
            exit_status = 1;
            break;
        }
        result = terminal_read_key_event(MG_TERMINAL_WAIT_FOREVER, &event);
        if (result != MG_OK) {
            exit_status = 1;
            break;
        }
        if (event.version != MG_TERMINAL_API_VERSION ||
            (event.flags & ~MG_TERMINAL_KEY_EVENT_REPEAT) != 0) {
            set_status(editor, "Invalid keyboard event.");
            continue;
        }
        if (editor->prompt != REED_PROMPT_NONE)
            handle_prompt_event(editor, &event);
        else if (editor->mode == REED_MODE_INSERT)
            dispatch_insert(editor, &event);
        else
            dispatch_normal(editor, &event);
    }
    if (editor->raw_input) (void)terminal_input_raw(false);
    if (editor->alternate_entered && terminal_alternate_leave() != MG_OK)
        exit_status = 1;
    if (editor->existing_file) (void)handle_close(editor->file_handle);
    reed_buffer_destroy(&editor->buffer);
    editor_destroy_frames(editor);
    free(editor);
    return exit_status;
}

static bool position_in_search_match(const reed_editor_t *editor, usize position,
                                     bool *out_current)
{
    usize length;

    if (out_current) *out_current = false;
    if (!editor || !editor->search_active || !editor->search_length) return false;
    length = reed_buffer_length(&editor->buffer);
    for (usize start = 0;
         start <= length && editor->search_length <= length - start; start++) {
        if (query_matches_at(editor, start) &&
            position >= start && position < start + editor->search_length) {
            if (out_current) *out_current = start == editor->search_match;
            return true;
        }
    }
    return false;
}

static bool decode_frame_text(const char *text, usize length, usize position,
                              u32 *out_codepoint, usize *out_next)
{
    u8 first;
    usize count = 1;
    u32 codepoint;

    if (!text || position >= length) return false;
    first = (u8)text[position];
    if (first < 0x80U) {
        codepoint = first;
    } else {
        if (first >= 0xC2U && first <= 0xDFU) count = 2;
        else if (first >= 0xE0U && first <= 0xEFU) count = 3;
        else if (first >= 0xF0U && first <= 0xF4U) count = 4;
        else return false;
        if (count > length - position) return false;
        codepoint = first & ((1U << (8U - count - 1U)) - 1U);
        for (usize index = 1; index < count; index++) {
            u8 byte = (u8)text[position + index];
            if ((byte & 0xC0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (byte & 0x3FU);
        }
        if ((count == 2 && codepoint < 0x80U) ||
            (count == 3 && codepoint < 0x800U) ||
            (count == 4 && codepoint < 0x10000U) ||
            codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) return false;
    }
    if (out_codepoint) *out_codepoint = codepoint;
    if (out_next) *out_next = position + count;
    return true;
}

static void frame_fill(reed_frame_cell_t *frame, u32 rows, u32 columns,
                       mg_terminal_color_t foreground,
                       mg_terminal_color_t background)
{
    if (!frame) return;
    for (u32 row = 0; row < rows; row++) {
        for (u32 column = 0; column < columns; column++) {
            reed_frame_cell_t *cell =
                &frame[(usize)row * columns + column];
            cell->codepoint = ' ';
            cell->foreground = (u8)foreground;
            cell->background = (u8)background;
            cell->reserved = 0;
        }
    }
}

static void frame_put_cell(reed_frame_cell_t *frame, u32 columns,
                           u32 row, u32 column, u32 codepoint,
                           mg_terminal_color_t foreground,
                           mg_terminal_color_t background)
{
    reed_frame_cell_t *cell;

    if (!frame || !columns) return;
    cell = &frame[(usize)row * columns + column];
    cell->codepoint = codepoint;
    cell->foreground = (u8)foreground;
    cell->background = (u8)background;
    cell->reserved = 0;
}

static usize frame_text_cells(const char *text, usize length)
{
    usize position = 0;
    usize cells = 0;

    while (position < length) {
        usize next;

        if (!decode_frame_text(text, length, position, NULL, &next)) {
            next = position + 1U;
        }
        position = next;
        cells++;
    }
    return cells;
}

static bool frame_put_text(reed_frame_cell_t *frame, u32 columns, u32 row,
                           const char *text, usize length, usize first_cell,
                           mg_terminal_color_t foreground,
                           mg_terminal_color_t background)
{
    usize position = 0;
    usize cell_index = 0;

    if (!frame || !columns || !text) return false;
    while (position < length) {
        u32 codepoint;
        usize next;

        if (!decode_frame_text(text, length, position, &codepoint, &next)) {
            codepoint = '?';
            next = position + 1U;
        }
        if (cell_index >= first_cell && cell_index - first_cell < columns) {
            if (codepoint < 0x20U && codepoint != '\t') codepoint = ' ';
            frame_put_cell(frame, columns, row,
                           (u32)(cell_index - first_cell), codepoint,
                           foreground, background);
        }
        position = next;
        cell_index++;
    }
    return true;
}

static bool render_line_to_frame(const reed_editor_t *editor, usize line,
                                 u32 row, u32 columns,
                                 reed_frame_cell_t *frame)
{
    usize start;
    usize end;
    usize position;
    usize column = 0;
    usize first_column = 0;

    if (!editor || !frame || !reed_buffer_line_bounds(&editor->buffer, line,
                                                       &start, &end))
        return false;
    if (line == reed_buffer_line_at(&editor->buffer,
                                    reed_buffer_cursor(&editor->buffer)))
        first_column = editor->horizontal_column;
    position = start;
    while (position < end) {
        u32 codepoint;
        usize next;
        bool current_match = false;
        bool match;
        mg_terminal_color_t foreground;
        mg_terminal_color_t background;

        if (!reed_buffer_codepoint(&editor->buffer, position, &codepoint,
                                   &next) || next > end) return false;
        match = position_in_search_match(editor, position, &current_match);
        foreground = match ? MG_TERMINAL_COLOR_BLACK
                           : MG_TERMINAL_COLOR_LIGHT_GRAY;
        background = match ? (current_match
                                  ? MG_TERMINAL_COLOR_BRIGHT_YELLOW
                                  : MG_TERMINAL_COLOR_CYAN)
                           : MG_TERMINAL_COLOR_BLACK;
        if (column >= first_column && column - first_column < columns) {
            if (codepoint < 0x20U && codepoint != '\t') codepoint = ' ';
            frame_put_cell(frame, columns, row,
                           (u32)(column - first_column), codepoint,
                           foreground, background);
        }
        column++;
        position = next;
    }
    return true;
}

static void adjust_view(reed_editor_t *editor)
{
    usize line;
    usize column;
    usize visible_rows;

    if (!editor) return;
    visible_rows = editor->screen_rows > 1U ? editor->screen_rows - 1U : 1U;
    line = reed_buffer_line_at(&editor->buffer, reed_buffer_cursor(&editor->buffer));
    column = reed_buffer_line_column(&editor->buffer,
                                     reed_buffer_cursor(&editor->buffer));
    if (line < editor->top_line) editor->top_line = line;
    if (line >= editor->top_line + visible_rows)
        editor->top_line = line - visible_rows + 1U;
    if (column < editor->horizontal_column)
        editor->horizontal_column = column;
    if (column >= editor->horizontal_column + editor->screen_columns)
        editor->horizontal_column = column - editor->screen_columns + 1U;
}

static void frame_build_status(reed_editor_t *editor, u32 row, u32 columns,
                               u32 *out_cursor_row,
                               u32 *out_cursor_column)
{
    char status[REED_STATUS_CAPACITY];
    const char *path;
    const char *status_mode;
    int status_length;
    usize line;
    usize column;

    if (!editor || !out_cursor_row || !out_cursor_column) return;
    if (editor->prompt != REED_PROMPT_NONE) {
        const char *prefix = "";
        char line_text[REED_STATUS_CAPACITY];
        usize prefix_length;
        usize input_length;
        usize total_length;
        usize cursor_length;
        usize first_cell;
        usize cursor_cell;

        switch (editor->prompt) {
            case REED_PROMPT_COMMAND: prefix = "/"; break;
            case REED_PROMPT_SEARCH_FORWARD: prefix = "Find forward: "; break;
            case REED_PROMPT_SEARCH_BACKWARD: prefix = "Find backward: "; break;
            case REED_PROMPT_CONFIRM:
                prefix = "Save changes? [s] Save [d] Don't Save [c] Cancel: ";
                break;
            case REED_PROMPT_SAVE_PATH: prefix = "Save as: "; break;
            case REED_PROMPT_NONE: break;
        }
        prefix_length = strlen(prefix);
        if (prefix_length >= sizeof(line_text))
            prefix_length = sizeof(line_text) - 1U;
        memcpy(line_text, prefix, prefix_length);
        input_length = editor->input_length;
        if (input_length > sizeof(line_text) - 1U - prefix_length)
            input_length = sizeof(line_text) - 1U - prefix_length;
        memcpy(line_text + prefix_length, editor->input, input_length);
        total_length = prefix_length + input_length;
        line_text[total_length] = '\0';
        cursor_length = editor->input_cursor;
        if (cursor_length > input_length) cursor_length = input_length;
        cursor_cell = frame_text_cells(line_text, prefix_length + cursor_length);
        first_cell = frame_text_cells(line_text, total_length) > columns
            ? frame_text_cells(line_text, total_length) - columns : 0;
        for (usize column = 0; column < columns; column++)
            frame_put_cell(editor->frame, columns, row, (u32)column, ' ',
                           MG_TERMINAL_COLOR_WHITE,
                           MG_TERMINAL_COLOR_BLACK);
        (void)frame_put_text(editor->frame, columns, row, line_text,
                             total_length, first_cell,
                             MG_TERMINAL_COLOR_WHITE,
                             MG_TERMINAL_COLOR_BLACK);
        *out_cursor_row = row;
        *out_cursor_column = cursor_cell > first_cell
            ? (u32)(cursor_cell - first_cell) : 0;
        if (*out_cursor_column >= columns) *out_cursor_column = columns - 1U;
        return;
    }

    for (u32 column_index = 0; column_index < columns; column_index++)
        frame_put_cell(editor->frame, columns, row, column_index, ' ',
                       MG_TERMINAL_COLOR_BRIGHT_CYAN,
                       MG_TERMINAL_COLOR_BLACK);
    path = reed_buffer_path(&editor->buffer);
    status_mode = mode_name(editor->mode);
    status_length = snprintf(status, sizeof(status), "%s  %s%s  %llu:%llu%s",
                             status_mode, path ? path : "[unnamed]",
                             editor->save_uncertain ? " [?]" :
                             (reed_buffer_is_modified(&editor->buffer)
                                 ? " [+]" : ""),
                             (unsigned long long)(reed_buffer_line_at(
                                 &editor->buffer,
                                 reed_buffer_cursor(&editor->buffer)) + 1U),
                             (unsigned long long)(reed_buffer_line_column(
                                 &editor->buffer,
                                 reed_buffer_cursor(&editor->buffer)) + 1U),
                             editor->status[0] ? "  " : "");
    if (status_length < 0) status_length = 0;
    if ((usize)status_length > sizeof(status) - 1U)
        status_length = sizeof(status) - 1U;
    if (editor->status[0] && (usize)status_length < sizeof(status) - 1U) {
        usize used = (usize)status_length;
        usize remain = sizeof(status) - 1U - used;
        usize message_length = strlen(editor->status);

        if (message_length > remain) message_length = remain;
        memcpy(status + used, editor->status, message_length);
        used += message_length;
        status[used] = '\0';
        status_length = (int)used;
    }
    if ((usize)status_length > columns) status_length = (int)columns;
    (void)frame_put_text(editor->frame, columns, row, status,
                         (usize)status_length, 0,
                         MG_TERMINAL_COLOR_BRIGHT_CYAN,
                         MG_TERMINAL_COLOR_BLACK);
    line = reed_buffer_line_at(&editor->buffer,
                               reed_buffer_cursor(&editor->buffer));
    column = reed_buffer_line_column(&editor->buffer,
                                     reed_buffer_cursor(&editor->buffer));
    *out_cursor_row = line >= editor->top_line
        ? (u32)(line - editor->top_line) : 0;
    *out_cursor_column = column >= editor->horizontal_column
        ? (u32)(column - editor->horizontal_column) : 0;
    if (*out_cursor_row >= row) *out_cursor_row = row - 1U;
    if (*out_cursor_column >= columns) *out_cursor_column = columns - 1U;
}

static bool frame_cell_equal(const reed_frame_cell_t *left,
                             const reed_frame_cell_t *right)
{
    return left && right && left->codepoint == right->codepoint &&
           left->foreground == right->foreground &&
           left->background == right->background;
}

static bool emit_frame_run(const reed_frame_cell_t *frame, u32 columns,
                           u32 row, u32 first, u32 last)
{
    while (first < last) {
        u32 end = first + 1U;
        u8 foreground = frame[(usize)row * columns + first].foreground;
        u8 background = frame[(usize)row * columns + first].background;
        char text[REED_MAX_SCREEN_COLUMNS * 4U];
        usize length = 0;

        while (end < last &&
               frame[(usize)row * columns + end].foreground == foreground &&
               frame[(usize)row * columns + end].background == background)
            end++;
        for (u32 column = first; column < end; column++) {
            char encoded[4];
            usize encoded_length;
            u32 codepoint = frame[(usize)row * columns + column].codepoint;

            if (!encode_codepoint(codepoint, encoded, &encoded_length)) {
                codepoint = '?';
                if (!encode_codepoint(codepoint, encoded, &encoded_length))
                    return false;
            }
            if (encoded_length > sizeof(text) - length) return false;
            memcpy(text + length, encoded, encoded_length);
            length += encoded_length;
        }
        if (terminal_cursor_move(row, first) != MG_OK ||
            !write_style(text, length, (mg_terminal_color_t)foreground,
                         (mg_terminal_color_t)background)) return false;
        first = end;
    }
    return true;
}

static bool emit_frame_changes(const reed_editor_t *editor, u32 rows,
                               u32 columns, bool full_redraw)
{
    if (!editor || !editor->frame || !editor->presented_frame) return false;
    for (u32 row = 0; row < rows; row++) {
        u32 column = 0;

        while (column < columns) {
            bool changed = full_redraw ||
                !frame_cell_equal(&editor->frame[(usize)row * columns + column],
                                  &editor->presented_frame[(usize)row * columns + column]);
            u32 first;

            if (!changed) {
                column++;
                continue;
            }
            first = column++;
            while (column < columns &&
                   (full_redraw ||
                    !frame_cell_equal(&editor->frame[(usize)row * columns + column],
                                      &editor->presented_frame[(usize)row * columns + column])))
                column++;
            if (!emit_frame_run(editor->frame, columns, row, first, column))
                return false;
        }
    }
    return true;
}

static bool editor_render(reed_editor_t *editor)
{
    mg_terminal_size_t size;
    u32 cursor_row;
    u32 cursor_column;
    bool full_redraw;

    if (!editor || !editor->frame || !editor->presented_frame ||
        terminal_get_size(&size) != MG_OK || size.rows < 2U ||
        !size.columns || size.columns > REED_MAX_SCREEN_COLUMNS ||
        size.rows > REED_MAX_SCREEN_ROWS) return false;
    editor->screen_rows = size.rows;
    editor->screen_columns = size.columns;
    adjust_view(editor);
    frame_fill(editor->frame, size.rows, size.columns,
               MG_TERMINAL_COLOR_LIGHT_GRAY, MG_TERMINAL_COLOR_BLACK);
    for (u32 row = 0; row + 1U < size.rows; row++) {
        usize source_line = editor->top_line + row;

        if (source_line < reed_buffer_line_count(&editor->buffer) &&
            !render_line_to_frame(editor, source_line, row, size.columns,
                                  editor->frame)) return false;
    }
    frame_build_status(editor, size.rows - 1U, size.columns,
                       &cursor_row, &cursor_column);
    full_redraw = !editor->frame_valid || editor->frame_rows != size.rows ||
                  editor->frame_columns != size.columns;
    if (terminal_update_begin() != MG_OK) return false;
    if (full_redraw && terminal_clear_screen() != MG_OK) {
        (void)terminal_update_end();
        return false;
    }
    if (!emit_frame_changes(editor, size.rows, size.columns, full_redraw)) {
        (void)terminal_update_end();
        return false;
    }
    if (terminal_cursor_move(cursor_row, cursor_column) != MG_OK ||
        terminal_update_end() != MG_OK) return false;
    {
        reed_frame_cell_t *old_presented = editor->presented_frame;

        editor->presented_frame = editor->frame;
        editor->frame = old_presented;
    }
    editor->frame_rows = size.rows;
    editor->frame_columns = size.columns;
    editor->frame_valid = true;
    return true;
}
