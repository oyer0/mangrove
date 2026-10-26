/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "editor.h"
#include "buffer.h"
#include "config.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/clipboard.h>
#include <mg/object.h>
#include <mg/terminal.h>
#if defined(REED_GUEST_CONFLICT_SYNC)
#include <mg/process.h>
#include <mg/time.h>
#endif
#include <mg/utf8.h>
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
#define HID_KEY_C                 0x06U
#define HID_KEY_V                 0x19U
#define HID_KEY_X                 0x1BU
#define HID_KEY_D                 0x07U
#define HID_KEY_E                 0x08U
#define HID_KEY_G                 0x0AU
#define HID_KEY_H                 0x0BU
#define HID_KEY_I                 0x0CU
#define HID_KEY_J                 0x0DU
#define HID_KEY_K                 0x0EU
#define HID_KEY_L                 0x0FU
#define HID_KEY_N                 0x11U
#define HID_KEY_O                 0x12U
#define HID_KEY_S                 0x16U
#define HID_KEY_U                 0x18U
#define HID_KEY_LEFT_BRACKET      0x2FU
#define HID_KEY_RIGHT_BRACKET     0x30U
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
    REED_MODE_SELECTION,
    REED_MODE_COMMAND,
    REED_MODE_SEARCH,
} reed_mode_t;

typedef enum {
    REED_SELECTION_NONE,
    REED_SELECTION_CHARACTER,
    REED_SELECTION_LINE,
} reed_selection_t;

typedef enum {
    REED_PROMPT_NONE,
    REED_PROMPT_COMMAND,
    REED_PROMPT_SEARCH,
    REED_PROMPT_CONFIRM,
    REED_PROMPT_CONFLICT,
    REED_PROMPT_SAVE_PATH,
} reed_prompt_t;

typedef enum {
    REED_ACTION_NONE,
    REED_ACTION_COPY,
    REED_ACTION_CUT,
    REED_ACTION_PASTE,
    REED_ACTION_MOVE_LEFT,
    REED_ACTION_MOVE_DOWN,
    REED_ACTION_MOVE_UP,
    REED_ACTION_MOVE_RIGHT,
    REED_ACTION_LINE_START,
    REED_ACTION_LINE_END,
    REED_ACTION_LINE_START_INSERT,
    REED_ACTION_LINE_END_INSERT,
    REED_ACTION_DOCUMENT_START,
    REED_ACTION_DOCUMENT_END,
    REED_ACTION_WORD_PREVIOUS_END,
    REED_ACTION_WORD_CURRENT_PREVIOUS_START,
    REED_ACTION_WORD_CURRENT_NEXT_END,
    REED_ACTION_WORD_NEXT_START,
    REED_ACTION_PARAGRAPH_PREVIOUS,
    REED_ACTION_PARAGRAPH_NEXT,
    REED_ACTION_INSERT,
    REED_ACTION_OPEN_BELOW,
    REED_ACTION_OPEN_ABOVE,
    REED_ACTION_SELECT_CHARACTER,
    REED_ACTION_SELECT_LINE,
    REED_ACTION_SELECT_ALL,
    REED_ACTION_DELETE_CHAR,
    REED_ACTION_DELETE_LEFT,
    REED_ACTION_DELETE_RIGHT,
    REED_ACTION_UNDO,
    REED_ACTION_REDO,
    REED_ACTION_COMMAND,
    REED_ACTION_SEARCH_PROMPT,
    REED_ACTION_PAGE_UP,
    REED_ACTION_PAGE_DOWN,
    REED_ACTION_SEARCH_NEXT,
    REED_ACTION_SEARCH_PREVIOUS,
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

typedef struct { usize position, column; } reed_layout_checkpoint_t;
typedef struct {
    usize start, end, width, row, checkpoint;
} reed_layout_line_t;

typedef struct reed_editor {
    reed_buffer_t buffer;
    mg_handle_t file_handle;
    bool existing_file;
    bool save_uncertain;
    bool save_conflict;
    reed_mode_t mode;
    reed_prompt_t prompt;
    bool running;
    bool alternate_entered;
    bool raw_input;
    bool insert_transaction;
    bool insert_recorded;
    reed_selection_t selection;
    usize selection_anchor;
    bool selection_endpoint_inclusive;
    reed_action_t clipboard_operator;
    usize desired_column;
    usize top_visual_row;
    usize horizontal_column;
    reed_config_t config;
    u32 screen_rows;
    u32 screen_columns;
    char input[REED_INPUT_CAPACITY];
    usize input_length;
    usize input_cursor;
    char status[REED_STATUS_CAPACITY];
    char search_text[REED_INPUT_CAPACITY];
    usize search_length;
    usize search_match;
    usize search_anchor;
    bool search_active;
    reed_frame_cell_t *frame;
    reed_frame_cell_t *presented_frame;
    u32 frame_rows;
    u32 frame_columns;
    bool frame_valid;
    reed_layout_line_t *layout_lines;
    reed_layout_checkpoint_t *layout_points;
    usize layout_line_count, layout_point_count, layout_point_capacity;
    u64 layout_revision;
    u32 layout_columns, layout_tab_width;
    bool layout_wrap;
} reed_editor_t;

#if defined(REED_GUEST_CONFLICT_SYNC)
static char reed_conflict_sync_run[17];

int reed_guest_conflict_sync_enable(const char *run_id)
{
    if (!run_id || strlen(run_id) != 16U) return 0;
    for (usize index = 0; index < 16U; index++) {
        char value = run_id[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) return 0;
    }
    memcpy(reed_conflict_sync_run, run_id, sizeof(reed_conflict_sync_run));
    return 1;
}
#endif

static bool editor_render(reed_editor_t *editor);
static char lower_ascii(char value);
static void set_cursor(reed_editor_t *editor, usize position);

static const reed_key_binding_t normal_keymap[] = {
    { HID_KEY_H, false, false, false, REED_ACTION_MOVE_LEFT },
    { HID_KEY_J, false, false, false, REED_ACTION_MOVE_DOWN },
    { HID_KEY_K, false, false, false, REED_ACTION_MOVE_UP },
    { HID_KEY_L, false, false, false, REED_ACTION_MOVE_RIGHT },
    { HID_KEY_LEFT, false, false, false, REED_ACTION_MOVE_LEFT },
    { HID_KEY_DOWN, false, false, false, REED_ACTION_MOVE_DOWN },
    { HID_KEY_UP, false, false, false, REED_ACTION_MOVE_UP },
    { HID_KEY_RIGHT, false, false, false, REED_ACTION_MOVE_RIGHT },
    { HID_KEY_A, false, false, false, REED_ACTION_LINE_START },
    { HID_KEY_E, false, false, false, REED_ACTION_LINE_END },
    { HID_KEY_A, true, false, false, REED_ACTION_LINE_START_INSERT },
    { HID_KEY_E, true, false, false, REED_ACTION_LINE_END_INSERT },
    { HID_KEY_G, false, false, false, REED_ACTION_DOCUMENT_START },
    { HID_KEY_G, true, false, false, REED_ACTION_DOCUMENT_END },
    { HID_KEY_HOME, false, false, false, REED_ACTION_LINE_START },
    { HID_KEY_END, false, false, false, REED_ACTION_LINE_END },
    { HID_KEY_HOME, false, false, true, REED_ACTION_DOCUMENT_START },
    { HID_KEY_END, false, false, true, REED_ACTION_DOCUMENT_END },
    { HID_KEY_B, false, false, false, REED_ACTION_WORD_PREVIOUS_END },
    { HID_KEY_B, true, false, false, REED_ACTION_WORD_CURRENT_PREVIOUS_START },
    { HID_KEY_N, false, false, false, REED_ACTION_WORD_CURRENT_NEXT_END },
    { HID_KEY_N, true, false, false, REED_ACTION_WORD_NEXT_START },
    { HID_KEY_LEFT_BRACKET, false, false, false, REED_ACTION_PARAGRAPH_PREVIOUS },
    { HID_KEY_RIGHT_BRACKET, false, false, false, REED_ACTION_PARAGRAPH_NEXT },
    { HID_KEY_I, false, false, false, REED_ACTION_INSERT },
    { HID_KEY_O, false, false, false, REED_ACTION_OPEN_BELOW },
    { HID_KEY_O, true, false, false, REED_ACTION_OPEN_ABOVE },
    { HID_KEY_S, false, false, false, REED_ACTION_SELECT_CHARACTER },
    { HID_KEY_S, true, false, false, REED_ACTION_SELECT_LINE },
    { HID_KEY_S, false, true, false, REED_ACTION_SELECT_ALL },
    { HID_KEY_C, false, false, false, REED_ACTION_COPY },
    { HID_KEY_X, false, false, false, REED_ACTION_CUT },
    { HID_KEY_V, false, false, false, REED_ACTION_PASTE },
    { HID_KEY_D, false, false, false, REED_ACTION_DELETE_CHAR },
    { HID_KEY_BACKSPACE, false, false, false, REED_ACTION_DELETE_LEFT },
    { HID_KEY_DELETE, false, false, false, REED_ACTION_DELETE_RIGHT },
    { HID_KEY_U, false, false, false, REED_ACTION_UNDO },
    { HID_KEY_U, true, false, false, REED_ACTION_REDO },
    { HID_KEY_SLASH, false, false, false, REED_ACTION_COMMAND },
    { HID_KEY_SLASH, true, false, false, REED_ACTION_SEARCH_PROMPT },
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
    free(editor->layout_lines);
    free(editor->layout_points);
    editor->layout_lines = NULL;
    editor->layout_points = NULL;
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
    usize consumed = 0;
    u32 scalar;

    if (!editor || position >= editor->input_length) return editor
        ? editor->input_length : 0;
    if (mg_utf8_decode_one((const u8 *)editor->input + position,
                           editor->input_length - position, &scalar,
                           &consumed) != MG_UTF8_OK || !consumed)
        consumed = 1U;
    return position + consumed;
}

static usize input_previous_boundary(const reed_editor_t *editor,
                                     usize position)
{
    usize first;
    if (!editor || !position || position > editor->input_length) return 0;
    first = position > 4U ? position - 4U : 0U;
    for (usize start = first; start < position; start++) {
        usize consumed;
        u32 scalar;
        if (mg_utf8_decode_one((const u8 *)editor->input + start,
                               position - start, &scalar, &consumed) ==
                MG_UTF8_OK && consumed == position - start)
            return start;
    }
    return position - 1U;
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
    usize produced;
    if (!output || !out_length ||
        mg_utf8_encode_one(codepoint, (u8 *)output, 4U, &produced) !=
            MG_UTF8_OK) return false;
    *out_length = produced;
    return true;
}

static bool resolve_path_alloc(const char *input, char **out_path)
{
    return command_expand_home_path_alloc(input, true, out_path);
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
    for (u32 attempt = 0;; attempt++) {
        text = (char *)malloc(capacity + 1U);
        if (!text) return MG_ERR_NO_MEMORY;
        result = file_snapshot(handle, text, capacity, &state);
        if (result != MG_ERR_BUFFER_TOO_SMALL) break;
        free(text);
        if (attempt == 3U || capacity == REED_MAX_BUFFER_BYTES)
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
    char *text = NULL;
    usize length = 0;
    if (!editor || !editor->existing_file) return MG_ERR_UNSUPPORTED;
    if (!reed_buffer_copy_file_allocated(&editor->buffer, &text, &length))
        return MG_ERR_NO_MEMORY;
    mg_result_t result = file_replace(editor->file_handle, text, length);
    if (result == MG_OK) {
        usize saved_length = reed_buffer_length(&editor->buffer);
        text[saved_length] = '\0';
        reed_buffer_commit_saved(&editor->buffer, text, saved_length);
    } else free(text);
    return result;
}

static mg_result_t save_created_file(reed_editor_t *editor, const char *path)
{
    char *text = NULL, *path_copy = NULL;
    usize length = 0, path_length;
    mg_handle_t new_handle = 0;
    mg_handle_t old_handle;
    bool old_existing;
    mg_result_t result;

    if (!editor || !path || !*path) return MG_ERR_BAD_ARGUMENT;
    path_length = strlen(path);
    if (!path_length || path_length >= REED_MAX_PATH_BYTES)
        return MG_ERR_BAD_ARGUMENT;
    path_copy = (char *)malloc(path_length + 1U);
    if (!path_copy) return MG_ERR_NO_MEMORY;
    memcpy(path_copy, path, path_length + 1U);
    if (!reed_buffer_copy_file_allocated(&editor->buffer, &text, &length)) {
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
    usize saved_length = reed_buffer_length(&editor->buffer);
    text[saved_length] = '\0';
    reed_buffer_commit_saved(&editor->buffer, text, saved_length);
    if (old_existing) (void)handle_close(old_handle);
    return MG_OK;

failure:
    free(text);
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
        !reed_buffer_set_path(&next, path)) {
        result = MG_ERR_NO_MEMORY;
        goto failed;
    }
    reed_buffer_commit_saved(&next, text, length);
    free(editor->layout_lines);
    free(editor->layout_points);
    editor->layout_lines = NULL;
    editor->layout_points = NULL;
    previous = editor->buffer;
    editor->buffer = next;
    reed_buffer_destroy(&previous);
    if (editor->existing_file) (void)handle_close(editor->file_handle);
    editor->file_handle = next_handle;
    editor->existing_file = next_existing;
    editor->save_uncertain = false;
    editor->save_conflict = false;
    editor->desired_column = 0;
    editor->top_visual_row = 0;
    editor->horizontal_column = 0;
    editor->selection = REED_SELECTION_NONE;
    editor->search_active = false;
    editor->search_length = 0;
    editor->search_anchor = 0;
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
        case REED_MODE_SELECTION: return "SELECT";
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
    const char *query;
    usize query_length;
    usize length;

    length = editor ? reed_buffer_length(&editor->buffer) : 0;
    if (!editor) return false;
    if (editor->prompt == REED_PROMPT_SEARCH) {
        query = editor->input;
        query_length = editor->input_length;
    } else {
        if (!editor->search_active) return false;
        query = editor->search_text;
        query_length = editor->search_length;
    }
    if (!query_length || query_length > length ||
        position > length - query_length ||
        query_length >= sizeof(candidate)) return false;
    if (!reed_buffer_copy_slice(&editor->buffer, position, candidate,
                                query_length)) return false;
    return memcmp(candidate, query, query_length) == 0;
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

static bool word_character_at(const reed_buffer_t *buffer, usize position)
{
    return buffer && position < reed_buffer_length(buffer) &&
           is_word_byte((u8)buffer_byte_at(buffer, position));
}

static usize word_run_start(const reed_buffer_t *buffer, usize position)
{
    while (position) {
        usize previous = reed_buffer_previous_boundary(buffer, position);
        if (!word_character_at(buffer, previous)) break;
        position = previous;
    }
    return position;
}

static usize word_run_end(const reed_buffer_t *buffer, usize position)
{
    usize length = reed_buffer_length(buffer);

    while (position < length && word_character_at(buffer, position))
        position = reed_buffer_next_boundary(buffer, position);
    return position;
}

static usize word_last_character(const reed_buffer_t *buffer,
                                 usize position)
{
    usize end = word_run_end(buffer, position);

    return end > position ? reed_buffer_previous_boundary(buffer, end)
                          : position;
}

/* n: reach the current word's last scalar, then advance to the next word. */
static usize word_current_next_end(const reed_buffer_t *buffer,
                                   usize position)
{
    usize original = position;
    usize length;

    if (!buffer) return 0;
    length = reed_buffer_length(buffer);
    if (word_character_at(buffer, position)) {
        usize end = word_run_end(buffer, position);
        usize last = reed_buffer_previous_boundary(buffer, end);

        if (position < last) return last;
        position = end;
    }
    while (position < length && !word_character_at(buffer, position))
        position = reed_buffer_next_boundary(buffer, position);
    if (position >= length) return original;
    return word_last_character(buffer, position);
}

/* N: skip the word under the cursor and find the next word start. */
static usize word_next_start(const reed_buffer_t *buffer, usize position)
{
    usize original = position;
    usize length;

    if (!buffer) return 0;
    length = reed_buffer_length(buffer);
    if (word_character_at(buffer, position))
        position = word_run_end(buffer, position);
    while (position < length && !word_character_at(buffer, position))
        position = reed_buffer_next_boundary(buffer, position);
    return position < length ? position : original;
}

/* B: start of the current word, or the previous word when already at start. */
static usize word_current_previous_start(const reed_buffer_t *buffer,
                                         usize position)
{
    usize original = position;

    if (!buffer) return 0;
    if (word_character_at(buffer, position)) {
        usize start = word_run_start(buffer, position);

        if (start < position) return start;
        position = start;
    }
    while (position) {
        usize previous = reed_buffer_previous_boundary(buffer, position);
        if (word_character_at(buffer, previous))
            return word_run_start(buffer, previous);
        position = previous;
    }
    return original;
}

/* b: skip the containing word and stop on the preceding word's last scalar. */
static usize word_previous_end(const reed_buffer_t *buffer, usize position)
{
    usize original = position;

    if (!buffer) return 0;
    while (position) {
        usize previous = reed_buffer_previous_boundary(buffer, position);
        if (!word_character_at(buffer, previous)) {
            position = previous;
            continue;
        }
        {
            usize start = word_run_start(buffer, previous);
            usize end = word_run_end(buffer, previous);
            if (end < original)
                return reed_buffer_previous_boundary(buffer, end);
            position = start;
        }
    }
    return original;
}

static bool line_is_blank(const reed_buffer_t *buffer, usize line)
{
    usize start;
    usize end;

    if (!buffer || !reed_buffer_line_bounds(buffer, line, &start, &end))
        return true;
    while (start < end) {
        char byte = buffer_byte_at(buffer, start);
        if (byte != ' ' && byte != '\t' && byte != '\r') return false;
        start++;
    }
    return true;
}

static bool paragraph_start_line(const reed_buffer_t *buffer, usize line,
                                 usize *out_start)
{
    usize count = reed_buffer_line_count(buffer);

    if (!buffer || !out_start || line >= count) return false;
    while (line && line_is_blank(buffer, line)) line--;
    if (line_is_blank(buffer, line)) return false;
    while (line && !line_is_blank(buffer, line - 1U)) line--;
    *out_start = line;
    return true;
}

static void move_paragraph_previous(reed_editor_t *editor)
{
    usize cursor;
    usize line;
    usize paragraph;
    usize line_start;
    usize line_end;
    usize target;
    usize position;

    if (!editor) return;
    cursor = reed_buffer_cursor(&editor->buffer);
    line = reed_buffer_line_at(&editor->buffer, cursor);
    if (!paragraph_start_line(&editor->buffer, line, &paragraph)) return;
    if (!reed_buffer_line_bounds(&editor->buffer, line, &line_start,
                                 &line_end)) return;
    if (line_is_blank(&editor->buffer, line) || line > paragraph ||
        cursor > line_start)
        target = paragraph;
    else {
        if (!paragraph) return;
        if (!paragraph_start_line(&editor->buffer, paragraph - 1U, &target))
            return;
    }
    if (reed_buffer_line_bounds(&editor->buffer, target, &position, &line_end))
        set_cursor(editor, position);
}

static void move_paragraph_next(reed_editor_t *editor)
{
    usize line;
    usize count;
    usize target;
    usize position;

    if (!editor) return;
    line = reed_buffer_line_at(&editor->buffer,
                               reed_buffer_cursor(&editor->buffer));
    count = reed_buffer_line_count(&editor->buffer);
    target = line;
    if (!line_is_blank(&editor->buffer, line)) {
        while (target < count && !line_is_blank(&editor->buffer, target))
            target++;
    }
    while (target < count && line_is_blank(&editor->buffer, target)) target++;
    if (target < count &&
        reed_buffer_line_bounds(&editor->buffer, target, &position, &line))
        set_cursor(editor, position);
}

/* Reed lays out valid UTF-8 scalars as one cell each. Tabs expand to the next
 * configured stop. This is deliberately scalar/cell layout, not grapheme or
 * East Asian width shaping. */
static u32 layout_columns(const reed_editor_t *editor)
{
    return editor && editor->screen_columns ? editor->screen_columns : 80U;
}

static usize tab_cells(const reed_editor_t *editor, usize column)
{
    usize width = editor && editor->config.tab_width
        ? editor->config.tab_width : 4U;
    return width - column % width;
}

/* The common within-line edit reflows only that logical line. Unchanged
 * scalar checkpoints and row prefixes are shifted with checked logical byte
 * deltas. Structural edits use the complete rebuild below. */
static bool layout_splice(reed_editor_t *editor)
{
    reed_buffer_t *buffer = &editor->buffer;
    usize line = reed_buffer_line_at(buffer, buffer->change_position);
    usize start, end;
    if (!buffer->change_within_line || line >= editor->layout_line_count ||
        !reed_buffer_line_bounds(buffer, line, &start, &end)) return false;
    usize capacity = (end - start) / 256U + 1U;
    if (capacity > (usize)-1 / sizeof(reed_layout_checkpoint_t)) return false;
    reed_layout_checkpoint_t *points = malloc(capacity * sizeof(*points));
    if (!points) return false;
    usize used = 1, position = start, column = 0;
    points[0] = (reed_layout_checkpoint_t){start, 0};
    while (position < end) {
        if (position - points[used - 1].position >= 256U)
            points[used++] = (reed_layout_checkpoint_t){position, column};
        u32 cp; usize next;
        if (!reed_buffer_codepoint(buffer, position, &cp, &next)) {free(points);return false;}
        column += cp == '\t' ? tab_cells(editor, column) : 1U;
        position = next;
    }
    usize first = editor->layout_lines[line].checkpoint;
    usize last = editor->layout_lines[line + 1].checkpoint;
    usize tail = editor->layout_point_count - last;
    usize total = first + used + tail;
    if (total > editor->layout_point_capacity) {free(points);return false;}
    memmove(editor->layout_points + first + used, editor->layout_points + last,
            tail * sizeof(*points));
    memcpy(editor->layout_points + first, points, used * sizeof(*points));
    free(points);
    for (usize i = first + used; i < total; i++)
        editor->layout_points[i].position = editor->layout_points[i].position -
            buffer->change_removed + buffer->change_inserted;
    usize old_rows = editor->layout_lines[line + 1].row - editor->layout_lines[line].row;
    usize new_rows = editor->config.wrap && column ?
        (column - 1U) / layout_columns(editor) + 1U : 1U;
    editor->layout_lines[line].end = end;
    editor->layout_lines[line].width = column;
    for (usize i = line + 1; i <= editor->layout_line_count; i++) {
        reed_layout_line_t *cached = &editor->layout_lines[i];
        cached->row = cached->row - old_rows + new_rows;
        cached->checkpoint = cached->checkpoint - (last - first) + used;
        if (i < editor->layout_line_count) {
            cached->start = cached->start - buffer->change_removed + buffer->change_inserted;
            cached->end = cached->end - buffer->change_removed + buffer->change_inserted;
        }
    }
    editor->layout_point_count = total;
    editor->layout_revision = buffer->revision;
    return true;
}

/* Rebuilt after text/configuration changes; navigation reuses cell widths,
 * visual-row prefixes, and sparse scalar-safe checkpoints. Allocation failure
 * uses the original streaming layout below, without affecting document data. */
static bool layout_prepare(const reed_editor_t *constant_editor)
{
    reed_editor_t *editor = (reed_editor_t *)constant_editor;
    usize count = reed_buffer_line_count(&editor->buffer);
    bool compatible = editor->layout_lines &&
        editor->layout_columns == editor->screen_columns &&
        editor->layout_tab_width == editor->config.tab_width &&
        editor->layout_wrap == editor->config.wrap;
    if (compatible && editor->layout_revision == editor->buffer.revision) return true;
    if (compatible && editor->layout_revision + 1U == editor->buffer.revision &&
        count == editor->layout_line_count && layout_splice(editor)) return true;
    usize length = reed_buffer_length(&editor->buffer);
    usize points_capacity = length / 256U + count + 1U;
    if (count >= (usize)-1 / sizeof(reed_layout_line_t) ||
        points_capacity > (usize)-1 / sizeof(reed_layout_checkpoint_t)) return false;
    reed_layout_line_t *lines = malloc((count + 1U) * sizeof(*lines));
    reed_layout_checkpoint_t *points = malloc(points_capacity * sizeof(*points));
    if (!lines || !points) { free(lines); free(points); return false; }
    usize used = 0, rows = 0;
    for (usize line = 0; line < count; line++) {
        usize start, end, column = 0;
        reed_buffer_line_bounds(&editor->buffer, line, &start, &end);
        lines[line] = (reed_layout_line_t){.start = start, .end = end,
            .row = rows, .checkpoint = used};
        points[used++] = (reed_layout_checkpoint_t){start, 0};
        while (start < end) {
            if (start - points[used - 1].position >= 256U)
                points[used++] = (reed_layout_checkpoint_t){start, column};
            u32 cp; usize next;
            if (!reed_buffer_codepoint(&editor->buffer, start, &cp, &next)) {
                free(lines); free(points); return false;
            }
            column += cp == '\t' ? tab_cells(editor, column) : 1U;
            start = next;
        }
        lines[line].width = column;
        rows += editor->config.wrap && column ?
            (column - 1U) / layout_columns(editor) + 1U : 1U;
    }
    lines[count] = (reed_layout_line_t){.row = rows, .checkpoint = used};
    free(editor->layout_lines); free(editor->layout_points);
    editor->layout_lines = lines; editor->layout_points = points;
    editor->layout_line_count = count; editor->layout_point_count = used;
    editor->layout_point_capacity = points_capacity;
    editor->layout_revision = editor->buffer.revision;
    editor->layout_columns = editor->screen_columns;
    editor->layout_tab_width = editor->config.tab_width;
    editor->layout_wrap = editor->config.wrap;
    return true;
}

static void layout_seek(const reed_editor_t *editor, usize line, usize wanted,
                         bool byte_position, usize *position, usize *column)
{
    if (!layout_prepare(editor) || line >= editor->layout_line_count) return;
    usize first = editor->layout_lines[line].checkpoint;
    usize low = first, high = editor->layout_lines[line + 1].checkpoint;
    while (low < high) {
        usize mid = low + (high - low) / 2;
        usize value = byte_position ? editor->layout_points[mid].position :
                                     editor->layout_points[mid].column;
        if (value <= wanted) low = mid + 1; else high = mid;
    }
    usize at = low > first ? low - 1 : first;
    *position = editor->layout_points[at].position;
    *column = editor->layout_points[at].column;
}

static usize line_display_column(const reed_editor_t *editor, usize line,
                                 usize position)
{
    usize start;
    usize end;
    usize column = 0;

    if (!editor ||
        !reed_buffer_line_bounds(&editor->buffer, line, &start, &end))
        return 0;
    if (position > end) position = end;
    layout_seek(editor, line, position, true, &start, &column);
    while (start < position) {
        u32 codepoint;
        usize next;

        if (!reed_buffer_codepoint(&editor->buffer, start, &codepoint,
                                   &next) || next > end || next > position)
            break;
        column += codepoint == '\t' ? tab_cells(editor, column) : 1U;
        start = next;
    }
    return column;
}

static bool layout_line_span(const reed_editor_t *editor, usize start,
                             usize *out_end, usize *out_width)
{
    usize length;
    usize position = start;
    usize column = 0;

    if (!editor || !out_end || !out_width) return false;
    length = reed_buffer_length(&editor->buffer);
    if (start > length) return false;
    if (layout_prepare(editor)) {
        usize line = reed_buffer_line_at(&editor->buffer, start);
        if (start == editor->layout_lines[line].start) {
            *out_end = editor->layout_lines[line].end;
            *out_width = editor->layout_lines[line].width;
            return true;
        }
    }
    while (position < length) {
        u32 codepoint;
        usize next;

        if (!reed_buffer_codepoint(&editor->buffer, position, &codepoint,
                                   &next) || next > length) return false;
        if (codepoint == '\n') break;
        column += codepoint == '\t' ? tab_cells(editor, column) : 1U;
        position = next;
    }
    *out_end = position;
    *out_width = column;
    return true;
}

static usize layout_rows_for_width(const reed_editor_t *editor, usize width)
{
    usize columns;

    if (!editor || !editor->config.wrap) return 1U;
    columns = layout_columns(editor);
    return width ? (width - 1U) / columns + 1U : 1U;
}

static usize visual_rows_before_line(const reed_editor_t *editor,
                                     usize line)
{
    usize total = 0;
    usize position = 0;
    usize length;

    if (!editor) return 0;
    if (layout_prepare(editor)) {
        if (line > editor->layout_line_count) line = editor->layout_line_count;
        return editor->layout_lines[line].row;
    }
    length = reed_buffer_length(&editor->buffer);
    for (usize index = 0; index < line; index++) {
        usize end;
        usize width;

        if (!layout_line_span(editor, position, &end, &width)) break;
        total += layout_rows_for_width(editor, width);
        if (end == length) break;
        position = end + 1U;
    }
    return total;
}

static usize visual_row_count(const reed_editor_t *editor)
{
    return visual_rows_before_line(editor,
                                   reed_buffer_line_count(&editor->buffer));
}

static bool visual_row_location(const reed_editor_t *editor, usize visual_row,
                                usize *out_line, usize *out_subrow,
                                usize *out_line_start, usize *out_line_end,
                                usize *out_line_rows)
{
    usize base = 0;
    usize position = 0;
    usize length;

    if (!editor || !out_line || !out_subrow) return false;
    if (layout_prepare(editor)) {
        usize low = 0, high = editor->layout_line_count;
        if (visual_row >= editor->layout_lines[high].row) return false;
        while (low + 1 < high) {
            usize middle = low + (high - low) / 2;
            if (editor->layout_lines[middle].row <= visual_row) low = middle;
            else high = middle;
        }
        *out_line = low;
        *out_subrow = visual_row - editor->layout_lines[low].row;
        usize start, end;
        reed_buffer_line_bounds(&editor->buffer, low, &start, &end);
        if (out_line_start) *out_line_start = start;
        if (out_line_end) *out_line_end = end;
        if (out_line_rows) *out_line_rows = editor->layout_lines[low + 1].row - editor->layout_lines[low].row;
        return true;
    }
    length = reed_buffer_length(&editor->buffer);
    for (usize line = 0; position <= length; line++) {
        usize end;
        usize width;
        usize rows;

        if (!layout_line_span(editor, position, &end, &width)) return false;
        rows = layout_rows_for_width(editor, width);
        if (visual_row < base + rows) {
            *out_line = line;
            *out_subrow = visual_row - base;
            if (out_line_start) *out_line_start = position;
            if (out_line_end) *out_line_end = end;
            if (out_line_rows) *out_line_rows = rows;
            return true;
        }
        base += rows;
        if (end == length) break;
        position = end + 1U;
    }
    return false;
}

static usize cursor_visual_row(const reed_editor_t *editor,
                               usize *out_screen_column)
{
    usize cursor;
    usize line;
    usize column;
    usize screen_column;
    usize visual_subrow = 0;

    if (!editor) return 0;
    cursor = reed_buffer_cursor(&editor->buffer);
    line = reed_buffer_line_at(&editor->buffer, cursor);
    column = line_display_column(editor, line, cursor);
    if (editor->config.wrap) {
        usize columns = layout_columns(editor);
        usize line_start;
        usize line_end;
        bool at_exact_end = reed_buffer_line_bounds(&editor->buffer, line,
            &line_start, &line_end) && cursor == line_end && column &&
            column % columns == 0;

        (void)line_start;
        screen_column = at_exact_end ? columns - 1U : column % columns;
        visual_subrow = at_exact_end ? column / columns - 1U
                                     : column / columns;
    } else {
        screen_column = column >= editor->horizontal_column
            ? column - editor->horizontal_column : 0U;
    }
    if (out_screen_column) *out_screen_column = screen_column;
    return visual_rows_before_line(editor, line) +
           (editor->config.wrap ? visual_subrow : 0U);
}

static bool position_for_display_column(const reed_editor_t *editor,
                                        usize line, usize wanted_column,
                                        usize *out_position)
{
    usize start;
    usize end;
    usize column = 0;

    if (!editor || !out_position ||
        !reed_buffer_line_bounds(&editor->buffer, line, &start, &end))
        return false;
    layout_seek(editor, line, wanted_column, false, &start, &column);
    while (start < end) {
        u32 codepoint;
        usize next;
        usize cells;

        if (!reed_buffer_codepoint(&editor->buffer, start, &codepoint,
                                   &next) || next > end) return false;
        cells = codepoint == '\t' ? tab_cells(editor, column) : 1U;
        if (wanted_column < column + cells) {
            *out_position = start;
            return true;
        }
        column += cells;
        start = next;
    }
    *out_position = end;
    return true;
}

static bool position_for_visual_cell(const reed_editor_t *editor,
                                     usize visual_row, usize screen_column,
                                     usize *out_position)
{
    usize line;
    usize subrow;
    usize wanted_column;

    if (!visual_row_location(editor, visual_row, &line, &subrow, NULL, NULL,
                             NULL)) return false;
    if (editor->config.wrap)
        wanted_column = subrow * layout_columns(editor) + screen_column;
    else
        wanted_column = editor->horizontal_column + screen_column;
    return position_for_display_column(editor, line, wanted_column,
                                       out_position);
}

static void update_desired_column(reed_editor_t *editor)
{
    usize screen_column;

    if (!editor) return;
    (void)cursor_visual_row(editor, &screen_column);
    if (screen_column >= layout_columns(editor))
        screen_column = layout_columns(editor) - 1U;
    editor->desired_column = screen_column;
}

static void set_cursor(reed_editor_t *editor, usize position)
{
    if (!editor) return;
    if (reed_buffer_cursor_set(&editor->buffer, position))
        update_desired_column(editor);
}

static bool selection_bounds(const reed_editor_t *editor, usize *out_start,
                             usize *out_end)
{
    usize cursor;
    usize start;
    usize end;

    if (!editor || editor->selection == REED_SELECTION_NONE || !out_start ||
        !out_end) return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    if (editor->selection == REED_SELECTION_CHARACTER) {
        *out_start = editor->selection_anchor < cursor
            ? editor->selection_anchor : cursor;
        *out_end = editor->selection_anchor > cursor
            ? editor->selection_anchor : cursor;
        if (editor->selection_endpoint_inclusive &&
            cursor > editor->selection_anchor) {
            usize after_cursor = reed_buffer_next_boundary(&editor->buffer,
                                                           cursor);
            if (after_cursor > *out_end) *out_end = after_cursor;
        }
        return true;
    }
    {
        usize last_end;
        usize anchor_line = reed_buffer_line_at(&editor->buffer,
                                                editor->selection_anchor);
        usize cursor_line = reed_buffer_line_at(&editor->buffer, cursor);
        usize first = anchor_line < cursor_line ? anchor_line : cursor_line;
        usize last = anchor_line > cursor_line ? anchor_line : cursor_line;

        if (!reed_buffer_line_bounds(&editor->buffer, first, &start, &end) ||
            !reed_buffer_line_bounds(&editor->buffer, last, &end, &last_end))
            return false;
        /* Remove the following separator when available. For a selection that
         * reaches the final logical line, consume its preceding separator
         * instead so deleting it does not leave a synthetic blank line. */
        end = last_end;
        if (end < reed_buffer_length(&editor->buffer)) end++;
        else if (start && last == reed_buffer_line_count(&editor->buffer) - 1U)
            start = reed_buffer_previous_boundary(&editor->buffer, start);
        *out_start = start;
        *out_end = end;
        return true;
    }
}

static void clear_selection(reed_editor_t *editor)
{
    if (!editor) return;
    editor->selection = REED_SELECTION_NONE;
    editor->selection_endpoint_inclusive = false;
    editor->mode = REED_MODE_NORMAL;
}

static bool delete_selection(reed_editor_t *editor)
{
    usize start;
    usize end;

    if (!editor || !selection_bounds(editor, &start, &end) || end <= start)
        return false;
    if (!reed_buffer_record_edit(&editor->buffer)) {
        set_status(editor, "Edit history is full.");
        return false;
    }
    if (!reed_buffer_delete(&editor->buffer, start, end - start)) {
        /* All bounds are checked before the edit snapshot; a delete failure is
         * an invariant failure, so retain the selection and report it. */
        set_status(editor, "Selected text could not be deleted.");
        return false;
    }
    clear_selection(editor);
    set_cursor(editor, start);
    set_status(editor, "");
    return true;
}

static void move_vertical(reed_editor_t *editor, bool down)
{
    usize current_row;
    usize target_row;
    usize position;

    if (!editor) return;
    current_row = cursor_visual_row(editor, NULL);
    if (down) {
        target_row = current_row + 1U;
    } else {
        if (!current_row) return;
        target_row = current_row - 1U;
    }
    if (position_for_visual_cell(editor, target_row, editor->desired_column,
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

static void move_line_end(reed_editor_t *editor)
{
    usize line = reed_buffer_line_at(&editor->buffer,
                                     reed_buffer_cursor(&editor->buffer));
    usize start;
    usize end;

    if (reed_buffer_line_bounds(&editor->buffer, line, &start, &end))
        set_cursor(editor, end);
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

static bool search_find_closest(reed_editor_t *editor, usize anchor,
                                usize *out_match)
{
    if (!editor || !editor->search_active || !editor->search_length) return false;
    usize length = reed_buffer_length(&editor->buffer);
    if (anchor > length) anchor = length;
    usize reach = anchor > length - anchor ? anchor : length - anchor;
    /* Examine starts in increasing byte distance, later position first on a
     * tie. A nearby match does not require scanning the rest of a large file. */
    for (usize distance = 0; distance <= reach; distance++) {
        usize position;
        if (distance <= length - anchor &&
            query_matches_at(editor, position = anchor + distance)) {
            if (out_match) *out_match = position;
            return true;
        }
        if (distance && distance <= anchor &&
            query_matches_at(editor, position = anchor - distance)) {
            if (out_match) *out_match = position;
            return true;
        }
    }
    return false;
}

static bool search_find_from_cursor(reed_editor_t *editor, bool forward)
{
    usize cursor;
    usize length;

    if (!editor || !editor->search_active || !editor->search_length)
        return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    length = reed_buffer_length(&editor->buffer);
    if (forward) {
        for (usize position = cursor;
             position <= length && editor->search_length <= length - position; ) {
            if (query_matches_at(editor, position)) {
                editor->search_match = position;
                set_cursor(editor, position + editor->search_length);
                return true;
            }
            if (position == length) break;
            position = reed_buffer_next_boundary(&editor->buffer, position);
        }
    } else {
        usize position = cursor;

        while (true) {
            if (editor->search_length <= cursor - position &&
                query_matches_at(editor, position) &&
                !(position == editor->search_match &&
                  cursor == editor->search_match + editor->search_length)) {
                editor->search_match = position;
                set_cursor(editor, position + editor->search_length);
                return true;
            }
            if (!position) break;
            position = reed_buffer_previous_boundary(&editor->buffer,
                                                      position);
        }
    }
    return false;
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
    update_desired_column(editor);
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

static void finish_insert(reed_editor_t *editor)
{
    if (!editor) return;
    editor->mode = REED_MODE_NORMAL;
    editor->insert_transaction = false;
    editor->insert_recorded = false;
    update_desired_column(editor);
}

static void start_insert(reed_editor_t *editor)
{
    if (!editor) return;
    editor->mode = REED_MODE_INSERT;
    editor->insert_transaction = true;
    editor->insert_recorded = false;
    update_desired_column(editor);
}

static bool insert_newline(reed_editor_t *editor, bool above)
{
    usize line;
    usize start;
    usize end;
    usize position;
    usize original_length;
    bool need_final_newline;
    const char newline = '\n';

    if (!editor || editor->mode != REED_MODE_NORMAL ||
        editor->selection != REED_SELECTION_NONE) return false;
    line = reed_buffer_line_at(&editor->buffer,
                               reed_buffer_cursor(&editor->buffer));
    if (!reed_buffer_line_bounds(&editor->buffer, line, &start, &end))
        return false;
    original_length = reed_buffer_length(&editor->buffer);
    position = above ? start : end;
    need_final_newline = (!above && end == original_length) ||
                         (above && original_length == 0U);

    /* Grow the gap before recording the undo point. If allocation fails, the
     * buffer, cursor, mode, and history remain unchanged. */
    if (!reed_buffer_reserve_insert(&editor->buffer, position, 1U)) {
        set_status(editor, "Cannot open line: buffer full or out of memory.");
        return false;
    }
    if (!reed_buffer_record_edit(&editor->buffer)) {
        set_status(editor, "Edit history is full.");
        return false;
    }
    if (!reed_buffer_insert(&editor->buffer, position, &newline, 1U)) {
        set_status(editor, "Cannot open line: buffer state is invalid.");
        return false;
    }
    /* The text newline separates the new logical line. At EOF a second,
     * file-format terminator is needed so reloading does not consume that
     * blank line as the terminator itself. */
    if (need_final_newline &&
        !reed_buffer_set_final_newline(&editor->buffer, true)) {
        set_status(editor, "Cannot preserve opened line at document end.");
        return false;
    }
    if (above && !reed_buffer_cursor_set(&editor->buffer, position)) {
        set_status(editor, "Cannot position cursor on opened line.");
        return false;
    }
    start_insert(editor);
    editor->insert_recorded = true;
    set_status(editor, "");
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

static char prompt_conflict_choice(reed_editor_t *editor, bool can_overwrite)
{
    mg_terminal_key_event_t event;
    char choice = 'c';

    if (!editor) return choice;
    editor->prompt = REED_PROMPT_CONFLICT;
    reset_input(editor);
    if (editor_render(editor) &&
        terminal_read_key_event(MG_TERMINAL_WAIT_FOREVER, &event) == MG_OK &&
        event.keycode != HID_KEY_ESCAPE && event.keycode != HID_KEY_ENTER &&
        !has_alt(event.modifiers) && !has_control(event.modifiers)) {
        char key = (char)event.codepoint;
        if (key >= 'A' && key <= 'Z') key = (char)(key - 'A' + 'a');
        if (key == 'a' || (key == 'o' && can_overwrite)) choice = key;
    }
    editor->prompt = REED_PROMPT_NONE;
    reset_input(editor);
    return choice;
}

static bool prompt_save_path(reed_editor_t *editor, char **out_path)
{
    mg_terminal_key_event_t event;
    bool accepted = false;

    if (!editor || !out_path) return false;
    *out_path = NULL;
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
                resolve_path_alloc(editor->input, out_path);
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

static bool command_prefix_matches(const char *command, const char *prefix,
                                   usize prefix_length)
{
    if (!command || !prefix || strlen(command) < prefix_length) return false;
    for (usize index = 0; index < prefix_length; index++)
        if (lower_ascii(command[index]) != lower_ascii(prefix[index]))
            return false;
    return true;
}

static void complete_command_name(reed_editor_t *editor)
{
    static const char *const commands[] = {
        "s", "sq", "saveas", "q", "open", "find"
    };
    const char *candidate = NULL;
    usize matches = 0;

    if (!editor || editor->input_cursor != editor->input_length ||
        memchr(editor->input, ' ', editor->input_length) ||
        memchr(editor->input, '\t', editor->input_length)) return;
    for (usize index = 0; index < sizeof(commands) / sizeof(commands[0]);
         index++) {
        if (command_prefix_matches(commands[index], editor->input,
                                   editor->input_length)) {
            candidate = commands[index];
            matches++;
        }
    }
    if (matches != 1U || !candidate || strlen(candidate) <= editor->input_length)
        return;
    reset_input(editor);
    (void)input_insert_bytes(editor, candidate, strlen(candidate));
}

typedef enum {
    REED_COMMAND_UNKNOWN,
    REED_COMMAND_SAVE,
    REED_COMMAND_SAVE_QUIT,
    REED_COMMAND_QUIT,
    REED_COMMAND_SAVE_AS,
    REED_COMMAND_OPEN,
    REED_COMMAND_FIND,
} reed_command_t;

static reed_command_t parse_command_line(const char *command,
                                         const char **out_argument)
{
    const char *name;
    const char *end;
    usize name_length;

    if (out_argument) *out_argument = "";
    if (!command) return REED_COMMAND_UNKNOWN;
    name = skip_spaces(command);
    end = name;
    while (*end && *end != ' ' && *end != '\t') end++;
    name_length = (usize)(end - name);
    if (!name_length) return REED_COMMAND_UNKNOWN;
    if (out_argument) *out_argument = skip_spaces(end);
    if (text_equals_ignore_case(name, "s", name_length))
        return REED_COMMAND_SAVE;
    if (text_equals_ignore_case(name, "sq", name_length))
        return REED_COMMAND_SAVE_QUIT;
    if (text_equals_ignore_case(name, "q", name_length))
        return REED_COMMAND_QUIT;
    if (text_equals_ignore_case(name, "saveas", name_length))
        return REED_COMMAND_SAVE_AS;
    if (text_equals_ignore_case(name, "open", name_length))
        return REED_COMMAND_OPEN;
    if (text_equals_ignore_case(name, "find", name_length))
        return REED_COMMAND_FIND;
    return REED_COMMAND_UNKNOWN;
}

/* A complete snapshot binds the opened handle's current generation. Its bytes
 * are deliberately kept separate from Reed's edited buffer. */
static mg_result_t snapshot_conflict_version(reed_editor_t *editor)
{
    const usize limit = REED_MAX_BUFFER_BYTES;
    mg_path_info_t info;
    const char *path = reed_buffer_path(&editor->buffer);
    usize capacity = reed_buffer_length(&editor->buffer);
    if (path && path_info(path, &info) == MG_OK && info.size > capacity) {
        if (info.size > limit) return MG_ERR_REPLACEMENT_TOO_LARGE;
        capacity = (usize)info.size;
    }
    if (!capacity) capacity = 1U;
    mg_file_lines_state_t state;
    mg_result_t result;

    for (u32 attempt = 0;; attempt++) {
        char *data = (char *)malloc(capacity);
        if (!data) return MG_ERR_NO_MEMORY;
        result = file_snapshot(editor->file_handle, data, capacity, &state);
        free(data);
        if (result != MG_ERR_BUFFER_TOO_SMALL) return result;
        if (attempt == 3U || capacity == limit) return result;
        capacity = capacity > limit / 2U ? limit : capacity * 2U;
    }
}

static bool report_save_result(reed_editor_t *editor, mg_result_t result,
                               bool created)
{
    if (result == MG_OK) {
        editor->save_uncertain = false;
        editor->save_conflict = false;
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
            set_status(editor, "Save refused: file exceeds format/address limits; edits kept.");
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
    COMMAND_PATH_AUTO char *path = NULL;

    if (!editor || !argument || !*argument ||
        !resolve_path_alloc(argument, &path))
        return report_save_result(editor, MG_ERR_BAD_ARGUMENT, true);
    return report_save_result(editor, save_created_file(editor, path), true);
}

static bool resolve_save_conflict(reed_editor_t *editor)
{
    mg_result_t snapshot_result;
    char choice;
    COMMAND_PATH_AUTO char *path = NULL;

    editor->save_conflict = true;
    snapshot_result = snapshot_conflict_version(editor);
    if (snapshot_result == MG_OK)
        set_status(editor, "Changed: [c] Cancel [a] Save As [o] Overwrite: ");
    else if (snapshot_result == MG_ERR_BUFFER_TOO_SMALL)
        set_status(editor, "Changed during snapshot: [c] Cancel [a] Save As: ");
    else if (snapshot_result == MG_ERR_ACCESS_DENIED)
        set_status(editor, "Changed; read denied: [c] Cancel [a] Save As: ");
    else if (snapshot_result == MG_ERR_DEVICE_GONE)
        set_status(editor, "Changed; device removed: [c] Cancel [a] Save As: ");
    else if (snapshot_result == MG_ERR_NO_MEMORY)
        set_status(editor, "Changed; no snapshot memory: [c] Cancel [a] Save As: ");
    else
        set_status(editor, "Changed; snapshot failed: [c] Cancel [a] Save As: ");
    choice = prompt_conflict_choice(editor, snapshot_result == MG_OK);
    if (choice == 'a') {
        if (!prompt_save_path(editor, &path)) {
            set_status(editor, "Save As cancelled; edits kept.");
            return false;
        }
        return report_save_result(editor, save_created_file(editor, path), true);
    }
    if (choice == 'o')
        return report_save_result(editor, save_existing_file(editor), false);
    set_status(editor, "Save cancelled; external change unresolved; edits kept.");
    return false;
}

static bool save_action(reed_editor_t *editor)
{
    mg_result_t result;
    const char *path;
    COMMAND_PATH_AUTO char *prompted_path = NULL;

    if (!editor) return false;
    if (editor->save_uncertain) {
        set_status(editor, "Save blocked after uncertain outcome; use /saveas PATH or discard.");
        return false;
    }
    if (editor->existing_file) {
        if (editor->save_conflict) return resolve_save_conflict(editor);
        result = save_existing_file(editor);
        if (result == MG_ERR_STALE_GENERATION)
            return resolve_save_conflict(editor);
        return report_save_result(editor, result, false);
    }
    path = reed_buffer_path(&editor->buffer);
    if (!path) {
        if (!prompt_save_path(editor, &prompted_path)) {
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
        !editor->save_uncertain && !editor->save_conflict) {
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

static void handle_input_interrupt(reed_editor_t *editor)
{
    /* Treat Ctrl+C as a request to leave the foreground editor, but keep the
     * established save/discard/cancel protection for modified documents. */
    if (editor) (void)quit_action(editor);
}

static const char *open_error_text(mg_result_t result)
{
    switch (result) {
        case MG_ERR_BUFFER_TOO_SMALL:
            return "Open failed: file exceeds address limits.";
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
    COMMAND_PATH_AUTO char *path = NULL;
    char choice;
    mg_result_t result;

    if (!editor || !argument || !*argument ||
        !resolve_path_alloc(argument, &path)) {
        set_status(editor, "Open failed: invalid pathname.");
        return false;
    }
    if (reed_buffer_is_modified(&editor->buffer) ||
        editor->save_uncertain || editor->save_conflict) {
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

static void start_search(reed_editor_t *editor)
{
    if (!editor) return;
    editor->mode = REED_MODE_SEARCH;
    editor->prompt = REED_PROMPT_SEARCH;
    editor->search_anchor = reed_buffer_cursor(&editor->buffer);
    editor->search_active = false;
    editor->search_length = 0;
    reset_input(editor);
}

static void accept_search(reed_editor_t *editor)
{
    usize match;

    if (!editor) return;
    if (!editor->input_length) {
        set_status(editor, "Empty search.");
        editor->mode = REED_MODE_NORMAL;
        editor->prompt = REED_PROMPT_NONE;
        set_cursor(editor, editor->search_anchor);
        reset_input(editor);
        return;
    }
    memcpy(editor->search_text, editor->input, editor->input_length + 1U);
    editor->search_length = editor->input_length;
    editor->search_active = true;
    if (!search_find_closest(editor, editor->search_anchor, &match)) {
        editor->search_active = false;
        editor->search_length = 0;
        set_cursor(editor, editor->search_anchor);
        set_status(editor, "Pattern not found.");
        editor->mode = REED_MODE_NORMAL;
    } else {
        editor->search_match = match;
        set_cursor(editor, match + editor->search_length);
        set_status(editor, "");
        editor->mode = REED_MODE_SEARCH;
    }
    editor->prompt = REED_PROMPT_NONE;
    reset_input(editor);
}

static void execute_command_line(reed_editor_t *editor)
{
    char command[REED_INPUT_CAPACITY];
    const char *argument;
    reed_command_t parsed;

    if (!editor) return;
    memcpy(command, editor->input, editor->input_length + 1U);
    editor->prompt = REED_PROMPT_NONE;
    editor->mode = REED_MODE_NORMAL;
    reset_input(editor);
    parsed = parse_command_line(command, &argument);
    switch (parsed) {
        case REED_COMMAND_SAVE:
        case REED_COMMAND_SAVE_QUIT:
            if (*argument) {
                set_status(editor, "Usage: /s, /sq");
                return;
            }
            execute_save_command(editor, parsed == REED_COMMAND_SAVE_QUIT);
            return;
        case REED_COMMAND_SAVE_AS:
            if (!*argument) {
                set_status(editor, "Usage: /saveas PATH");
                return;
            }
            (void)save_as_action(editor, argument);
            return;
        case REED_COMMAND_QUIT:
            if (*argument) {
                set_status(editor, "Usage: /q");
                return;
            }
            set_status(editor, "Save changes? [s] Save [d] Don't Save [c] Cancel");
            (void)quit_action(editor);
            return;
        case REED_COMMAND_OPEN:
            if (!*argument) {
                set_status(editor, "Usage: /open PATH");
                return;
            }
            (void)open_action(editor, argument);
            return;
        case REED_COMMAND_FIND:
            if (!*argument) {
                set_status(editor, "Usage: /find TEXT");
                return;
            }
            if (strlen(argument) >= sizeof(editor->search_text)) {
                set_status(editor, "Search text is too long.");
                return;
            }
            strcpy(editor->search_text, argument);
            editor->search_length = strlen(argument);
            editor->search_active = true;
            if (!search_find_from(editor,
                                  reed_buffer_cursor(&editor->buffer), true,
                                  NULL))
                set_status(editor, "Pattern not found.");
            else set_status(editor, "");
            return;
        case REED_COMMAND_UNKNOWN:
        default:
            if (!*skip_spaces(command)) set_status(editor, "Empty command.");
            else set_status(editor, "Unknown Reed command.");
            return;
    }
}

static void cancel_prompt(reed_editor_t *editor)
{
    if (!editor) return;
    if (editor->mode == REED_MODE_SEARCH) {
        set_cursor(editor, editor->search_anchor);
        editor->search_active = false;
        editor->search_length = 0;
    }
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
    if (event->keycode == HID_KEY_TAB || event->codepoint == '\t') {
        if (editor->mode == REED_MODE_COMMAND)
            complete_command_name(editor);
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
    usize current_row;
    usize total_rows;
    usize amount;
    usize target;
    usize position;

    if (!editor) return;
    amount = editor->screen_rows > 2U ? editor->screen_rows - 2U : 1U;
    current_row = cursor_visual_row(editor, NULL);
    total_rows = visual_row_count(editor);
    if (down) {
        target = current_row + amount;
        if (target >= total_rows) target = total_rows - 1U;
    } else {
        target = current_row > amount ? current_row - amount : 0;
    }
    if (position_for_visual_cell(editor, target, editor->desired_column,
                                 &position))
        (void)reed_buffer_cursor_set(&editor->buffer, position);
}

static bool apply_navigation(reed_editor_t *editor, reed_action_t action)
{
    usize cursor;

    if (!editor) return false;
    cursor = reed_buffer_cursor(&editor->buffer);
    switch (action) {
        case REED_ACTION_MOVE_LEFT:
            set_cursor(editor, reed_buffer_previous_boundary(&editor->buffer,
                                                             cursor));
            return true;
        case REED_ACTION_MOVE_RIGHT:
            set_cursor(editor, reed_buffer_next_boundary(&editor->buffer,
                                                         cursor));
            return true;
        case REED_ACTION_MOVE_DOWN:
            move_vertical(editor, true);
            return true;
        case REED_ACTION_MOVE_UP:
            move_vertical(editor, false);
            return true;
        case REED_ACTION_LINE_START:
            move_line_start(editor);
            return true;
        case REED_ACTION_LINE_END:
            move_line_end(editor);
            return true;
        case REED_ACTION_DOCUMENT_START:
            set_cursor(editor, 0);
            return true;
        case REED_ACTION_DOCUMENT_END:
            set_cursor(editor, reed_buffer_length(&editor->buffer));
            return true;
        case REED_ACTION_WORD_PREVIOUS_END:
            set_cursor(editor, word_previous_end(&editor->buffer, cursor));
            return true;
        case REED_ACTION_WORD_CURRENT_PREVIOUS_START:
            set_cursor(editor,
                       word_current_previous_start(&editor->buffer, cursor));
            return true;
        case REED_ACTION_WORD_CURRENT_NEXT_END:
            set_cursor(editor, word_current_next_end(&editor->buffer, cursor));
            return true;
        case REED_ACTION_WORD_NEXT_START:
            set_cursor(editor, word_next_start(&editor->buffer, cursor));
            return true;
        case REED_ACTION_PARAGRAPH_PREVIOUS:
            move_paragraph_previous(editor);
            return true;
        case REED_ACTION_PARAGRAPH_NEXT:
            move_paragraph_next(editor);
            return true;
        case REED_ACTION_PAGE_UP:
            action_page(editor, false);
            return true;
        case REED_ACTION_PAGE_DOWN:
            action_page(editor, true);
            return true;
        default:
            return false;
    }
}

static void begin_selection(reed_editor_t *editor, reed_selection_t kind)
{
    if (!editor) return;
    editor->selection = kind;
    editor->selection_anchor = reed_buffer_cursor(&editor->buffer);
    editor->selection_endpoint_inclusive = false;
    editor->mode = REED_MODE_SELECTION;
    set_status(editor, "");
}

static void select_all(reed_editor_t *editor)
{
    if (!editor) return;
    editor->selection = REED_SELECTION_CHARACTER;
    editor->selection_anchor = 0;
    editor->selection_endpoint_inclusive = false;
    editor->mode = REED_MODE_SELECTION;
    set_cursor(editor, reed_buffer_length(&editor->buffer));
    set_status(editor, "");
}

static bool clipboard_copy_range(reed_editor_t *editor, usize start, usize end,
                                  bool cut)
{
    usize length = end - start;
    /* Stream directly out of the gap buffer. There is no second complete
     * clipboard copy in the source editor. */
    reed_buffer_edit_t edit = {0};
    if (cut && length && !reed_buffer_prepare_edit(&editor->buffer, start,
                                                   length, NULL, 0, &edit)) {
        set_status(editor, "Cannot prepare cut; document unchanged.");
        return false;
    }
    mg_clipboard_transfer_t transfer;
    mg_result_t result = clipboard_publish_begin(MG_CLIPBOARD_TEXT, length, &transfer);
    if (result == MG_OK) {
        char bytes[MG_CLIPBOARD_CHUNK];
        for (usize offset = 0; offset < length; ) {
            usize amount = length - offset;
            if (amount > sizeof(bytes)) amount = sizeof(bytes);
            if (!reed_buffer_copy_slice(&editor->buffer, start + offset, bytes, amount)) {
                result = MG_ERR_BAD_ARGUMENT;
                break;
            }
            result = clipboard_write(&transfer, bytes, amount);
            if (result != MG_OK) break;
            offset += amount;
        }
        if (result == MG_OK) result = clipboard_publish_commit(&transfer);
        else (void)clipboard_transfer_close(&transfer);
    }
    if (result != MG_OK) {
        reed_buffer_discard_edit(&edit);
        set_status(editor, "Clipboard copy failed; document unchanged.");
        return false;
    }
    if (cut && length) reed_buffer_commit_edit(&edit);
    if (editor->selection != REED_SELECTION_NONE) clear_selection(editor);
    if (cut && length) set_cursor(editor, start);
    set_status(editor, cut ? "Cut to clipboard." : "Copied to clipboard.");
    return true;
}

static bool clipboard_paste(reed_editor_t *editor)
{
    usize start = reed_buffer_cursor(&editor->buffer), end = start;
    if (editor->selection != REED_SELECTION_NONE &&
        !selection_bounds(editor, &start, &end)) return false;
    usize limit = REED_MAX_BUFFER_BYTES -
        (reed_buffer_length(&editor->buffer) - (end - start));
    char *text;
    usize length;
    mg_result_t result = clipboard_get_text(limit, &text, &length);
    if (result != MG_OK) {
        set_status(editor, "Clipboard paste unavailable; document unchanged.");
        return false;
    }
    if (!length && start == end) {
        free(text);
        clear_selection(editor);
        set_status(editor, "Clipboard is empty.");
        return true;
    }
    if (memchr(text, 0, length)) {
        free(text);
        set_status(editor, "Clipboard contains NUL; document unchanged.");
        return false;
    }
    reed_buffer_edit_t edit = {0};
    if (!reed_buffer_prepare_edit(&editor->buffer, start, end - start,
                                  text, length, &edit)) {
        free(text);
        set_status(editor, "Cannot prepare paste; document unchanged.");
        return false;
    }
    reed_buffer_commit_edit(&edit);
    free(text);
    clear_selection(editor);
    update_desired_column(editor);
    set_status(editor, "Pasted from clipboard.");
    return true;
}

static bool clipboard_motion(reed_editor_t *editor, reed_action_t action,
                              usize *start, usize *end)
{
    usize cursor = reed_buffer_cursor(&editor->buffer);
    usize column = editor->desired_column;
    switch (action) {
        case REED_ACTION_WORD_PREVIOUS_END:
        case REED_ACTION_WORD_CURRENT_PREVIOUS_START:
        case REED_ACTION_WORD_CURRENT_NEXT_END:
        case REED_ACTION_WORD_NEXT_START:
        case REED_ACTION_LINE_START:
        case REED_ACTION_LINE_END:
        case REED_ACTION_DOCUMENT_START:
        case REED_ACTION_DOCUMENT_END:
        case REED_ACTION_PARAGRAPH_PREVIOUS:
        case REED_ACTION_PARAGRAPH_NEXT: break;
        default: return false;
    }
    (void)apply_navigation(editor, action);
    usize target = reed_buffer_cursor(&editor->buffer);
    (void)reed_buffer_cursor_set(&editor->buffer, cursor);
    editor->desired_column = column;
    *start = cursor < target ? cursor : target;
    *end = cursor > target ? cursor : target;
    if (target > cursor && action == REED_ACTION_WORD_CURRENT_NEXT_END)
        *end = reed_buffer_next_boundary(&editor->buffer, target);
    return true;
}

static void clipboard_operator_event(reed_editor_t *editor,
                                      const mg_terminal_key_event_t *event,
                                      reed_action_t action)
{
    reed_action_t operation = editor->clipboard_operator;
    if (action == operation && (event->flags & MG_TERMINAL_KEY_EVENT_REPEAT)) return;
    editor->clipboard_operator = REED_ACTION_NONE;
    if (event->keycode == HID_KEY_ESCAPE) { set_status(editor, "Cancelled."); return; }
    usize start, end;
    if (action == operation) {
        reed_selection_t selection = editor->selection;
        usize anchor = editor->selection_anchor;
        editor->selection = REED_SELECTION_LINE;
        editor->selection_anchor = reed_buffer_cursor(&editor->buffer);
        bool valid = selection_bounds(editor, &start, &end);
        editor->selection = selection;
        editor->selection_anchor = anchor;
        if (!valid) return;
    } else if (!clipboard_motion(editor, action, &start, &end)) {
        set_status(editor, "Clipboard motion expected; cancelled.");
        return;
    }
    (void)clipboard_copy_range(editor, start, end, operation == REED_ACTION_CUT);
}

static void dispatch_normal(reed_editor_t *editor,
                            const mg_terminal_key_event_t *event)
{
    reed_action_t action;

    if (!editor || !event) return;
    action = normal_action(event);
    if (editor->clipboard_operator != REED_ACTION_NONE) {
        clipboard_operator_event(editor, event, action);
        return;
    }
    if (event->keycode == HID_KEY_ESCAPE) return;
    if (apply_navigation(editor, action)) return;
    switch (action) {
        case REED_ACTION_COPY:
        case REED_ACTION_CUT:
            editor->clipboard_operator = action;
            set_status(editor, "Clipboard: motion or repeat operator for line; Escape cancels.");
            break;
        case REED_ACTION_PASTE:
            (void)clipboard_paste(editor);
            break;
        case REED_ACTION_LINE_START_INSERT:
            move_line_start(editor);
            start_insert(editor);
            break;
        case REED_ACTION_LINE_END_INSERT:
            move_line_end(editor);
            start_insert(editor);
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
        case REED_ACTION_SELECT_CHARACTER:
            begin_selection(editor, REED_SELECTION_CHARACTER);
            break;
        case REED_ACTION_SELECT_LINE:
            begin_selection(editor, REED_SELECTION_LINE);
            break;
        case REED_ACTION_SELECT_ALL:
            select_all(editor);
            break;
        case REED_ACTION_DELETE_CHAR:
        case REED_ACTION_DELETE_RIGHT:
            (void)delete_right(editor);
            break;
        case REED_ACTION_DELETE_LEFT:
            (void)delete_left(editor);
            break;
        case REED_ACTION_UNDO:
            if (!reed_buffer_undo(&editor->buffer))
                set_status(editor, "Nothing to undo.");
            update_desired_column(editor);
            break;
        case REED_ACTION_REDO:
            if (!reed_buffer_redo(&editor->buffer))
                set_status(editor, "Nothing to redo.");
            update_desired_column(editor);
            break;
        case REED_ACTION_COMMAND:
            editor->mode = REED_MODE_COMMAND;
            editor->prompt = REED_PROMPT_COMMAND;
            reset_input(editor);
            break;
        case REED_ACTION_SEARCH_PROMPT:
            start_search(editor);
            break;
        case REED_ACTION_NONE:
        default:
            break;
    }
}

static void dispatch_selection(reed_editor_t *editor,
                               const mg_terminal_key_event_t *event)
{
    reed_action_t action;

    if (!editor || !event) return;
    if (event->keycode == HID_KEY_ESCAPE) {
        clear_selection(editor);
        set_status(editor, "");
        return;
    }
    action = normal_action(event);
    if (action == REED_ACTION_COPY || action == REED_ACTION_CUT) {
        usize start, end;
        if (selection_bounds(editor, &start, &end))
            (void)clipboard_copy_range(editor, start, end, action == REED_ACTION_CUT);
        return;
    }
    if (action == REED_ACTION_PASTE) { (void)clipboard_paste(editor); return; }
    if (action == REED_ACTION_SELECT_CHARACTER ||
        action == REED_ACTION_SELECT_LINE) {
        reed_selection_t requested = action == REED_ACTION_SELECT_CHARACTER
            ? REED_SELECTION_CHARACTER : REED_SELECTION_LINE;

        if (editor->selection == requested) clear_selection(editor);
        else editor->selection = requested;
        return;
    }
    if (action == REED_ACTION_DELETE_CHAR) {
        usize start;
        usize end;

        if (selection_bounds(editor, &start, &end) && end <= start) {
            clear_selection(editor);
            return;
        }
        if (!delete_selection(editor)) {
            return;
        }
        return;
    }
    if (action == REED_ACTION_LINE_START_INSERT) {
        usize before = reed_buffer_cursor(&editor->buffer);

        move_line_start(editor);
        if (reed_buffer_cursor(&editor->buffer) != before)
            editor->selection_endpoint_inclusive = false;
        return;
    }
    if (action == REED_ACTION_LINE_END_INSERT) {
        usize before = reed_buffer_cursor(&editor->buffer);

        move_line_end(editor);
        if (reed_buffer_cursor(&editor->buffer) != before)
            editor->selection_endpoint_inclusive = false;
        return;
    }
    {
        usize before = reed_buffer_cursor(&editor->buffer);

        if (apply_navigation(editor, action)) {
            usize after = reed_buffer_cursor(&editor->buffer);

            if (after != before)
                editor->selection_endpoint_inclusive =
                    action == REED_ACTION_WORD_PREVIOUS_END ||
                    action == REED_ACTION_WORD_CURRENT_NEXT_END;
        }
    }
}

static void dispatch_search(reed_editor_t *editor,
                            const mg_terminal_key_event_t *event)
{
    reed_action_t action;

    if (!editor || !event) return;
    if (event->keycode == HID_KEY_ESCAPE) action = REED_ACTION_SEARCH_PROMPT;
    else action = normal_action(event);
    if (action == REED_ACTION_SEARCH_PROMPT) {
        editor->search_active = false;
        editor->search_length = 0;
        editor->mode = REED_MODE_NORMAL;
        editor->prompt = REED_PROMPT_NONE;
        set_status(editor, "");
        return;
    }
    if (event->keycode == HID_KEY_N && !has_alt(event->modifiers) &&
        !has_control(event->modifiers) &&
        !(event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_GUI |
                              MG_TERMINAL_MODIFIER_RIGHT_GUI)) &&
        !has_shift(event->modifiers))
        action = REED_ACTION_SEARCH_NEXT;
    else if (event->keycode == HID_KEY_B && !has_alt(event->modifiers) &&
             !has_control(event->modifiers) &&
             !(event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_GUI |
                                   MG_TERMINAL_MODIFIER_RIGHT_GUI)) &&
             !has_shift(event->modifiers))
        action = REED_ACTION_SEARCH_PREVIOUS;
    if (action == REED_ACTION_SEARCH_NEXT ||
        action == REED_ACTION_SEARCH_PREVIOUS) {
        if (!search_find_from_cursor(editor,
                                     action == REED_ACTION_SEARCH_NEXT))
            set_status(editor, "No further match.");
        else
            set_status(editor, "");
        return;
    }
    (void)apply_navigation(editor, action);
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
        move_line_end(editor);
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

#if defined(REED_GUEST_CONFLICT_SYNC)
static bool wait_for_guest_conflict_release(void)
{
    char ready_path[64];
    char release_path[64];
    mg_handle_t marker = 0;
    mg_monotonic_time_t start;
    mg_monotonic_time_t now;
    int length;

    if (!reed_conflict_sync_run[0]) return true;
    length = snprintf(ready_path, sizeof(ready_path),
                      "/temp/reed-open-%s.ready", reed_conflict_sync_run);
    if (length < 0 || (usize)length >= sizeof(ready_path)) return false;
    length = snprintf(release_path, sizeof(release_path),
                      "/temp/reed-open-%s.continue", reed_conflict_sync_run);
    if (length < 0 || (usize)length >= sizeof(release_path)) return false;
    if (file_create_contents(ready_path, "opened\n", 7U, &marker) != MG_OK ||
        !marker) {
        if (marker) (void)handle_close(marker);
        return false;
    }
    if (handle_close(marker) != MG_OK) return false;
    if (mg_clock_monotonic(&start) != MG_OK) return false;
    for (;;) {
        mg_result_t opened = file_open(release_path, MG_OPEN_READ);

        if (opened > 0) {
            char contents[16];
            mg_file_lines_state_t state = {0};
            mg_result_t snapshot = file_snapshot((mg_handle_t)opened,
                                                  contents, sizeof(contents),
                                                  &state);
            mg_result_t closed = handle_close((mg_handle_t)opened);

            return snapshot == MG_OK && closed == MG_OK &&
                   state.length == 9U &&
                   memcmp(contents, "continue\n", 9U) == 0;
        }
        if (opened != MG_ERR_NOT_FOUND || mg_clock_monotonic(&now) != MG_OK ||
            now.milliseconds < start.milliseconds ||
            now.milliseconds - start.milliseconds > 30000U ||
            process_sleep_ms(10U) != MG_OK) return false;
    }
}
#endif

static bool editor_init(reed_editor_t *editor, const char *pathname)
{
    COMMAND_PATH_AUTO char *path = NULL;
    mg_result_t result;

    if (!editor || !reed_buffer_init(&editor->buffer)) return false;
    memset((char *)editor + sizeof(editor->buffer), 0,
           sizeof(*editor) - sizeof(editor->buffer));
    reed_config_defaults(&editor->config);
    editor->mode = REED_MODE_NORMAL;
    editor->running = true;
    editor->screen_rows = 24U;
    editor->screen_columns = 80U;
    if (pathname) {
        if (!resolve_path_alloc(pathname, &path)) {
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
#if defined(REED_GUEST_CONFLICT_SYNC)
        if (!wait_for_guest_conflict_release()) {
            set_status(editor, "test conflict synchronization failed.");
            if (editor->existing_file) (void)handle_close(editor->file_handle);
            reed_buffer_destroy(&editor->buffer);
            return false;
        }
#endif
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
    char config_diagnostic[REED_STATUS_CAPACITY];

    editor = (reed_editor_t *)calloc(1, sizeof(*editor));
    if (!editor || !editor_init(editor, pathname)) {
        const char *error = editor && editor->status[0]
            ? editor->status : "could not open buffer.";
        printf("reed: %s\n", error);
        free(editor);
        return 1;
    }
    result = reed_config_load(&editor->config, config_diagnostic,
                              sizeof(config_diagnostic));
    if (result != MG_OK && config_diagnostic[0])
        set_status(editor, config_diagnostic);
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
            if (result == MG_ERR_INTERRUPTED) {
                handle_input_interrupt(editor);
                continue;
            }
            exit_status = 1;
            break;
        }
        if (event.version != MG_TERMINAL_API_VERSION ||
            (event.flags & ~MG_TERMINAL_KEY_EVENT_REPEAT) != 0) {
            set_status(editor, "Invalid keyboard event.");
            continue;
        }
#if defined(REED_GUEST_CONFLICT_SYNC)
        reed_action_t diagnostic_action = REED_ACTION_NONE;
        bool diagnostic_u = reed_conflict_sync_run[0] &&
                            editor->prompt == REED_PROMPT_NONE &&
                            editor->mode == REED_MODE_NORMAL &&
                            event.keycode == HID_KEY_U;
        if (diagnostic_u) diagnostic_action = normal_action(&event);
#endif
        if (editor->prompt != REED_PROMPT_NONE)
            handle_prompt_event(editor, &event);
        else if (editor->mode == REED_MODE_INSERT)
            dispatch_insert(editor, &event);
        else if (editor->mode == REED_MODE_SELECTION)
            dispatch_selection(editor, &event);
        else if (editor->mode == REED_MODE_SEARCH)
            dispatch_search(editor, &event);
        else
            dispatch_normal(editor, &event);
#if defined(REED_GUEST_CONFLICT_SYNC)
        if (diagnostic_u) {
            const char *action_name = diagnostic_action == REED_ACTION_REDO
                ? "redo" : diagnostic_action == REED_ACTION_UNDO
                    ? "undo" : "none";
            char diagnostic[REED_STATUS_CAPACITY];
            (void)snprintf(diagnostic, sizeof(diagnostic),
                           "test input U modifiers=%u action=%s bytes=%zu",
                           event.modifiers, action_name,
                           reed_buffer_length(&editor->buffer));
            set_status(editor, diagnostic);
        }
#endif
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
    usize query_length;
    usize candidate;

    if (out_current) *out_current = false;
    if (!editor) return false;
    query_length = editor->prompt == REED_PROMPT_SEARCH
        ? editor->input_length : editor->search_length;
    if (!query_length ||
        (editor->prompt != REED_PROMPT_SEARCH && !editor->search_active))
        return false;
    candidate = position;
    for (;;) {
        if (position - candidate < query_length &&
            query_matches_at(editor, candidate)) {
            if (out_current && editor->prompt != REED_PROMPT_SEARCH)
                *out_current = candidate == editor->search_match;
            return true;
        }
        if (!candidate || position - candidate >= query_length) break;
        candidate = reed_buffer_previous_boundary(&editor->buffer, candidate);
    }
    return false;
}

static bool selection_contains_line(const reed_editor_t *editor, usize line)
{
    usize anchor_line;
    usize cursor_line;

    if (!editor || editor->selection != REED_SELECTION_LINE) return false;
    anchor_line = reed_buffer_line_at(&editor->buffer,
                                      editor->selection_anchor);
    cursor_line = reed_buffer_line_at(&editor->buffer,
                                      reed_buffer_cursor(&editor->buffer));
    return line >= (anchor_line < cursor_line ? anchor_line : cursor_line) &&
           line <= (anchor_line > cursor_line ? anchor_line : cursor_line);
}

static bool decode_frame_text(const char *text, usize length, usize position,
                              u32 *out_codepoint, usize *out_next)
{
    usize consumed;
    u32 scalar;
    if (!text || position >= length ||
        mg_utf8_decode_one((const u8 *)text + position, length - position,
                           &scalar, &consumed) != MG_UTF8_OK || !consumed)
        return false;
    if (out_codepoint) *out_codepoint = scalar;
    if (out_next) *out_next = position + consumed;
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

static bool render_span_visual_row_to_frame(const reed_editor_t *editor,
                                           usize line, usize start, usize end,
                                           usize visual_subrow,
                                           u32 row, u32 columns,
                                           reed_frame_cell_t *frame)
{
    usize position;
    usize column = 0;
    usize first_column;
    usize selection_start = 0;
    usize selection_end = 0;
    bool character_selection;
    bool line_selection;

    if (!editor || !frame || start > end ||
        end > reed_buffer_length(&editor->buffer))
        return false;
    first_column = editor->config.wrap
        ? visual_subrow * columns : editor->horizontal_column;
    character_selection = editor->selection == REED_SELECTION_CHARACTER &&
                          selection_bounds(editor, &selection_start,
                                           &selection_end);
    line_selection = selection_contains_line(editor, line);
    if (line_selection) {
        for (u32 cell = 0; cell < columns; cell++)
            frame_put_cell(frame, columns, row, cell, ' ',
                           MG_TERMINAL_COLOR_WHITE,
                           MG_TERMINAL_COLOR_BLUE);
    }
    position = start;
    layout_seek(editor, line, first_column, false, &position, &column);
    while (position < end &&
           (column < first_column || column - first_column < columns)) {
        u32 codepoint;
        usize next;
        bool current_match = false;
        bool match;
        bool selected;
        mg_terminal_color_t foreground;
        mg_terminal_color_t background;

        if (!reed_buffer_codepoint(&editor->buffer, position, &codepoint,
                                   &next) || next > end) return false;
        match = position_in_search_match(editor, position, &current_match);
        selected = line_selection ||
                   (character_selection && position >= selection_start &&
                    position < selection_end);
        foreground = selected ? MG_TERMINAL_COLOR_WHITE :
                     match ? MG_TERMINAL_COLOR_BLACK
                           : MG_TERMINAL_COLOR_LIGHT_GRAY;
        background = selected ? MG_TERMINAL_COLOR_BLUE :
                     match ? (current_match
                                  ? MG_TERMINAL_COLOR_BRIGHT_YELLOW
                                  : MG_TERMINAL_COLOR_CYAN)
                           : MG_TERMINAL_COLOR_BLACK;
        {
            usize cells = codepoint == '\t' ? tab_cells(editor, column) : 1U;
            u32 display_codepoint = codepoint < 0x20U ? ' ' : codepoint;

            for (usize cell = 0; cell < cells; cell++) {
                usize display_column = column + cell;

                if (display_column >= first_column &&
                    display_column - first_column < columns)
                    frame_put_cell(frame, columns, row,
                                   (u32)(display_column - first_column),
                                   codepoint == '\t' ? ' ' : display_codepoint,
                                   foreground, background);
            }
            column += cells;
        }
        position = next;
    }
    if (character_selection && end < reed_buffer_length(&editor->buffer) &&
        selection_start <= end && selection_end > end &&
        column >= first_column && column - first_column < columns)
        frame_put_cell(frame, columns, row,
                       (u32)(column - first_column), ' ',
                       MG_TERMINAL_COLOR_WHITE, MG_TERMINAL_COLOR_BLUE);
    return true;
}

static bool render_document_to_frame(reed_editor_t *editor, u32 rows,
                                     u32 columns)
{
    usize source_line;
    usize source_subrow;
    usize source_start;
    usize source_end;
    usize source_rows;

    if (!editor || !editor->frame || !rows || !columns) return false;
    frame_fill(editor->frame, rows, columns, MG_TERMINAL_COLOR_LIGHT_GRAY,
               MG_TERMINAL_COLOR_BLACK);
    if (!visual_row_location(editor, editor->top_visual_row, &source_line,
                             &source_subrow, &source_start, &source_end,
                             &source_rows)) return true;
    for (u32 row = 0; row < rows; row++) {
        if (!render_span_visual_row_to_frame(editor, source_line, source_start,
                source_end, editor->config.wrap ? source_subrow : 0U,
                row, columns, editor->frame)) return false;
        source_subrow++;
        if (source_subrow >= source_rows) {
            usize source_width;

            if (source_end == reed_buffer_length(&editor->buffer)) break;
            source_line++;
            source_subrow = 0;
            source_start = source_end + 1U;
            if (!layout_line_span(editor, source_start, &source_end,
                                  &source_width)) return false;
            source_rows = layout_rows_for_width(editor, source_width);
        }
    }
    return true;
}

static usize adjust_view(reed_editor_t *editor, usize *out_column)
{
    usize column;
    usize visual_row;
    usize visible_rows;
    usize screen_column;

    if (!editor) return 0;
    visible_rows = editor->screen_rows > 1U ? editor->screen_rows - 1U : 1U;
    visual_row = cursor_visual_row(editor, &screen_column);
    if (visual_row < editor->top_visual_row)
        editor->top_visual_row = visual_row;
    if (visual_row >= editor->top_visual_row + visible_rows)
        editor->top_visual_row = visual_row - visible_rows + 1U;
    if (editor->config.wrap) {
        editor->horizontal_column = 0;
        if (out_column) *out_column = screen_column;
        return visual_row;
    }
    column = line_display_column(editor,
        reed_buffer_line_at(&editor->buffer,
                            reed_buffer_cursor(&editor->buffer)),
        reed_buffer_cursor(&editor->buffer));
    if (column < editor->horizontal_column)
        editor->horizontal_column = column;
    if (column >= editor->horizontal_column + editor->screen_columns)
        editor->horizontal_column = column - editor->screen_columns + 1U;
    if (out_column) *out_column = column - editor->horizontal_column;
    return visual_row;
}

static void frame_build_status(reed_editor_t *editor, u32 row, u32 columns,
                               usize visual_row, usize screen_column,
                               u32 *out_cursor_row,
                               u32 *out_cursor_column)
{
    char status[REED_STATUS_CAPACITY];
    const char *path;
    const char *status_mode;
    char selection_mode[16];
    int status_length;
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
            case REED_PROMPT_SEARCH: prefix = "Find: "; break;
            case REED_PROMPT_CONFIRM:
                prefix = "Save changes? [s] Save [d] Don't Save [c] Cancel: ";
                break;
            case REED_PROMPT_CONFLICT: prefix = editor->status; break;
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
    if (editor->mode == REED_MODE_SELECTION) {
        (void)snprintf(selection_mode, sizeof(selection_mode), "SELECT-%s",
                       editor->selection == REED_SELECTION_LINE
                           ? "LINE" : "CHAR");
        status_mode = selection_mode;
    }
    status_length = snprintf(status, sizeof(status), "%s  %s%s  %llu:%llu%s",
                             status_mode, path ? path : "[unnamed]",
                             editor->save_uncertain ? " [?]" :
                             editor->save_conflict ? " [!]" :
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
    *out_cursor_column = (u32)screen_column;
    *out_cursor_row = visual_row >= editor->top_visual_row
        ? (u32)(visual_row - editor->top_visual_row) : 0;
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
    usize visual_row;
    usize screen_column;

    if (!editor || !editor->frame || !editor->presented_frame ||
        terminal_get_size(&size) != MG_OK || size.rows < 2U ||
        !size.columns || size.columns > REED_MAX_SCREEN_COLUMNS ||
        size.rows > REED_MAX_SCREEN_ROWS) return false;
    editor->screen_rows = size.rows;
    editor->screen_columns = size.columns;
    visual_row = adjust_view(editor, &screen_column);
    if (!render_document_to_frame(editor, size.rows - 1U, size.columns))
        return false;
    frame_build_status(editor, size.rows - 1U, size.columns,
                       visual_row, screen_column,
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
