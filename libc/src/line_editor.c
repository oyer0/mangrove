#include <mg/line_editor.h>
#include <mg/object.h>
#include <string.h>

#define EDITOR_SGR_INVERSE "\x1b[7m"
#define EDITOR_SGR_NORMAL  "\x1b[0m"

static mg_result_t write_bytes(const void *buffer, usize length)
{
    return object_write_all(MG_CONSOLE_HANDLE, buffer, length);
}

static mg_result_t write_byte(char byte)
{
    return write_bytes(&byte, 1);
}

static mg_result_t read_byte(char *out)
{
    mg_result_t result;

    if (!out) return MG_ERR_BAD_ARGUMENT;
    result = object_read(MG_CONSOLE_HANDLE, out, 1);
    if (result < 0) return result;
    return result == 1 ? MG_OK : MG_ERR_END_OF_FILE;
}

static bool selection_has_text(const mg_line_editor_t *editor)
{
    return editor->selection_active &&
        editor->selection_anchor != editor->cursor;
}

static usize selection_start(const mg_line_editor_t *editor)
{
    return editor->selection_anchor < editor->cursor
        ? editor->selection_anchor : editor->cursor;
}

static usize selection_end(const mg_line_editor_t *editor)
{
    return editor->selection_anchor > editor->cursor
        ? editor->selection_anchor : editor->cursor;
}

static void clear_selection(mg_line_editor_t *editor)
{
    editor->selection_anchor = editor->cursor;
    editor->selection_active = false;
}

static void start_selection(mg_line_editor_t *editor)
{
    if (!editor->selection_active) {
        editor->selection_anchor = editor->cursor;
        editor->selection_active = true;
    }
}

static bool is_space(char character)
{
    return character == ' ' || character == '\t';
}

static bool is_word_character(char character)
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_';
}

static usize word_left(const mg_line_editor_t *editor, usize position)
{
    while (position > 0 && is_space(editor->buffer[position - 1])) position--;
    if (position > 0 && is_word_character(editor->buffer[position - 1])) {
        while (position > 0 && is_word_character(editor->buffer[position - 1])) {
            position--;
        }
    } else {
        while (position > 0 && !is_space(editor->buffer[position - 1]) &&
               !is_word_character(editor->buffer[position - 1])) {
            position--;
        }
    }
    return position;
}

static usize word_right(const mg_line_editor_t *editor, usize position)
{
    if (position < editor->length && is_word_character(editor->buffer[position])) {
        while (position < editor->length &&
               is_word_character(editor->buffer[position])) position++;
    } else {
        while (position < editor->length && !is_space(editor->buffer[position]) &&
               !is_word_character(editor->buffer[position])) position++;
    }
    while (position < editor->length && is_space(editor->buffer[position])) position++;
    return position;
}

typedef struct {
    char data[1024];
    usize len;
} line_redraw_buffer_t;

static void line_buf_append(line_redraw_buffer_t *b, const char *src, usize count)
{
    if (b->len + count > sizeof(b->data)) {
        count = sizeof(b->data) - b->len;
    }
    memcpy(b->data + b->len, src, count);
    b->len += count;
}

static void append_line_text(const mg_line_editor_t *editor, line_redraw_buffer_t *b)
{
    if (!selection_has_text(editor)) {
        line_buf_append(b, editor->buffer, editor->length);
        return;
    }

    usize first = selection_start(editor);
    usize last = selection_end(editor);
    line_buf_append(b, editor->buffer, first);
    line_buf_append(b, EDITOR_SGR_INVERSE, sizeof(EDITOR_SGR_INVERSE) - 1);
    line_buf_append(b, editor->buffer + first, last - first);
    line_buf_append(b, EDITOR_SGR_NORMAL, sizeof(EDITOR_SGR_NORMAL) - 1);
    line_buf_append(b, editor->buffer + last, editor->length - last);
}

static mg_result_t redraw(mg_line_editor_t *editor)
{
    static const char carriage_return = '\r';
    static const char backspace = '\b';
    static const char space = ' ';
    line_redraw_buffer_t b;
    b.len = 0;

    usize prompt_length = strlen(editor->prompt);
    usize line_end = prompt_length + editor->length;
    usize visual_end = editor->rendered_length > line_end
        ? editor->rendered_length : line_end;
    usize cursor_position = prompt_length + editor->cursor;
    usize i;

    line_buf_append(&b, &carriage_return, 1);
    line_buf_append(&b, editor->prompt, prompt_length);
    append_line_text(editor, &b);

    for (i = line_end; i < visual_end; i++) {
        line_buf_append(&b, &space, 1);
    }
    while (visual_end > cursor_position) {
        line_buf_append(&b, &backspace, 1);
        visual_end--;
    }

    editor->rendered_length = editor->rendered_length > line_end
        ? editor->rendered_length : line_end;

    return write_bytes(b.data, b.len);
}

static void delete_range(mg_line_editor_t *editor, usize first, usize last)
{
    if (first >= last || last > editor->length) return;
    memmove(editor->buffer + first, editor->buffer + last,
            editor->length - last);
    editor->length -= last - first;
    editor->cursor = first;
    editor->buffer[editor->length] = '\0';
    clear_selection(editor);
}

static bool delete_selection(mg_line_editor_t *editor)
{
    if (!selection_has_text(editor)) return false;
    delete_range(editor, selection_start(editor), selection_end(editor));
    return true;
}

static void insert_character(mg_line_editor_t *editor, char character)
{
    (void)delete_selection(editor);
    if (editor->length + 1 >= editor->capacity) return;
    memmove(editor->buffer + editor->cursor + 1,
            editor->buffer + editor->cursor,
            editor->length - editor->cursor);
    editor->buffer[editor->cursor++] = character;
    editor->length++;
    editor->buffer[editor->length] = '\0';
}

static void history_load(mg_line_editor_t *editor, usize index)
{
    mg_line_history_t *history = editor->history;
    const char *entry;

    if (!history || index >= history->count || !history->entry_capacity) return;
    entry = history->storage +
        ((history->first + index) % history->capacity) * history->entry_capacity;
    strncpy(editor->buffer, entry, editor->capacity - 1);
    editor->buffer[editor->capacity - 1] = '\0';
    editor->length = strlen(editor->buffer);
    editor->cursor = editor->length;
    clear_selection(editor);
}

static void history_add(mg_line_editor_t *editor)
{
    mg_line_history_t *history = editor->history;
    char *entry;
    usize index;

    if (!history || !history->storage || !history->capacity ||
        !history->entry_capacity || editor->length == 0) return;

    if (history->count < history->capacity) {
        index = (history->first + history->count) % history->capacity;
        history->count++;
    } else {
        index = history->first;
        history->first = (history->first + 1) % history->capacity;
    }
    entry = history->storage + index * history->entry_capacity;
    strncpy(entry, editor->buffer, history->entry_capacity - 1);
    entry[history->entry_capacity - 1] = '\0';
    history->index = -1;
}

bool line_editor_apply_action(mg_line_editor_t *editor,
                              editor_action_t action)
{
    if (!editor) return false;

    switch (action) {
        case EDITOR_ACTION_MOVE_LEFT:
            if (selection_has_text(editor)) {
                editor->cursor = selection_start(editor);
            } else if (editor->cursor > 0) {
                editor->cursor--;
            }
            clear_selection(editor);
            return true;
        case EDITOR_ACTION_MOVE_RIGHT:
            if (selection_has_text(editor)) {
                editor->cursor = selection_end(editor);
            } else if (editor->cursor < editor->length) {
                editor->cursor++;
            }
            clear_selection(editor);
            return true;
        case EDITOR_ACTION_MOVE_WORD_LEFT:
            editor->cursor = selection_has_text(editor) ? selection_start(editor)
                : word_left(editor, editor->cursor);
            clear_selection(editor);
            return true;
        case EDITOR_ACTION_MOVE_WORD_RIGHT:
            editor->cursor = selection_has_text(editor) ? selection_end(editor)
                : word_right(editor, editor->cursor);
            clear_selection(editor);
            return true;
        case EDITOR_ACTION_MOVE_LINE_START:
            editor->cursor = 0;
            clear_selection(editor);
            return true;
        case EDITOR_ACTION_MOVE_LINE_END:
            editor->cursor = editor->length;
            clear_selection(editor);
            return true;
        case EDITOR_ACTION_SELECT_LEFT:
            start_selection(editor);
            if (editor->cursor > 0) editor->cursor--;
            return true;
        case EDITOR_ACTION_SELECT_RIGHT:
            start_selection(editor);
            if (editor->cursor < editor->length) editor->cursor++;
            return true;
        case EDITOR_ACTION_SELECT_WORD_LEFT:
            start_selection(editor);
            editor->cursor = word_left(editor, editor->cursor);
            return true;
        case EDITOR_ACTION_SELECT_WORD_RIGHT:
            start_selection(editor);
            editor->cursor = word_right(editor, editor->cursor);
            return true;
        case EDITOR_ACTION_SELECT_LINE_START:
            start_selection(editor);
            editor->cursor = 0;
            return true;
        case EDITOR_ACTION_SELECT_LINE_END:
            start_selection(editor);
            editor->cursor = editor->length;
            return true;
        case EDITOR_ACTION_DELETE_LEFT:
            if (!delete_selection(editor) && editor->cursor > 0) {
                delete_range(editor, editor->cursor - 1, editor->cursor);
            }
            return true;
        case EDITOR_ACTION_DELETE_RIGHT:
            if (!delete_selection(editor) && editor->cursor < editor->length) {
                delete_range(editor, editor->cursor, editor->cursor + 1);
            }
            return true;
        case EDITOR_ACTION_DELETE_WORD_LEFT:
            if (!delete_selection(editor)) {
                usize first = word_left(editor, editor->cursor);
                delete_range(editor, first, editor->cursor);
            }
            return true;
        case EDITOR_ACTION_HISTORY_PREVIOUS:
            if (!editor->history || editor->history->count == 0) return true;
            if (editor->history->index < 0) {
                editor->history->index = (isize)editor->history->count - 1;
            } else if (editor->history->index > 0) {
                editor->history->index--;
            }
            history_load(editor, (usize)editor->history->index);
            return true;
        case EDITOR_ACTION_HISTORY_NEXT:
            if (!editor->history || editor->history->index < 0) return true;
            if ((usize)editor->history->index + 1 < editor->history->count) {
                editor->history->index++;
                history_load(editor, (usize)editor->history->index);
            } else {
                editor->history->index = -1;
                editor->length = 0;
                editor->cursor = 0;
                editor->buffer[0] = '\0';
                clear_selection(editor);
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

    result = read_byte(&introducer);
    if (result < 0) return result;
    if (introducer != EDITOR_ACTION_CSI) return MG_OK;
    result = read_byte(&command);
    if (result < 0) return result;

    if (command == EDITOR_ACTION_MARKER) {
        char encoded_action;
        editor_action_t action;
        result = read_byte(&encoded_action);
        if (result < 0) return result;
        action = EDITOR_ACTION_DECODE(encoded_action);
        if (line_editor_apply_action(editor, action)) {
            return redraw(editor);
        }
        return MG_OK;
    }

    /* Accept the Stage 16.1 cursor packets for compatibility. */
    switch (command) {
        case 'D': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_LEFT); break;
        case 'C': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_RIGHT); break;
        case 'H': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_LINE_START); break;
        case 'F': (void)line_editor_apply_action(editor, EDITOR_ACTION_MOVE_LINE_END); break;
        default: return MG_OK;
    }
    return redraw(editor);
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
    editor->selection_anchor = 0;
    editor->selection_active = false;
    editor->prompt = prompt ? prompt : "";
    editor->history = 0;
    editor->prompt_drawn = false;
    if (buffer && capacity) buffer[0] = '\0';
}

void line_editor_set_prompt(mg_line_editor_t *editor, const char *prompt)
{
    if (editor) editor->prompt = prompt ? prompt : "";
}

void line_editor_history_init(mg_line_history_t *history, char *storage,
                              usize entry_capacity, usize capacity)
{
    if (!history) return;
    history->storage = storage;
    history->entry_capacity = entry_capacity;
    history->capacity = capacity;
    history->count = 0;
    history->first = 0;
    history->index = -1;
}

void line_editor_set_history(mg_line_editor_t *editor,
                             mg_line_history_t *history)
{
    if (editor) editor->history = history;
}

mg_result_t line_editor_prepare_next_prompt(mg_line_editor_t *editor)
{
    mg_result_t result;
    if (!editor || !editor->buffer || editor->capacity == 0 || !editor->prompt)
        return MG_ERR_BAD_ARGUMENT;

    editor->length = 0;
    editor->cursor = 0;
    editor->rendered_length = 0;
    clear_selection(editor);
    if (editor->history) editor->history->index = -1;
    editor->buffer[0] = '\0';

    result = redraw(editor);
    if (result >= 0) {
        editor->prompt_drawn = true;
    }
    return result;
}

mg_result_t line_editor_read_line(mg_line_editor_t *editor)
{
    char character;
    mg_result_t result;

    if (!editor || !editor->buffer || editor->capacity == 0 ||
        !editor->prompt) return MG_ERR_BAD_ARGUMENT;

    if (!editor->prompt_drawn) {
        result = line_editor_prepare_next_prompt(editor);
        if (result < 0) return result;
    }

    for (;;) {
        result = read_byte(&character);
        if (result < 0) return result;

        if (character == '\n') {
            history_add(editor);
            result = write_byte('\n');
            editor->prompt_drawn = false;
            return result < 0 ? result : (mg_result_t)editor->length;
        }
        if (character == '\b') {
            (void)line_editor_apply_action(editor, EDITOR_ACTION_DELETE_LEFT);
            result = redraw(editor);
            if (result < 0) return result;
            continue;
        }
        if ((unsigned char)character == 0x7f) {
            (void)line_editor_apply_action(editor, EDITOR_ACTION_DELETE_RIGHT);
            result = redraw(editor);
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
            insert_character(editor, character);
            result = redraw(editor);
            if (result < 0) return result;
        }
    }
}
