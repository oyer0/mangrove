/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "completion.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <stdlib.h>
#include <string.h>
#include "../common/path.h"

#define COMPLETION_MAX_CANDIDATES 128U
#define COMPLETION_TEXT_CAPACITY  320U
#define COMPLETION_COLUMN_GAP     2U

typedef struct completion_token {
    usize raw_start;
    usize raw_end;
    usize value_start;
    usize value_end;
    bool leading_quote;
    char quote;
} completion_token_t;

typedef struct completion_candidate {
    char text[COMPLETION_TEXT_CAPACITY];
    bool directory;
} completion_candidate_t;

static bool is_separator(char value)
{
    return value == ' ' || value == '\t' || value == '>';
}

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static bool chars_equal(char left, char right, bool case_sensitive)
{
    return case_sensitive ? left == right :
        ascii_lower(left) == ascii_lower(right);
}

static bool starts_with(const char *text, const char *prefix,
                        bool case_sensitive)
{
    if (!text || !prefix) return false;
    while (*prefix) {
        if (!*text || !chars_equal(*text, *prefix, case_sensitive))
            return false;
        text++;
        prefix++;
    }
    return true;
}

static void token_at(const char *line, usize length, usize position,
                     completion_token_t *token)
{
    usize start = 0;
    usize end;
    char quote = '\0';
    bool closed = false;

    if (!line || !token) return;
    if (position > length) position = length;
    for (usize index = 0; index < position; index++) {
        char value = line[index];

        if (quote) {
            if (value == quote) quote = '\0';
        } else if (value == '\'' || value == '"') {
            quote = value;
        } else if (is_separator(value)) {
            start = index + 1U;
        }
    }

    end = start;
    quote = '\0';
    for (; end < length; end++) {
        char value = line[end];

        if (quote) {
            if (value == quote) {
                quote = '\0';
                closed = true;
            }
        } else if (value == '\'' || value == '"') {
            quote = value;
        } else if (is_separator(value)) {
            break;
        }
    }

    token->raw_start = start;
    token->raw_end = end;
    token->leading_quote = start < end &&
        (line[start] == '\'' || line[start] == '"');
    token->quote = token->leading_quote ? line[start] : '\0';
    token->value_start = token->leading_quote ? start + 1U : start;
    token->value_end = end;
    if (token->leading_quote && closed && end > token->value_start &&
        line[end - 1U] == token->quote)
        token->value_end = end - 1U;
}

static bool first_command(const char *line, usize length, char *name,
                          usize capacity, usize *out_start)
{
    usize start = 0;
    usize used = 0;
    char quote = '\0';

    if (!line || !name || capacity < 2U || !out_start) return false;
    while (start < length && (line[start] == ' ' || line[start] == '\t'))
        start++;
    if (start == length) return false;
    for (usize index = start; index < length; index++) {
        char value = line[index];

        if (quote) {
            if (value == quote) quote = '\0';
            else if (used + 1U < capacity) name[used++] = value;
        } else if (value == '\'' || value == '"') {
            quote = value;
        } else if (is_separator(value)) {
            break;
        } else if (used + 1U < capacity) {
            name[used++] = value;
        } else {
            return false;
        }
    }
    if (!used || quote) return false;
    name[used] = '\0';
    *out_start = start;
    return true;
}

static bool current_prefix(const mg_line_editor_t *editor,
                           const completion_token_t *token,
                           char *prefix, usize capacity)
{
    usize end;
    usize length;

    if (!editor || !token || !prefix || capacity == 0) return false;
    end = editor->cursor < token->value_end ? editor->cursor :
          token->value_end;
    if (end < token->value_start) end = token->value_start;
    length = end - token->value_start;
    if (length >= capacity) return false;
    memcpy(prefix, editor->buffer + token->value_start, length);
    prefix[length] = '\0';
    return true;
}

static bool candidate_add(completion_candidate_t *candidates, usize *count,
                          const char *text, bool directory,
                          const char *prefix, bool case_sensitive)
{
    if (!candidates || !count || !text || !prefix ||
        *count >= COMPLETION_MAX_CANDIDATES ||
        strlen(text) >= COMPLETION_TEXT_CAPACITY ||
        !starts_with(text, prefix, case_sensitive)) return true;
    for (usize index = 0; index < *count; index++)
        if (!strcmp(candidates[index].text, text)) return true;
    strcpy(candidates[*count].text, text);
    candidates[*count].directory = directory;
    (*count)++;
    return true;
}

static void sort_candidates(completion_candidate_t *candidates, usize count)
{
    for (usize index = 1; index < count; index++) {
        completion_candidate_t value = candidates[index];
        usize position = index;

        while (position > 0 &&
               strcmp(candidates[position - 1U].text, value.text) > 0) {
            candidates[position] = candidates[position - 1U];
            position--;
        }
        candidates[position] = value;
    }
}

static bool collect_commands(completion_candidate_t *candidates, usize *count,
                             const char *prefix, bool case_sensitive)
{
    mg_handle_t directory;
    mg_directory_entry_t batch[MG_DIRECTORY_BATCH_MAX];
    mg_result_t open_result;

    for (usize index = 0; index < shell_builtin_count(); index++) {
        const shell_command_info_t *builtin = shell_builtin_at(index);
        if (!builtin || !candidate_add(candidates, count, builtin->name, false,
                                       prefix, case_sensitive)) return false;
    }

    open_result = directory_open("/bin");
    if (open_result < 0) return true;
    directory = (mg_handle_t)open_result;
    for (;;) {
        usize entry_count = 0;
        mg_result_t result = directory_read_batch(
            directory, batch, MG_DIRECTORY_BATCH_MAX, &entry_count);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result < 0) break;
        for (usize index = 0; index < entry_count; index++) {
            if (batch[index].type != MG_PATH_TYPE_FILE) continue;
            if (!candidate_add(candidates, count, batch[index].name, false,
                               prefix, case_sensitive)) {
                (void)handle_close(directory);
                return false;
            }
        }
    }
    (void)handle_close(directory);
    return true;
}

static bool append_text(char *destination, usize *used, usize capacity,
                        const char *text)
{
    usize length;

    if (!destination || !used || !text) return false;
    length = strlen(text);
    if (*used + length >= capacity) return false;
    memcpy(destination + *used, text, length);
    *used += length;
    destination[*used] = '\0';
    return true;
}

static bool collect_paths(const char *prefix, bool directories_only,
                          completion_candidate_t *candidates, usize *count,
                          bool case_sensitive)
{
    char directory_input[SHOOT_LINE_CAPACITY];
    char expanded[256];
    char directory_path[256];
    const char *slash = NULL;
    usize prefix_length;
    usize directory_length = 0;
    usize base_start = 0;
    mg_handle_t directory;
    mg_directory_entry_t batch[MG_DIRECTORY_BATCH_MAX];

    if (!prefix || !candidates || !count) return false;
    prefix_length = strlen(prefix);
    for (usize index = 0; index < prefix_length; index++)
        if (prefix[index] == '/') slash = prefix + index;

    if (!slash && prefix[0] == '~') {
        strcpy(directory_input, "~/");
        directory_length = 2U;
        base_start = 1U;
    } else if (slash) {
        directory_length = (usize)(slash - prefix) + 1U;
        if (directory_length >= sizeof(directory_input)) return false;
        memcpy(directory_input, prefix, directory_length);
        directory_input[directory_length] = '\0';
        base_start = directory_length;
    } else {
        strcpy(directory_input, ".");
        directory_length = 0;
        base_start = 0;
    }
    (void)directory_length;
    if (!command_expand_home_path(directory_input, true, expanded,
                                  sizeof(expanded)) ||
        !command_resolve_path(expanded, directory_path,
                               sizeof(directory_path))) return false;

    directory = (mg_handle_t)directory_open(directory_path);
    if ((i32)directory < 0 || !directory) return true;
    for (;;) {
        usize entry_count = 0;
        mg_result_t result = directory_read_batch(
            directory, batch, MG_DIRECTORY_BATCH_MAX, &entry_count);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result < 0) break;
        for (usize index = 0; index < entry_count; index++) {
            char candidate[COMPLETION_TEXT_CAPACITY];
            usize used = 0;
            bool is_directory = batch[index].type == MG_PATH_TYPE_DIRECTORY;

            if (directories_only && !is_directory) continue;
            if (!append_text(candidate, &used, sizeof(candidate),
                             prefix[0] == '~' && !slash ? "~/" : "")) {
                (void)handle_close(directory);
                return false;
            }
            if (slash) {
                if (!append_text(candidate, &used, sizeof(candidate), prefix)) {
                    (void)handle_close(directory);
                    return false;
                }
                candidate[base_start] = '\0';
                used = base_start;
            }
            if (!append_text(candidate, &used, sizeof(candidate),
                             batch[index].name)) {
                (void)handle_close(directory);
                return false;
            }
            if (is_directory &&
                !append_text(candidate, &used, sizeof(candidate), "/")) {
                (void)handle_close(directory);
                return false;
            }
            if (!candidate_add(candidates, count, candidate, is_directory,
                               prefix, case_sensitive)) {
                (void)handle_close(directory);
                return false;
            }
        }
    }
    (void)handle_close(directory);
    return true;
}

static usize layout_rows(usize count, usize columns)
{
    return count / columns + (count % columns != 0U);
}

static usize layout_used_columns(usize count, usize columns)
{
    usize rows = layout_rows(count, columns);
    return count / rows + (count % rows != 0U);
}

static usize utf8_display_width(const char *text)
{
    usize width = 0;
    usize offset = 0;
    usize length;

    if (!text) return 0;
    length = strlen(text);
    while (offset < length) {
        u8 value = (u8)text[offset++];
        if (value < 0x80U) {
            width++;
        } else if ((value & 0xE0U) == 0xC0U && offset < length &&
                   ((u8)text[offset] & 0xC0U) == 0x80U) {
            offset++;
            width++;
        } else if ((value & 0xF0U) == 0xE0U && offset + 1U < length &&
                   ((u8)text[offset] & 0xC0U) == 0x80U &&
                   ((u8)text[offset + 1U] & 0xC0U) == 0x80U) {
            offset += 2U;
            width++;
        } else if ((value & 0xF8U) == 0xF0U && offset + 2U < length &&
                   ((u8)text[offset] & 0xC0U) == 0x80U &&
                   ((u8)text[offset + 1U] & 0xC0U) == 0x80U &&
                   ((u8)text[offset + 2U] & 0xC0U) == 0x80U) {
            offset += 3U;
            width++;
        } else {
            width++;
        }
    }
    return width;
}

static usize candidate_width(const completion_candidate_t *candidate)
{
    return candidate ? utf8_display_width(candidate->text) : 0;
}

static usize column_width(const completion_candidate_t *candidates, usize count,
                          usize columns, usize column)
{
    usize rows = layout_rows(count, columns);
    usize width = 0;

    for (usize row = 0; row < rows; row++) {
        usize index = column * rows + row;
        if (index < count && candidate_width(&candidates[index]) > width)
            width = candidate_width(&candidates[index]);
    }
    return width;
}

static bool columns_fit(const completion_candidate_t *candidates, usize count,
                        usize columns, usize width)
{
    usize total = 0;
    usize used = layout_used_columns(count, columns);

    for (usize column = 0; column < used; column++) {
        usize current = column_width(candidates, count, columns, column);
        if (column) total += COMPLETION_COLUMN_GAP;
        total += current;
        if (total > width) return false;
    }
    return true;
}

static usize choose_columns(const completion_candidate_t *candidates,
                            usize count, usize width)
{
    usize columns = count;

    if (!count || !width) return 1U;
    while (columns > 1U && !columns_fit(candidates, count, columns, width))
        columns--;
    return layout_used_columns(count, columns);
}

static bool utf8_next_codepoint(const char *text, usize length,
                                usize *offset, u32 *codepoint)
{
    usize start;
    usize continuation_count;
    u32 value;
    u8 first;

    if (!text || !offset || !codepoint || *offset >= length) return false;
    start = *offset;
    first = (u8)text[(*offset)++];
    if (first < 0x80U) {
        *codepoint = first;
        return true;
    }
    if ((first & 0xE0U) == 0xC0U) {
        value = first & 0x1FU;
        continuation_count = 1U;
    } else if ((first & 0xF0U) == 0xE0U) {
        value = first & 0x0FU;
        continuation_count = 2U;
    } else if ((first & 0xF8U) == 0xF0U) {
        value = first & 0x07U;
        continuation_count = 3U;
    } else {
        *codepoint = 0xFFFDU;
        return true;
    }
    if (*offset + continuation_count > length) {
        *offset = start + 1U;
        *codepoint = 0xFFFDU;
        return true;
    }
    for (usize part = 0; part < continuation_count; part++) {
        u8 continuation = (u8)text[*offset + part];
        if ((continuation & 0xC0U) != 0x80U) {
            *offset = start + 1U;
            *codepoint = 0xFFFDU;
            return true;
        }
        value = (value << 6) | (continuation & 0x3FU);
    }
    *offset += continuation_count;
    *codepoint = value;
    return true;
}

static void overlay_fill(mg_terminal_overlay_cell_t *cells, usize count,
                         mg_terminal_style_t style)
{
    if (!cells) return;
    for (usize index = 0; index < count; index++) {
        cells[index].codepoint = ' ';
        cells[index].foreground = (u8)style.foreground;
        cells[index].background = (u8)style.background;
        cells[index].reserved = 0;
    }
}

static mg_terminal_style_t candidate_style(const completion_candidate_t *candidate,
                                           const shell_state_t *state,
                                           bool styled_output)
{
    if (!styled_output || !candidate)
        return mg_terminal_style_for_role(MG_TERMINAL_STYLE_DEFAULT);
    if (candidate->directory)
        return mg_terminal_style_for_role(MG_TERMINAL_STYLE_DIRECTORY);
    return (mg_terminal_style_t){
        state->config.completion_color,
        MG_TERMINAL_COLOR_BLACK,
    };
}

static bool overlay_write_text(mg_terminal_overlay_cell_t *cells, usize width,
                               usize row, usize column, const char *text,
                               mg_terminal_style_t style)
{
    usize offset = 0;
    usize current = column;
    usize length;
    u32 codepoint;

    if (!cells || !width || !text) return false;
    length = strlen(text);
    while (offset < length) {
        if (current >= width ||
            !utf8_next_codepoint(text, length, &offset, &codepoint))
            return false;
        cells[row * width + current].codepoint = codepoint;
        cells[row * width + current].foreground = (u8)style.foreground;
        cells[row * width + current].background = (u8)style.background;
        current++;
    }
    return true;
}

static usize wrapped_candidate_rows(const completion_candidate_t *candidates,
                                    usize count, usize width)
{
    usize rows = 0;

    if (!width) return 0;
    for (usize index = 0; index < count; index++) {
        usize length = candidate_width(&candidates[index]);
        rows += length / width + (length % width != 0U);
    }
    return rows;
}

static bool overlay_write_wrapped(mg_terminal_overlay_cell_t *cells,
                                  usize width, usize rows,
                                  const completion_candidate_t *candidates,
                                  usize count, const shell_state_t *state,
                                  bool styled_output)
{
    usize row = 0;

    for (usize index = 0; index < count; index++) {
        const char *text = candidates[index].text;
        usize offset = 0;
        usize length = strlen(text);
        mg_terminal_style_t style = candidate_style(&candidates[index], state,
                                                    styled_output);

        while (offset < length) {
            usize column = 0;
            if (row >= rows) return false;
            while (offset < length && column < width) {
                u32 codepoint;
                if (!utf8_next_codepoint(text, length, &offset, &codepoint))
                    return false;
                cells[row * width + column].codepoint = codepoint;
                cells[row * width + column].foreground = (u8)style.foreground;
                cells[row * width + column].background = (u8)style.background;
                column++;
            }
            row++;
        }
    }
    return true;
}

static mg_result_t list_candidates(mg_line_editor_t *editor,
                                   shell_state_t *state,
                                   completion_candidate_t *candidates,
                                   usize count, bool styled_output)
{
    mg_terminal_size_t size = {0};
    usize width = 80U;
    usize columns;
    usize rows;
    usize cell_count;
    bool needs_wrapping = false;
    mg_terminal_overlay_cell_t *cells;
    mg_terminal_style_t default_style;
    mg_result_t result;

    if (terminal_get_size(&size) == MG_OK && size.columns)
        width = size.columns;
    if (width > 256U) width = 256U;
    for (usize index = 0; index < count; index++)
        if (candidate_width(&candidates[index]) > width)
            needs_wrapping = true;
    if (needs_wrapping) {
        columns = 1U;
        rows = wrapped_candidate_rows(candidates, count, width);
    } else {
        columns = choose_columns(candidates, count, width);
        rows = layout_rows(count, columns);
    }
    if (!rows || rows > 128U || !width) return MG_OK;
    cell_count = rows * width;
    cells = (mg_terminal_overlay_cell_t *)calloc(cell_count,
                                                   sizeof(*cells));
    if (!cells) return MG_ERR_NO_MEMORY;
    default_style = mg_terminal_style_for_role(MG_TERMINAL_STYLE_DEFAULT);
    overlay_fill(cells, cell_count, default_style);
    if (needs_wrapping) {
        if (!overlay_write_wrapped(cells, width, rows, candidates, count,
                                   state, styled_output)) {
            free(cells);
            return MG_OK;
        }
    } else {
        for (usize row = 0; row < rows; row++) {
            usize position = 0;
            for (usize column = 0; column < columns; column++) {
                usize index = column * rows + row;
                mg_terminal_style_t style;
                if (index >= count) continue;
                style = candidate_style(&candidates[index], state,
                                        styled_output);
                if (!overlay_write_text(cells, width, row, position,
                                        candidates[index].text, style)) {
                    free(cells);
                    return MG_OK;
                }
                position += column_width(candidates, count, columns, column) +
                            COMPLETION_COLUMN_GAP;
            }
        }
    }
    result = terminal_overlay_set(cells, (u32)rows, (u32)width);
    free(cells);
    if (result == MG_OK)
        line_editor_set_temporary_display(editor, true);
    return MG_OK;
}

static bool replace_editor_range(mg_line_editor_t *editor, usize first,
                                 usize last, const char *replacement)
{
    usize replacement_length;
    usize old_length;

    if (!editor || !replacement || first > last || last > editor->length)
        return false;
    replacement_length = strlen(replacement);
    old_length = last - first;
    if (editor->length - old_length + replacement_length + 1U >=
        editor->capacity) return false;
    memmove(editor->buffer + first + replacement_length,
            editor->buffer + last, editor->length - last + 1U);
    memcpy(editor->buffer + first, replacement, replacement_length);
    editor->length = editor->length - old_length + replacement_length;
    editor->cursor = first + replacement_length;
    editor->selection_anchor = editor->cursor;
    editor->selection_active = false;
    return true;
}

static bool common_prefix(const completion_candidate_t *candidates, usize count,
                          bool case_sensitive, char *out, usize capacity)
{
    usize length;

    if (!candidates || !count || !out || !capacity) return false;
    length = strlen(candidates[0].text);
    for (usize index = 1; index < count; index++) {
        usize current = 0;
        while (current < length && candidates[index].text[current] &&
               chars_equal(candidates[0].text[current],
                           candidates[index].text[current], case_sensitive))
            current++;
        length = current;
    }
    if (length >= capacity) length = capacity - 1U;
    memcpy(out, candidates[0].text, length);
    out[length] = '\0';
    return true;
}

static void remember_completion(shell_state_t *state,
                               const mg_line_editor_t *editor, bool pending)
{
    if (!state || !editor) return;
    strncpy(state->completion_line, editor->buffer,
            sizeof(state->completion_line) - 1U);
    state->completion_line[sizeof(state->completion_line) - 1U] = '\0';
    state->completion_cursor = editor->cursor;
    state->completion_pending = pending;
}

mg_result_t shell_complete_line(mg_line_editor_t *editor, void *context)
{
    shell_state_t *state = (shell_state_t *)context;
    completion_candidate_t *candidates;
    completion_token_t token;
    char prefix[SHOOT_LINE_CAPACITY];
    char command_name[SHOOT_LINE_CAPACITY];
    char common[COMPLETION_TEXT_CAPACITY];
    usize count = 0;
    usize command_start = 0;
    usize prefix_length;
    usize replace_end;
    bool command_token;
    bool case_sensitive;
    mg_terminal_capabilities_t terminal_capabilities = {0};
    bool styled_output;
    shell_completion_kind_t kind = SHELL_COMPLETION_PATH;

    if (!editor || !state || !editor->buffer) return MG_ERR_BAD_ARGUMENT;
    case_sensitive = state->config.completion_case_sensitive;
    styled_output = terminal_get_capabilities(&terminal_capabilities) == MG_OK &&
        (terminal_capabilities.capabilities & MG_TERMINAL_CAP_STYLED_OUTPUT) != 0;
    token_at(editor->buffer, editor->length, editor->cursor, &token);
    if (!current_prefix(editor, &token, prefix, sizeof(prefix)))
        return MG_OK;
    prefix_length = strlen(prefix);
    replace_end = token.value_end;
    if (replace_end < token.value_start) replace_end = token.value_start;
    command_token = first_command(editor->buffer, editor->length,
                                  command_name, sizeof(command_name),
                                  &command_start) &&
                   token.raw_start == command_start;

    if (!command_token && first_command(editor->buffer, editor->length,
                                        command_name, sizeof(command_name),
                                        &command_start)) {
        const shell_command_info_t *builtin = find_builtin(command_name);
        if (builtin) kind = builtin->completion_kind;
    }
    if (command_token) kind = SHELL_COMPLETION_COMMAND;
    if (kind == SHELL_COMPLETION_NONE) {
        remember_completion(state, editor, false);
        return MG_OK;
    }

    candidates = (completion_candidate_t *)calloc(
        COMPLETION_MAX_CANDIDATES, sizeof(*candidates));
    if (!candidates) return MG_ERR_NO_MEMORY;
    if (kind == SHELL_COMPLETION_COMMAND)
        (void)collect_commands(candidates, &count, prefix, case_sensitive);
    else
        (void)collect_paths(prefix, kind == SHELL_COMPLETION_DIRECTORY,
                             candidates, &count, case_sensitive);
    sort_candidates(candidates, count);

    if (!count) {
        remember_completion(state, editor, false);
        free(candidates);
        return MG_OK;
    }
    if (count == 1U) {
        if (!replace_editor_range(editor, token.value_start, replace_end,
                                  candidates[0].text)) {
            free(candidates);
            return MG_OK;
        }
        remember_completion(state, editor, false);
        free(candidates);
        return MG_OK;
    }
    (void)common_prefix(candidates, count, case_sensitive, common,
                        sizeof(common));
    if (strlen(common) > prefix_length) {
        if (replace_editor_range(editor, token.value_start, replace_end,
                                 common))
            remember_completion(state, editor, true);
        free(candidates);
        return MG_OK;
    }
    if (state->completion_pending &&
        state->completion_cursor == editor->cursor &&
        !strcmp(state->completion_line, editor->buffer)) {
        mg_result_t result = list_candidates(editor, state, candidates, count,
                                             styled_output);
        /* Keep the same completion active so a repeated Tab can refresh the
         * ephemeral candidate rows without changing the input buffer. */
        remember_completion(state, editor, true);
        free(candidates);
        return result;
    }
    remember_completion(state, editor, true);
    free(candidates);
    return MG_OK;
}
