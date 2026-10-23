/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/line_editor.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <mg/utf8.h>
#include <string.h>

static mg_result_t write_bytes(const void *buffer, usize length)
{
    return object_write_all(MG_STDOUT_HANDLE, buffer, length);
}

static mg_result_t write_byte(char byte)
{
    return write_bytes(&byte, 1);
}

static mg_result_t read_byte(char *out)
{
    mg_result_t result;

    if (!out) return MG_ERR_BAD_ARGUMENT;
    result = object_read(MG_STDIN_HANDLE, out, 1);
    if (result < 0) return result;
    return result == 1 ? MG_OK : MG_ERR_END_OF_FILE;
}

static mg_result_t read_editor_byte(mg_line_editor_t *editor, char *out)
{
    mg_terminal_key_event_t event;
    mg_result_t result;

    if (!editor || !out) return MG_ERR_BAD_ARGUMENT;
    if (!editor->key_event_input) return read_byte(out);
    result = terminal_read_key_event(MG_TERMINAL_WAIT_FOREVER, &event);
    if (result < 0) return result;
    if (event.flags & MG_TERMINAL_KEY_EVENT_COOKED_BYTE) {
        *out = (char)(u8)event.codepoint;
        return MG_OK;
    }
    editor->pending_key_event = event;
    editor->pending_key_event_valid = true;
    return MG_ERR_WOULD_BLOCK;
}

static bool previous_scalar(const mg_line_editor_t *editor, usize position,
                            usize *out_start, u32 *out_scalar)
{
    usize start;
    if (!editor || !position || position > editor->length || !out_start ||
        !out_scalar) return false;
    start = position > 4U ? position - 4U : 0U;
    for (usize candidate = position; candidate > start; candidate--) {
        usize consumed = 0;
        u32 scalar;
        if (mg_utf8_decode_one((const u8 *)editor->buffer + candidate - 1U,
                               position - (candidate - 1U), &scalar,
                               &consumed) == MG_UTF8_OK && consumed ==
                                   position - (candidate - 1U)) {
            *out_start = candidate - 1U;
            *out_scalar = scalar;
            return true;
        }
    }
    *out_start = position - 1U;
    *out_scalar = 0xfffdU;
    return true;
}

static usize next_scalar_position(const mg_line_editor_t *editor,
                                  usize position)
{
    usize consumed = 0;
    u32 scalar;
    if (!editor || position >= editor->length) return position;
    if (mg_utf8_decode_one((const u8 *)editor->buffer + position,
                           editor->length - position, &scalar, &consumed) !=
        MG_UTF8_OK || !consumed)
        consumed = 1U;
    return position + consumed;
}

static bool scalar_is_word(u32 scalar)
{
    return scalar >= 0x80U ||
        (scalar >= 'a' && scalar <= 'z') ||
        (scalar >= 'A' && scalar <= 'Z') ||
        (scalar >= '0' && scalar <= '9') || scalar == '_';
}

static usize word_left(const mg_line_editor_t *editor, usize position)
{
    usize previous_start;
    usize next_end;
    u32 previous, next;
    bool inside_word = false;

    if (position < editor->length &&
        previous_scalar(editor, position, &previous_start, &previous) &&
        scalar_is_word(previous) &&
        mg_utf8_decode_one((const u8 *)editor->buffer + position,
                           editor->length - position, &next, &next_end) ==
            MG_UTF8_OK && scalar_is_word(next))
        inside_word = true;

    /* If the cursor is strictly inside a word, first cross to that word's
     * start, then continue to a different word in the requested direction. */
    if (inside_word) {
        while (previous_scalar(editor, position, &previous_start, &previous) &&
               scalar_is_word(previous))
            position = previous_start;
    }

    while (previous_scalar(editor, position, &previous_start, &previous) &&
           !scalar_is_word(previous))
        position = previous_start;
    if (previous_scalar(editor, position, &previous_start, &previous) &&
        scalar_is_word(previous)) {
        do {
            position = previous_start;
        } while (previous_scalar(editor, position, &previous_start, &previous) &&
                 scalar_is_word(previous));
    }
    return position;
}

static usize word_right(const mg_line_editor_t *editor, usize position)
{
    usize previous_start;
    usize next_end = 0;
    u32 previous, scalar;
    bool inside_word = false;

    if (position < editor->length &&
        previous_scalar(editor, position, &previous_start, &previous) &&
        scalar_is_word(previous) &&
        mg_utf8_decode_one((const u8 *)editor->buffer + position,
                           editor->length - position, &scalar, &next_end) ==
            MG_UTF8_OK && scalar_is_word(scalar))
        inside_word = true;

    if (inside_word) {
        do {
            position += next_end;
            if (position == editor->length) break;
        } while (mg_utf8_decode_one((const u8 *)editor->buffer + position,
                                    editor->length - position, &scalar,
                                    &next_end) == MG_UTF8_OK &&
                 scalar_is_word(scalar));
    }

    while (position < editor->length) {
        if (mg_utf8_decode_one((const u8 *)editor->buffer + position,
                               editor->length - position, &scalar,
                               &next_end) !=
            MG_UTF8_OK) {
            scalar = 0xfffdU;
            next_end = 1U;
        }
        if (scalar_is_word(scalar)) {
            do {
                position += next_end;
                if (position == editor->length) break;
            } while (mg_utf8_decode_one(
                         (const u8 *)editor->buffer + position,
                         editor->length - position, &scalar, &next_end) ==
                         MG_UTF8_OK && scalar_is_word(scalar));
            break;
        }
        position += next_end;
    }
    return position;
}

static mg_result_t write_exact(const void *bytes, usize length)
{
    mg_result_t result;

    if (!length) return MG_OK;
    result = write_bytes(bytes, length);
    if (result < 0) return result;
    return (usize)result == length ? MG_OK : MG_ERR_IO;
}

static mg_result_t write_input_suffix(const mg_line_editor_t *editor,
                                      usize start)
{
    return write_exact(editor->buffer + start, editor->length - start);
}

static mg_result_t clear_rendered_tail(usize first, usize last)
{
    static const char spaces[] =
        "                                                                ";
    while (first < last) {
        usize count = last - first;
        if (count > sizeof(spaces) - 1U) count = sizeof(spaces) - 1U;
        mg_result_t result = write_exact(spaces, count);
        if (result < 0) return result;
        first += count;
    }
    return MG_OK;
}

static mg_result_t sync_cursor_visibility(mg_line_editor_t *editor)
{
    mg_result_t result;

    if (!editor) return MG_ERR_BAD_ARGUMENT;
    if (!editor->cursor_needs_sync) return MG_OK;
    result = terminal_cursor_set_visible(true);
    if (result == MG_OK) editor->cursor_needs_sync = false;
    return result;
}

void line_editor_set_temporary_display(mg_line_editor_t *editor, bool active)
{
    if (editor) editor->temporary_display_active = active;
}

mg_result_t line_editor_clear_temporary_display(mg_line_editor_t *editor)
{
    mg_result_t result;

    if (!editor) return MG_ERR_BAD_ARGUMENT;
    if (!editor->temporary_display_active) return MG_OK;
    result = terminal_overlay_clear();
    if (result == MG_OK) {
        editor->temporary_display_active = false;
        editor->cursor_needs_sync = true;
    }
    return result;
}

static usize display_cells(const char *text, usize length)
{
    usize cells = 0, offset = 0;
    while (text && offset < length) {
        usize consumed = 0;
        u32 scalar;
        if (mg_utf8_decode_one((const u8 *)text + offset, length - offset,
                               &scalar, &consumed) != MG_UTF8_OK || !consumed)
            consumed = 1U;
        offset += consumed;
        cells++;
    }
    return cells;
}

static usize utf8_valid_prefix(const char *text, usize length)
{
    usize offset = 0;
    if (!text && length) return 0;
    while (offset < length) {
        usize consumed = 0;
        u32 scalar;
        if (mg_utf8_decode_one((const u8 *)text + offset, length - offset,
                               &scalar, &consumed) != MG_UTF8_OK || !consumed)
            break;
        offset += consumed;
    }
    return offset;
}

static mg_result_t redraw_from(mg_line_editor_t *editor, usize start,
                               bool repaint_text)
{
    usize prompt_length = strlen(editor->prompt);
    usize prompt_cells = display_cells(editor->prompt, prompt_length);
    usize line_end = prompt_cells + display_cells(editor->buffer, editor->length);
    usize visual_end = editor->rendered_length > line_end ? editor->rendered_length : line_end;
    usize cursor_position = prompt_cells + display_cells(editor->buffer, editor->cursor);
    bool render_prompt = editor->prompt_needs_render;
    mg_result_t result = terminal_update_begin();

    if (start > editor->length) start = 0;
    if (render_prompt) {
        start = 0;
        repaint_text = true;
    }
    if (result < 0) return result;

    if (repaint_text) {
        usize render_offset = render_prompt ? 0U : prompt_cells +
            display_cells(editor->buffer, start);

        /* Keep the cursor reposition, changed text, stale-tail clearing, and
         * final cursor placement private until the outer update commits. */
        result = terminal_input_cursor((u32)render_offset,
                                       (u32)visual_end);
        if (result < 0) goto finish_update;
        if (render_prompt) {
            if (editor->prompt_styled) {
                result = terminal_write_styled(
                    editor->prompt, prompt_length,
                    editor->prompt_foreground, editor->prompt_background);
                if (result < 0 || (usize)result != prompt_length) {
                    if (result >= 0) result = MG_ERR_IO;
                    goto finish_update;
                }
            } else {
                result = write_exact(editor->prompt, prompt_length);
                if (result < 0) goto finish_update;
            }
        }
        result = write_input_suffix(editor, start);
        if (result < 0) goto finish_update;
        if (line_end < visual_end) {
            result = clear_rendered_tail(line_end, visual_end);
            if (result < 0) goto finish_update;
        }
        editor->rendered_length = visual_end;
        editor->prompt_needs_render = false;
        /* Text writes can overwrite a cursor glyph drawn at the suffix start
         * while the terminal still considers that cursor visible. Reassert
         * the final position before presenting the transaction, including
         * when the logical cursor is at the rendered end. */
        result = terminal_input_cursor((u32)cursor_position,
                                       (u32)visual_end);
    } else {
        /* Cursor-only movement does not rewrite the prompt or input cells. */
        result = terminal_input_cursor((u32)cursor_position,
                                       (u32)visual_end);
    }
    if (result >= 0) result = sync_cursor_visibility(editor);

finish_update:
    {
        mg_result_t end_result = terminal_update_end();
        if (result >= 0 && end_result < 0) result = end_result;
    }
    return result;
}

static mg_result_t redraw(mg_line_editor_t *editor)
{
    return redraw_from(editor, 0, true);
}

static void delete_range(mg_line_editor_t *editor, usize first, usize last)
{
    if (first >= last || last > editor->length) return;
    memmove(editor->buffer + first, editor->buffer + last,
            editor->length - last);
    editor->length -= last - first;
    editor->cursor = first;
    editor->buffer[editor->length] = '\0';
}

static void insert_character(mg_line_editor_t *editor, char character)
{
    if (editor->length + 1 >= editor->capacity) return;
    memmove(editor->buffer + editor->cursor + 1,
            editor->buffer + editor->cursor,
            editor->length - editor->cursor);
    editor->buffer[editor->cursor++] = character;
    editor->length++;
    editor->buffer[editor->length] = '\0';
}

static void insert_bytes(mg_line_editor_t *editor, const char *bytes,
                         usize length)
{
    if (!editor || !bytes || !length ||
        length > editor->capacity - 1U - editor->length) return;
    memmove(editor->buffer + editor->cursor + length,
            editor->buffer + editor->cursor,
            editor->length - editor->cursor);
    memcpy(editor->buffer + editor->cursor, bytes, length);
    editor->cursor += length;
    editor->length += length;
    editor->buffer[editor->length] = '\0';
}

static void history_load(mg_line_editor_t *editor, usize index)
{
    mg_line_history_t *history = editor->history;
    const char *entry;

    if (!history || index >= history->count || !history->entry_capacity ||
        !history->storage_capacity) return;
    entry = history->storage +
        ((history->first + index) % history->storage_capacity) *
        history->entry_capacity;
    strncpy(editor->buffer, entry, editor->capacity - 1);
    editor->buffer[editor->capacity - 1] = '\0';
    editor->length = strlen(editor->buffer);
    editor->length = utf8_valid_prefix(editor->buffer, editor->length);
    editor->buffer[editor->length] = '\0';
    editor->cursor = editor->length;
}

static void history_reset_navigation(mg_line_history_t *history)
{
    if (history) {
        history->index = -1;
        history->draft_valid = false;
    }
}

bool line_editor_history_add_text(mg_line_history_t *history, const char *text)
{
    char *entry;
    usize index;
    usize length;
    usize copy_length;

    if (!history || !history->enabled || !history->storage || !text ||
        !history->capacity || !history->storage_capacity ||
        !history->entry_capacity) return false;

    length = strlen(text);
    if (length == 0) return false;

    if (history->count < history->capacity) {
        index = (history->first + history->count) % history->storage_capacity;
        history->count++;
    } else {
        index = (history->first + history->count) % history->storage_capacity;
        history->first = (history->first + 1) % history->storage_capacity;
    }
    entry = history->storage + index * history->entry_capacity;
    copy_length = length < history->entry_capacity - 1
        ? length : history->entry_capacity - 1;
    copy_length = utf8_valid_prefix(text, copy_length);
    memcpy(entry, text, copy_length);
    entry[copy_length] = '\0';
    history_reset_navigation(history);
    return true;
}

static void history_add(mg_line_editor_t *editor)
{
    mg_line_history_t *history = editor->history;

    if (!history || !history->automatic_recording || editor->length == 0) return;
    (void)line_editor_history_add_text(history, editor->buffer);
}

bool line_editor_apply_action(mg_line_editor_t *editor,
                              editor_action_t action)
{
    if (!editor) return false;

    switch (action) {
        case EDITOR_ACTION_MOVE_LEFT:
            if (editor->cursor > 0) {
                usize start;
                u32 scalar;
                if (previous_scalar(editor, editor->cursor, &start, &scalar))
                    editor->cursor = start;
            }
            return true;
        case EDITOR_ACTION_MOVE_RIGHT:
            editor->cursor = next_scalar_position(editor, editor->cursor);
            return true;
        case EDITOR_ACTION_MOVE_WORD_LEFT:
            editor->cursor = word_left(editor, editor->cursor);
            return true;
        case EDITOR_ACTION_MOVE_WORD_RIGHT:
            editor->cursor = word_right(editor, editor->cursor);
            return true;
        case EDITOR_ACTION_MOVE_LINE_START:
            editor->cursor = 0;
            return true;
        case EDITOR_ACTION_MOVE_LINE_END:
            editor->cursor = editor->length;
            return true;
        case EDITOR_ACTION_DELETE_LEFT:
            if (editor->cursor > 0) {
                usize start;
                u32 scalar;
                if (previous_scalar(editor, editor->cursor, &start, &scalar))
                    delete_range(editor, start, editor->cursor);
            }
            return true;
        case EDITOR_ACTION_DELETE_RIGHT:
            if (editor->cursor < editor->length) {
                delete_range(editor, editor->cursor,
                             next_scalar_position(editor, editor->cursor));
            }
            return true;
        case EDITOR_ACTION_DELETE_WORD_LEFT:
            delete_range(editor, word_left(editor, editor->cursor),
                         editor->cursor);
            return true;
        case EDITOR_ACTION_DELETE_WORD_RIGHT:
            delete_range(editor, editor->cursor,
                         word_right(editor, editor->cursor));
            return true;
        case EDITOR_ACTION_HISTORY_PREVIOUS:
            if (!editor->history || !editor->history->enabled ||
                editor->history->count == 0) return true;
            if (editor->history->index < 0) {
                if (editor->history->draft_storage &&
                    editor->history->draft_capacity > 0) {
                    usize copy_length = editor->length;
                    if (copy_length >= editor->history->draft_capacity) {
                        copy_length = editor->history->draft_capacity - 1;
                    }
                    copy_length = utf8_valid_prefix(editor->buffer,
                                                    copy_length);
                    memcpy(editor->history->draft_storage, editor->buffer,
                           copy_length);
                    editor->history->draft_storage[copy_length] = '\0';
                    editor->history->draft_cursor = editor->cursor;
                    if (editor->history->draft_cursor > copy_length) {
                        editor->history->draft_cursor = copy_length;
                    }
                    editor->history->draft_valid = true;
                }
                editor->history->index = (isize)editor->history->count - 1;
            } else if (editor->history->index > 0) {
                editor->history->index--;
            }
            history_load(editor, (usize)editor->history->index);
            return true;
        case EDITOR_ACTION_HISTORY_NEXT:
            if (!editor->history || !editor->history->enabled ||
                editor->history->index < 0) return true;
            if ((usize)editor->history->index + 1 < editor->history->count) {
                editor->history->index++;
                history_load(editor, (usize)editor->history->index);
            } else {
                editor->history->index = -1;
                if (editor->history->draft_valid &&
                    editor->history->draft_storage) {
                    usize length = strlen(editor->history->draft_storage);
                    if (length >= editor->capacity) length = editor->capacity - 1;
                    memcpy(editor->buffer, editor->history->draft_storage, length);
                    editor->buffer[length] = '\0';
                    editor->length = length;
                    editor->cursor = editor->history->draft_cursor;
                    if (editor->cursor > length) editor->cursor = length;
                    editor->cursor = utf8_valid_prefix(editor->buffer,
                                                       editor->cursor);
                } else {
                    editor->length = 0;
                    editor->cursor = 0;
                    editor->buffer[0] = '\0';
                }
                editor->history->draft_valid = false;
            }
            return true;
        default:
            return false;
    }
}

static mg_result_t handle_escape(mg_line_editor_t *editor)
{
    char introducer;
    char command;
    mg_result_t result;

    result = read_editor_byte(editor, &introducer);
    if (result == MG_ERR_WOULD_BLOCK) return MG_OK;
    if (result < 0) return result;
    if (introducer != EDITOR_ACTION_CSI) return MG_OK;
    result = read_editor_byte(editor, &command);
    if (result == MG_ERR_WOULD_BLOCK) return MG_OK;
    if (result < 0) return result;

    if (command == EDITOR_ACTION_MARKER) {
        char encoded_action;
        editor_action_t action;
        result = read_editor_byte(editor, &encoded_action);
        if (result == MG_ERR_WOULD_BLOCK) return MG_OK;
        if (result < 0) return result;
        action = EDITOR_ACTION_DECODE(encoded_action);
        if (!line_editor_apply_action(editor, action)) return MG_OK;

        switch (action) {
            case EDITOR_ACTION_MOVE_LEFT:
            case EDITOR_ACTION_MOVE_RIGHT:
            case EDITOR_ACTION_MOVE_WORD_LEFT:
            case EDITOR_ACTION_MOVE_WORD_RIGHT:
            case EDITOR_ACTION_MOVE_LINE_START:
            case EDITOR_ACTION_MOVE_LINE_END:
                return redraw_from(editor, editor->length, false);
            case EDITOR_ACTION_DELETE_LEFT:
            case EDITOR_ACTION_DELETE_RIGHT:
            case EDITOR_ACTION_DELETE_WORD_LEFT:
            case EDITOR_ACTION_DELETE_WORD_RIGHT:
                return redraw_from(editor, editor->cursor, true);
            default:
                /* History replacement can change arbitrary input cells. */
                return redraw_from(editor, 0, true);
        }
    }

    /* Accept the Stage 16.1 cursor packets for compatibility. */
    switch (command) {
        case 'D': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_LEFT); break;
        case 'C': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_RIGHT); break;
        case 'H': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_LINE_START); break;
        case 'F': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_LINE_END); break;
        default: return MG_OK;
    }
    return redraw_from(editor, editor->length, false);
}

void line_editor_init(mg_line_editor_t *editor, char *buffer,
                      usize capacity, const char *prompt)
{
    if (!editor) return;
    editor->buffer = buffer;
    editor->capacity = capacity;
    editor->length = 0;
    editor->cursor = 0;
    editor->rendered_length = 0;
    editor->cursor_needs_sync = false;
    editor->temporary_display_active = false;
    editor->key_event_input = false;
    editor->pending_key_event_valid = false;
    editor->prompt = prompt ? prompt : "";
    editor->history = 0;
    editor->prompt_foreground = MG_TERMINAL_COLOR_LIGHT_GRAY;
    editor->prompt_background = MG_TERMINAL_COLOR_BLACK;
    editor->prompt_styled = false;
    editor->completion = 0;
    editor->completion_context = 0;
    editor->prompt_drawn = false;
    if (buffer && capacity) buffer[0] = '\0';
}

void line_editor_set_prompt(mg_line_editor_t *editor, const char *prompt)
{
    if (editor) {
        editor->prompt = prompt ? prompt : "";
        editor->prompt_needs_render = true;
    }
}

void line_editor_set_prompt_style(mg_line_editor_t *editor,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background)
{
    if (!editor || foreground >= MG_TERMINAL_COLOR_COUNT ||
        background >= MG_TERMINAL_COLOR_COUNT) return;
    editor->prompt_foreground = foreground;
    editor->prompt_background = background;
    editor->prompt_styled = true;
    editor->prompt_needs_render = true;
}

void line_editor_set_completion(mg_line_editor_t *editor,
                                mg_line_completion_fn completion,
                                void *context)
{
    if (!editor) return;
    editor->completion = completion;
    editor->completion_context = context;
}

void line_editor_set_key_event_input(mg_line_editor_t *editor, bool enabled)
{
    if (editor) editor->key_event_input = enabled;
}

void line_editor_history_init(mg_line_history_t *history, char *storage,
                              usize entry_capacity, usize capacity)
{
    if (!history) return;
    history->storage = storage;
    history->entry_capacity = entry_capacity;
    history->capacity = capacity;
    history->storage_capacity = capacity;
    history->count = 0;
    history->first = 0;
    history->index = -1;
    history->enabled = true;
    history->automatic_recording = true;
    history->draft_storage = NULL;
    history->draft_capacity = 0;
    history->draft_cursor = 0;
    history->draft_valid = false;
}

void line_editor_set_history(mg_line_editor_t *editor,
                             mg_line_history_t *history)
{
    if (editor) editor->history = history;
}

bool line_editor_history_set_capacity(mg_line_history_t *history, usize capacity)
{
    if (!history || capacity > history->storage_capacity) return false;

    if (capacity == 0) {
        history->capacity = 0;
        history->count = 0;
        history->first = 0;
        history_reset_navigation(history);
        return true;
    }

    if (history->count > capacity) {
        usize discarded = history->count - capacity;
        history->first = (history->first + discarded) % history->storage_capacity;
        history->count = capacity;
    }
    history->capacity = capacity;
    history_reset_navigation(history);
    return true;
}

void line_editor_history_set_enabled(mg_line_history_t *history, bool enabled)
{
    if (!history) return;
    history->enabled = enabled;
    if (!enabled) history_reset_navigation(history);
}

void line_editor_history_set_automatic_recording(mg_line_history_t *history,
                                                 bool enabled)
{
    if (history) history->automatic_recording = enabled;
}

void line_editor_history_set_draft(mg_line_history_t *history,
                                   char *storage,
                                   usize capacity)
{
    if (!history) return;
    history->draft_storage = storage;
    history->draft_capacity = capacity;
    history->draft_cursor = 0;
    history->draft_valid = false;
}

const char *line_editor_history_last(const mg_line_history_t *history)
{
    return history && history->count
        ? line_editor_history_at(history, history->count - 1) : NULL;
}

const char *line_editor_history_at(const mg_line_history_t *history, usize index)
{
    usize slot;

    if (!history || index >= history->count || !history->storage_capacity)
        return NULL;
    slot = (history->first + index) % history->storage_capacity;
    return history->storage + slot * history->entry_capacity;
}

mg_result_t line_editor_prepare_next_prompt(mg_line_editor_t *editor)
{
    mg_result_t result;
    if (!editor || !editor->buffer || editor->capacity == 0 || !editor->prompt)
        return MG_ERR_BAD_ARGUMENT;

    result = terminal_prepare_input_line();
    if (result != MG_OK) return result;
    editor->length = 0;
    editor->cursor = 0;
    editor->rendered_length = 0;
    editor->prompt_needs_render = true;
    if (editor->history) history_reset_navigation(editor->history);
    editor->buffer[0] = '\0';

    result = redraw(editor);
    if (result >= 0) {
        editor->prompt_drawn = true;
    }
    return result;
}

mg_result_t line_editor_clear_line(mg_line_editor_t *editor)
{
    static const char carriage_return = '\r';
    mg_result_t result;

    if (!editor) return MG_ERR_BAD_ARGUMENT;
    result = write_bytes(&carriage_return, 1);
    if (result < 0) return result;
    result = terminal_clear_line();
    if (result != MG_OK) return result;
    /* terminal_clear_line() restores the cursor as part of its normal
     * operation.  Completion listings keep it hidden until the new prompt
     * and input have been redrawn. */
    result = terminal_cursor_set_visible(false);
    if (result == MG_OK) {
        editor->rendered_length = 0;
        editor->prompt_needs_render = true;
        editor->cursor_needs_sync = true;
    }
    return result;
}

static mg_result_t line_editor_submit(mg_line_editor_t *editor)
{
    mg_result_t result;

    history_add(editor);
    /* Finish below the entire input, even when Enter was pressed with the
     * cursor on an earlier wrapped row. */
    result = terminal_input_cursor(
        display_cells(editor->prompt, strlen(editor->prompt)) +
            display_cells(editor->buffer, editor->length),
        editor->rendered_length);
    if (result < 0) return result;
    result = write_byte('\n');
    editor->prompt_drawn = false;
    editor->prompt_needs_render = false;
    return result < 0 ? result : (mg_result_t)editor->length;
}

static mg_result_t line_editor_cancel(mg_line_editor_t *editor)
{
    static const char cancelled[] = "^C\n";
    mg_result_t result = terminal_update_begin();
    mg_result_t end_result;

    if (result < 0) return result;
    result = terminal_input_cursor(
        display_cells(editor->prompt, strlen(editor->prompt)) +
            display_cells(editor->buffer, editor->length),
        editor->rendered_length);
    if (result >= 0) result = write_exact(cancelled, sizeof(cancelled) - 1U);
    if (result >= 0) {
        editor->length = 0;
        editor->cursor = 0;
        editor->rendered_length = 0;
        editor->buffer[0] = '\0';
        editor->prompt_drawn = false;
        editor->prompt_needs_render = true;
        if (editor->history) history_reset_navigation(editor->history);
    }
    end_result = terminal_update_end();
    if (result >= 0 && end_result < 0) result = end_result;
    return result < 0 ? result : MG_ERR_CANCELLED;
}

static mg_result_t line_editor_clear_and_redraw(mg_line_editor_t *editor)
{
    mg_result_t result = terminal_update_begin();
    mg_result_t end_result;

    if (result < 0) return result;
    result = terminal_clear_screen();
    if (result == MG_OK) result = terminal_prepare_input_line();
    if (result == MG_OK) {
        editor->rendered_length = 0;
        editor->prompt_needs_render = true;
        editor->cursor_needs_sync = true;
        result = redraw_from(editor, 0, true);
    }
    end_result = terminal_update_end();
    if (result >= 0 && end_result < 0) result = end_result;
    return result;
}

static mg_result_t apply_action_and_redraw(mg_line_editor_t *editor,
                                           editor_action_t action)
{
    usize old_length = editor->length;

    if (!line_editor_apply_action(editor, action)) return MG_OK;
    switch (action) {
        case EDITOR_ACTION_MOVE_LEFT:
        case EDITOR_ACTION_MOVE_RIGHT:
        case EDITOR_ACTION_MOVE_WORD_LEFT:
        case EDITOR_ACTION_MOVE_WORD_RIGHT:
        case EDITOR_ACTION_MOVE_LINE_START:
        case EDITOR_ACTION_MOVE_LINE_END:
            return redraw_from(editor, editor->length, false);
        case EDITOR_ACTION_DELETE_LEFT:
        case EDITOR_ACTION_DELETE_RIGHT:
        case EDITOR_ACTION_DELETE_WORD_LEFT:
        case EDITOR_ACTION_DELETE_WORD_RIGHT:
            return redraw_from(editor, editor->cursor,
                               editor->length != old_length);
        case EDITOR_ACTION_HISTORY_PREVIOUS:
        case EDITOR_ACTION_HISTORY_NEXT:
            return redraw_from(editor, 0, true);
        default:
            return MG_OK;
    }
}

static mg_result_t line_editor_handle_key_event(
    mg_line_editor_t *editor, const mg_terminal_key_event_t *event,
    bool *finished)
{
    const u32 control_mask = MG_TERMINAL_MODIFIER_LEFT_CTRL |
                             MG_TERMINAL_MODIFIER_RIGHT_CTRL;
    const u32 alt_mask = MG_TERMINAL_MODIFIER_LEFT_ALT |
                         MG_TERMINAL_MODIFIER_RIGHT_ALT;
    const u32 gui_mask = MG_TERMINAL_MODIFIER_LEFT_GUI |
                         MG_TERMINAL_MODIFIER_RIGHT_GUI;
    bool control, shift, alt, gui;
    editor_action_t action = EDITOR_ACTION_NONE;

    if (!editor || !event || !finished) return MG_ERR_BAD_ARGUMENT;
    *finished = false;
    control = (event->modifiers & control_mask) != 0;
    shift = (event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_SHIFT |
                                 MG_TERMINAL_MODIFIER_RIGHT_SHIFT)) != 0;
    alt = (event->modifiers & alt_mask) != 0;
    gui = (event->modifiers & gui_mask) != 0;

    if (control && !shift && !alt && !gui) {
        switch (event->keycode) {
            case 0x06U: /* C */
                *finished = true;
                return line_editor_cancel(editor);
            case 0x07U: /* D */
                if (editor->length == 0) {
                    *finished = true;
                    return MG_ERR_END_OF_FILE;
                }
                return apply_action_and_redraw(editor,
                                               EDITOR_ACTION_DELETE_RIGHT);
            case 0x0fU: /* L */
                return line_editor_clear_and_redraw(editor);
            default:
                return MG_OK;
        }
    }
    if (alt && !control && !shift && !gui) {
        switch (event->keycode) {
            case MG_TERMINAL_KEY_LEFT:
                action = EDITOR_ACTION_MOVE_WORD_LEFT;
                break;
            case MG_TERMINAL_KEY_RIGHT:
                action = EDITOR_ACTION_MOVE_WORD_RIGHT;
                break;
            case MG_TERMINAL_KEY_BACKSPACE:
                action = EDITOR_ACTION_DELETE_WORD_LEFT;
                break;
            case MG_TERMINAL_KEY_DELETE:
                action = EDITOR_ACTION_DELETE_WORD_RIGHT;
                break;
            default:
                return MG_OK;
        }
        return apply_action_and_redraw(editor, action);
    }
    if (control || alt || gui) return MG_OK;

    switch (event->keycode) {
        case MG_TERMINAL_KEY_ENTER:
            *finished = true;
            return line_editor_submit(editor);
        case MG_TERMINAL_KEY_TAB:
            if (editor->completion) {
                mg_result_t result = editor->completion(
                    editor, editor->completion_context);
                if (result < 0) return result;
                if (!editor->temporary_display_active)
                    return redraw_from(editor, 0, true);
            }
            return MG_OK;
        case MG_TERMINAL_KEY_LEFT:
            action = EDITOR_ACTION_MOVE_LEFT;
            break;
        case MG_TERMINAL_KEY_RIGHT:
            action = EDITOR_ACTION_MOVE_RIGHT;
            break;
        case MG_TERMINAL_KEY_UP:
            action = EDITOR_ACTION_HISTORY_PREVIOUS;
            break;
        case MG_TERMINAL_KEY_DOWN:
            action = EDITOR_ACTION_HISTORY_NEXT;
            break;
        case MG_TERMINAL_KEY_HOME:
            action = EDITOR_ACTION_MOVE_LINE_START;
            break;
        case MG_TERMINAL_KEY_END:
            action = EDITOR_ACTION_MOVE_LINE_END;
            break;
        case MG_TERMINAL_KEY_BACKSPACE:
            action = EDITOR_ACTION_DELETE_LEFT;
            break;
        case MG_TERMINAL_KEY_DELETE:
            action = EDITOR_ACTION_DELETE_RIGHT;
            break;
        default:
            break;
    }
    if (action != EDITOR_ACTION_NONE)
        return apply_action_and_redraw(editor, action);

    if (event->codepoint >= 0x20U && event->codepoint != 0x7fU) {
        u8 encoded[4];
        usize encoded_length = 0;
        usize old_cursor = editor->cursor;
        usize old_length = editor->length;
        if (mg_utf8_encode_one(event->codepoint, encoded, sizeof(encoded),
                               &encoded_length) != MG_UTF8_OK)
            return MG_OK;
        insert_bytes(editor, (const char *)encoded, encoded_length);
        return redraw_from(editor, old_cursor,
                           editor->length != old_length);
    }
    return MG_OK;
}

static mg_result_t line_editor_read_line_inner(mg_line_editor_t *editor)
{
    char character;
    mg_result_t result;
    u8 pending_utf8[4];
    usize pending_length = 0;
    mg_utf8_state_t utf8_state = { 0 };

    if (!editor || !editor->buffer || editor->capacity == 0 ||
        !editor->prompt) return MG_ERR_BAD_ARGUMENT;

    if (!editor->prompt_drawn) {
        result = line_editor_prepare_next_prompt(editor);
        if (result < 0) return result;
    }

    for (;;) {
        if (editor->key_event_input) {
            mg_terminal_key_event_t event;
            bool finished;

            if (editor->pending_key_event_valid) {
                event = editor->pending_key_event;
                editor->pending_key_event_valid = false;
            } else {
                result = terminal_read_key_event(MG_TERMINAL_WAIT_FOREVER,
                                                 &event);
                if (result < 0) return result;
            }

            result = line_editor_clear_temporary_display(editor);
            if (result < 0) return result;
            if (event.flags & MG_TERMINAL_KEY_EVENT_COOKED_BYTE) {
                character = (char)(u8)event.codepoint;
                goto process_input_byte;
            }
            result = line_editor_handle_key_event(editor, &event, &finished);
            if (finished || result < 0) return result;
            continue;
        }

        result = read_byte(&character);
        if (result < 0) return result;

        /* Completion rows are visual-only.  Any subsequent editing action
         * restores the backing terminal before changing the input line. */
        result = line_editor_clear_temporary_display(editor);
        if (result < 0) return result;

process_input_byte:
        if (pending_length || (u8)character >= 0x80U) {
            mg_utf8_feed_result_t decoded = mg_utf8_state_feed(
                &utf8_state, (u8)character);
            if (decoded.status == MG_UTF8_INCOMPLETE) {
                if (decoded.consumed && pending_length < sizeof(pending_utf8))
                    pending_utf8[pending_length++] = (u8)character;
                continue;
            }
            if (decoded.status == MG_UTF8_INVALID) {
                pending_length = 0;
                if (!decoded.consumed) goto process_input_byte;
                continue;
            }
            if (!pending_length && decoded.scalar < 0x80U)
                goto process_ascii_byte;
            if (pending_length >= sizeof(pending_utf8)) {
                pending_length = 0;
                continue;
            }
            pending_utf8[pending_length++] = (u8)character;
            usize old_cursor = editor->cursor;
            usize old_length = editor->length;
            if (decoded.scalar != 0U)
                insert_bytes(editor, (const char *)pending_utf8,
                             pending_length);
            pending_length = 0;
            result = redraw_from(editor, old_cursor,
                                 editor->length != old_length);
            if (result < 0) return result;
            continue;
        }

process_ascii_byte:
        if (character == '\n') {
            return line_editor_submit(editor);
        }
        if (character == '\t') {
            if (editor->completion) {
                result = editor->completion(editor,
                                            editor->completion_context);
                if (result < 0) return result;
                if (!editor->temporary_display_active) {
                    result = redraw_from(editor, 0, true);
                    if (result < 0) return result;
                }
            }
            continue;
        }
        if (character == '\b') {
            usize old_length = editor->length;
            (void)line_editor_apply_action(editor, EDITOR_ACTION_DELETE_LEFT);
            result = redraw_from(editor, editor->cursor,
                                 editor->length != old_length);
            if (result < 0) return result;
            continue;
        }
        if ((unsigned char)character == 0x7f) {
            usize old_length = editor->length;
            (void)line_editor_apply_action(editor, EDITOR_ACTION_DELETE_RIGHT);
            result = redraw_from(editor, editor->cursor,
                                 editor->length != old_length);
            if (result < 0) return result;
            continue;
        }
        if ((unsigned char)character == (u8)EDITOR_ACTION_ESCAPE) {
            result = handle_escape(editor);
            if (result < 0) return result;
            continue;
        }
        if ((unsigned char)character >= 0x20 &&
            (unsigned char)character != 0x7f) {
            usize old_cursor = editor->cursor;
            usize old_length = editor->length;
            insert_character(editor, character);
            result = redraw_from(editor, old_cursor,
                                 editor->length != old_length);
            if (result < 0) return result;
        }
    }
}

mg_result_t line_editor_read_line(mg_line_editor_t *editor)
{
    mg_result_t result;
    mg_result_t release_result;

    if (!editor || !editor->buffer || editor->capacity == 0 ||
        !editor->prompt) return MG_ERR_BAD_ARGUMENT;
    if (!editor->key_event_input)
        return line_editor_read_line_inner(editor);

    result = terminal_input_raw(true);
    if (result < 0) return result;
    result = line_editor_read_line_inner(editor);
    release_result = terminal_input_raw(false);
    if (result >= 0 && release_result < 0) return release_result;
    return result;
}
