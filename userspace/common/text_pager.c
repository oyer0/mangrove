/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "text_pager.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <mg/terminal.h>
#include <mg/utf8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGER_MAX_COLUMNS 256U
#define PAGER_MAX_ROWS 128U
#define PAGER_TAB_WIDTH 4U
#define PAGER_FILE_CHUNK 4096U
#define PAGER_STATUS_CAPACITY 384U
#define PAGER_SCREEN_ROWS 128U
#define PAGER_SCREEN_COLUMNS 256U

typedef struct text_pager_cell {
    u32 codepoint;
    u8 foreground;
    u8 background;
    u16 reserved;
} text_pager_cell_t;

typedef struct text_pager_runtime {
    text_pager_document_t document;
    const char *title;
    const char *path;
    u32 screen_rows;
    u32 screen_columns;
    usize top_row;
    usize anchor_top_row;
    usize anchor_offset;
    usize current_match;
    bool searching;
    bool search_prompt;
    bool has_current_match;
    char query[TEXT_PAGER_MAX_QUERY_BYTES + 1U];
    usize query_length;
    char status[PAGER_STATUS_CAPACITY];
    text_pager_cell_t *frame;
    text_pager_cell_t *previous_frame;
    bool frame_valid;
} text_pager_runtime_t;

static bool pager_decode_cell(const u8 *text, usize length, usize offset,
                              u32 *out_codepoint, usize *out_consumed)
{
    u32 codepoint;
    usize consumed;

    if (!text || offset >= length || !out_codepoint || !out_consumed)
        return false;
    if (mg_utf8_decode_one(text + offset, length - offset, &codepoint,
                          &consumed) != MG_UTF8_OK) {
        *out_codepoint = 0xFFFDU;
        *out_consumed = 1U;
        return true;
    }
    if (codepoint == 0U || (codepoint < 0x20U && codepoint != '\n' &&
                            codepoint != '\t') || codepoint == 0x7FU)
        codepoint = 0xFFFDU;
    *out_codepoint = codepoint;
    *out_consumed = consumed;
    return true;
}

static mg_result_t pager_append_row(text_pager_document_t *document,
                                    usize *capacity, u32 offset,
                                    u32 tab_remaining, u32 logical_column,
                                    u32 cells)
{
    text_pager_visual_row_t *replacement;
    usize next_capacity;

    if (!document || !capacity || document->row_count >=
        TEXT_PAGER_MAX_VISUAL_ROWS) return MG_ERR_BUFFER_TOO_SMALL;
    if (document->row_count == *capacity) {
        next_capacity = *capacity ? *capacity * 2U : 128U;
        if (next_capacity > TEXT_PAGER_MAX_VISUAL_ROWS)
            next_capacity = TEXT_PAGER_MAX_VISUAL_ROWS;
        if (next_capacity <= document->row_count ||
            next_capacity > (usize)-1 / sizeof(*document->rows))
            return MG_ERR_BUFFER_TOO_SMALL;
        replacement = realloc(document->rows,
                              next_capacity * sizeof(*document->rows));
        if (!replacement) return MG_ERR_NO_MEMORY;
        document->rows = replacement;
        *capacity = next_capacity;
    }
    document->rows[document->row_count++] = (text_pager_visual_row_t){
        .byte_offset = offset,
        .tab_remaining = tab_remaining,
        .logical_column = logical_column,
        .cells = cells,
    };
    return MG_OK;
}

static mg_result_t pager_build_layout(text_pager_document_t *document,
                                     u32 columns)
{
    usize capacity = 0U;
    u32 offset = 0U;
    u32 row_offset = 0U;
    u32 row_tab_remaining = 0U;
    u32 row_logical_column = 0U;
    u32 row_cells = 0U;
    u32 tab_remaining = 0U;
    u32 logical_column = 0U;

    if (!document || !document->text || document->length >
        TEXT_PAGER_MAX_DOCUMENT_BYTES || !columns ||
        columns > PAGER_MAX_COLUMNS) return MG_ERR_BAD_ARGUMENT;
    document->columns = columns;
    document->row_count = 0U;
    free(document->rows);
    document->rows = NULL;

    while ((usize)offset < document->length) {
        u32 codepoint = 0U;
        usize consumed = 0U;

        if (!tab_remaining && document->text[offset] == '\r' &&
            (usize)offset + 1U < document->length &&
            document->text[offset + 1U] == '\n') {
            offset++;
            continue;
        }
        if (!tab_remaining && document->text[offset] == '\n') {
            mg_result_t result = pager_append_row(document, &capacity,
                row_offset, row_tab_remaining, row_logical_column, row_cells);
            if (result != MG_OK) return result;
            offset++;
            row_offset = offset;
            row_tab_remaining = 0U;
            row_logical_column = 0U;
            row_cells = 0U;
            tab_remaining = 0U;
            logical_column = 0U;
            continue;
        }
        if (row_cells == columns) {
            mg_result_t result = pager_append_row(document, &capacity,
                row_offset, row_tab_remaining, row_logical_column, row_cells);
            if (result != MG_OK) return result;
            row_offset = offset;
            row_tab_remaining = tab_remaining;
            row_logical_column = logical_column;
            row_cells = 0U;
            continue;
        }
        if (!tab_remaining) {
            if (!pager_decode_cell(document->text, document->length, offset,
                                   &codepoint, &consumed))
                return MG_ERR_BAD_ARGUMENT;
            if (codepoint == '\t')
                tab_remaining = document->tab_width -
                                logical_column % document->tab_width;
        }
        if (tab_remaining) {
            tab_remaining--;
            logical_column++;
            row_cells++;
            if (!tab_remaining) offset++;
        } else {
            offset += (u32)consumed;
            logical_column++;
            row_cells++;
        }
    }
    return pager_append_row(document, &capacity, row_offset,
                            row_tab_remaining, row_logical_column, row_cells);
}

mg_result_t text_pager_document_init(text_pager_document_t *document,
                                     const void *text, usize length,
                                     u32 columns, u32 tab_width)
{
    text_pager_document_t candidate = {0};
    mg_result_t result;

    if (!document || (!text && length) || length >
        TEXT_PAGER_MAX_DOCUMENT_BYTES || !tab_width || tab_width > 16U)
        return MG_ERR_BAD_ARGUMENT;
    candidate.text = (const u8 *)text;
    candidate.length = length;
    candidate.tab_width = tab_width;
    if (!candidate.text) candidate.text = (const u8 *)"";
    result = pager_build_layout(&candidate, columns);
    if (result != MG_OK) {
        free(candidate.rows);
        return result;
    }
    *document = candidate;
    return MG_OK;
}

mg_result_t text_pager_document_relayout(text_pager_document_t *document,
                                         u32 columns)
{
    text_pager_document_t candidate;
    mg_result_t result;

    if (!document) return MG_ERR_BAD_ARGUMENT;
    candidate = *document;
    candidate.rows = NULL;
    candidate.row_count = 0U;
    result = pager_build_layout(&candidate, columns);
    if (result != MG_OK) {
        free(candidate.rows);
        return result;
    }
    free(document->rows);
    *document = candidate;
    return MG_OK;
}

void text_pager_document_destroy(text_pager_document_t *document)
{
    if (!document) return;
    free(document->rows);
    memset(document, 0, sizeof(*document));
}

mg_result_t text_pager_read_file(const char *path, char **out_text,
                                 usize *out_length)
{
    mg_path_info_t info;
    mg_handle_t handle = 0;
    char *text = NULL;
    usize length = 0U;
    mg_result_t result;

    if (!path || !out_text || !out_length) return MG_ERR_BAD_ARGUMENT;
    *out_text = NULL;
    *out_length = 0U;
    result = path_info(path, &info);
    if (result != MG_OK) return result;
    if (info.type != MG_PATH_TYPE_FILE) return MG_ERR_BAD_ARGUMENT;
    if (info.size > TEXT_PAGER_MAX_DOCUMENT_BYTES)
        return MG_ERR_BUFFER_TOO_SMALL;
    text = malloc((usize)info.size + 1U);
    if (!text) return MG_ERR_NO_MEMORY;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) {
        free(text);
        return result;
    }
    handle = (mg_handle_t)result;

    while (length < (usize)info.size) {
        usize request = (usize)info.size - length;
        if (request > PAGER_FILE_CHUNK) request = PAGER_FILE_CHUNK;
        result = object_read(handle, text + length, request);
        if (result <= 0 || (usize)result > request) {
            result = result < 0 ? result : MG_ERR_IO;
            goto failed;
        }
        length += (usize)result;
    }
    {
        u8 extra;
        result = object_read(handle, &extra, 1U);
        if (result != MG_ERR_END_OF_FILE && result != 0) {
            result = result < 0 ? result : MG_ERR_BUSY;
            goto failed;
        }
    }
    result = handle_close(handle);
    handle = 0;
    if (result < 0) goto failed;
    text[length] = '\0';
    *out_text = text;
    *out_length = length;
    return MG_OK;

failed:
    if (handle) (void)handle_close(handle);
    free(text);
    return result;
}

static bool pager_query_match_at(const text_pager_document_t *document,
                                 const char *query, usize query_length,
                                 usize offset)
{
    return document && query && query_length &&
           offset <= document->length &&
           query_length <= document->length - offset &&
           memcmp(document->text + offset, query, query_length) == 0;
}

static usize pager_find_next_match(const text_pager_document_t *document,
                                   const char *query, usize query_length,
                                   usize start)
{
    if (!document || !query || !query_length || start > document->length ||
        query_length > document->length - start) return (usize)-1;
    for (usize position = start;
         position <= document->length - query_length; position++)
        if (pager_query_match_at(document, query, query_length, position))
            return position;
    return (usize)-1;
}

static usize pager_find_next_match_before(
    const text_pager_document_t *document, const char *query,
    usize query_length, usize start, usize limit)
{
    if (!document || !query || !query_length || start > document->length ||
        query_length > document->length - start) return (usize)-1;
    if (limit > document->length - query_length)
        limit = document->length - query_length + 1U;
    for (usize position = start; position < limit; position++)
        if (pager_query_match_at(document, query, query_length, position))
            return position;
    return (usize)-1;
}

static usize pager_find_previous_match(const text_pager_document_t *document,
                                       const char *query, usize query_length,
                                       usize before)
{
    usize found = (usize)-1;

    if (!document || !query || !query_length || before > document->length)
        return found;
    for (usize position = 0; position < before; position++)
        if (pager_query_match_at(document, query, query_length, position))
            found = position;
    return found;
}

static usize pager_find_closest_match(const text_pager_document_t *document,
                                      const char *query, usize query_length,
                                      usize anchor)
{
    usize best = (usize)-1;
    usize best_distance = (usize)-1;
    usize position = 0U;

    while ((position = pager_find_next_match(document, query, query_length,
                                              position)) != (usize)-1) {
        usize distance = position > anchor ? position - anchor : anchor - position;
        if (distance < best_distance ||
            (distance == best_distance && position > best)) {
            best = position;
            best_distance = distance;
        }
        position++;
    }
    return best;
}

static usize pager_row_for_offset(const text_pager_document_t *document,
                                  usize offset)
{
    usize low = 0U;
    usize high;

    if (!document || !document->row_count) return 0U;
    high = document->row_count;
    while (low + 1U < high) {
        usize middle = low + (high - low) / 2U;
        if (document->rows[middle].byte_offset <= offset) low = middle;
        else high = middle;
    }
    return low;
}

static usize pager_max_top(const text_pager_runtime_t *runtime)
{
    usize visible;

    if (!runtime || runtime->screen_rows < 2U) return 0U;
    visible = runtime->screen_rows - 1U;
    return runtime->document.row_count > visible
        ? runtime->document.row_count - visible : 0U;
}

static void pager_set_top(text_pager_runtime_t *runtime, usize row)
{
    if (!runtime) return;
    runtime->top_row = row > pager_max_top(runtime)
        ? pager_max_top(runtime) : row;
}

static void pager_frame_fill(text_pager_runtime_t *runtime,
                             text_pager_cell_t *frame)
{
    usize count = (usize)runtime->screen_rows * runtime->screen_columns;
    for (usize index = 0; index < count; index++) {
        frame[index] = (text_pager_cell_t){
            .codepoint = ' ',
            .foreground = MG_TERMINAL_COLOR_LIGHT_GRAY,
            .background = MG_TERMINAL_COLOR_BLACK,
        };
    }
}

static void pager_render_text_row(text_pager_runtime_t *runtime,
                                  text_pager_cell_t *frame, u32 screen_row,
                                  usize visual_row)
{
    const text_pager_visual_row_t *row;
    usize offset;
    u32 tab_remaining;
    u32 logical_column;
    usize cell_index;
    usize search_limit;
    const char *query;
    usize query_length;
    usize next_match;

    if (!runtime || visual_row >= runtime->document.row_count) return;
    row = &runtime->document.rows[visual_row];
    offset = row->byte_offset;
    tab_remaining = row->tab_remaining;
    logical_column = row->logical_column;
    cell_index = (usize)screen_row * runtime->screen_columns;
    query = runtime->searching || runtime->search_prompt
        ? runtime->query : NULL;
    query_length = query ? runtime->query_length : 0U;
    search_limit = visual_row + 1U < runtime->document.row_count
        ? runtime->document.rows[visual_row + 1U].byte_offset
        : runtime->document.length;
    next_match = pager_find_next_match_before(&runtime->document, query,
        query_length, offset >= query_length ? offset - query_length + 1U : 0U,
        search_limit);

    for (u32 cell = 0; cell < row->cells && cell < runtime->screen_columns;
         cell++) {
        u32 codepoint = 0U;
        usize consumed = 0U;
        usize source_offset = offset;
        bool highlighted;

        if (!tab_remaining) {
            if (!pager_decode_cell(runtime->document.text,
                                   runtime->document.length, offset,
                                   &codepoint, &consumed)) break;
            if (codepoint == '\t')
                tab_remaining = runtime->document.tab_width -
                    logical_column % runtime->document.tab_width;
        }
        if (tab_remaining) {
            codepoint = ' ';
            tab_remaining--;
            logical_column++;
            if (!tab_remaining) offset++;
        } else {
            offset += consumed;
            logical_column++;
        }
        while (next_match != (usize)-1 &&
               next_match + query_length <= source_offset)
            next_match = pager_find_next_match_before(&runtime->document,
                query, query_length, next_match + 1U, search_limit);
        highlighted = next_match != (usize)-1 &&
                      next_match <= source_offset &&
                      source_offset < next_match + query_length;
        if (highlighted) {
            frame[cell_index + cell].foreground = MG_TERMINAL_COLOR_BLACK;
            frame[cell_index + cell].background =
                MG_TERMINAL_COLOR_BRIGHT_YELLOW;
        }
        frame[cell_index + cell].codepoint = codepoint;
    }
}

static void pager_render_status(text_pager_runtime_t *runtime,
                                text_pager_cell_t *frame)
{
    char status[PAGER_STATUS_CAPACITY];
    usize row_index = runtime->screen_rows - 1U;
    usize cell_index = row_index * runtime->screen_columns;
    const char *text;
    int length;
    usize offset = 0U;
    u32 cell = 0U;

    if (runtime->search_prompt) {
        length = snprintf(status, sizeof(status), "Find: %s", runtime->query);
        text = status;
    } else if (runtime->status[0]) {
        length = snprintf(status, sizeof(status), "%s", runtime->status);
        text = status;
    } else {
        usize visible = runtime->screen_rows - 1U;
        usize end_row = runtime->top_row + visible;
        if (end_row > runtime->document.row_count)
            end_row = runtime->document.row_count;
        length = snprintf(status, sizeof(status), "%s  visual row %llu-%llu/%llu  j/k PgUp/PgDn g/G ? q",
            runtime->title ? runtime->title : "Text",
            (unsigned long long)(runtime->top_row + 1U),
            (unsigned long long)end_row,
            (unsigned long long)runtime->document.row_count);
        text = status;
    }
    if (length < 0) return;
    if ((usize)length >= sizeof(status)) length = (int)sizeof(status) - 1;
    for (usize index = 0; index < runtime->screen_columns; index++) {
        frame[cell_index + index] = (text_pager_cell_t){
            .codepoint = ' ',
            .foreground = MG_TERMINAL_COLOR_WHITE,
            .background = MG_TERMINAL_COLOR_BLUE,
        };
    }
    while (offset < (usize)length && cell < runtime->screen_columns) {
        u32 codepoint;
        usize consumed;
        if (mg_utf8_decode_display_one((const u8 *)text + offset,
                (usize)length - offset, &codepoint, &consumed) != MG_UTF8_OK) {
            codepoint = 0xFFFDU;
            consumed = 1U;
        }
        frame[cell_index + cell].codepoint = codepoint;
        offset += consumed;
        cell++;
    }
}

static bool pager_build_frame(text_pager_runtime_t *runtime)
{
    pager_frame_fill(runtime, runtime->frame);
    for (u32 row = 0; row + 1U < runtime->screen_rows; row++) {
        usize visual_row = runtime->top_row + row;
        if (visual_row < runtime->document.row_count)
            pager_render_text_row(runtime, runtime->frame, row, visual_row);
    }
    pager_render_status(runtime, runtime->frame);
    return true;
}

static bool pager_cells_equal(const text_pager_cell_t *left,
                              const text_pager_cell_t *right, usize count)
{
    return memcmp(left, right, count * sizeof(*left)) == 0;
}

static bool pager_flush_frame(text_pager_runtime_t *runtime)
{
    usize cells_per_row = runtime->screen_columns;
    bool any_changed = !runtime->frame_valid;
    mg_result_t result;

    if (!any_changed) {
        for (u32 row = 0; row < runtime->screen_rows; row++) {
            usize first = (usize)row * cells_per_row;
            if (!pager_cells_equal(runtime->frame + first,
                                   runtime->previous_frame + first,
                                   cells_per_row)) {
                any_changed = true;
                break;
            }
        }
    }
    if (!any_changed) return true;
    result = terminal_update_begin();
    if (result != MG_OK) return false;
    for (u32 row = 0; row < runtime->screen_rows; row++) {
        usize first = (usize)row * cells_per_row;
        u32 column = 0U;

        if (runtime->frame_valid && pager_cells_equal(
                runtime->frame + first, runtime->previous_frame + first,
                cells_per_row)) continue;
        if (terminal_cursor_move(row, 0U) != MG_OK ||
            terminal_clear_line() != MG_OK) {
            (void)terminal_update_end();
            return false;
        }
        while (column < runtime->screen_columns) {
            char encoded[PAGER_SCREEN_COLUMNS * 4U];
            usize encoded_length = 0U;
            u8 foreground = runtime->frame[first + column].foreground;
            u8 background = runtime->frame[first + column].background;

            while (column < runtime->screen_columns &&
                   runtime->frame[first + column].foreground == foreground &&
                   runtime->frame[first + column].background == background) {
                u8 scalar[4];
                usize scalar_length = 0U;
                if (mg_utf8_encode_one(runtime->frame[first + column].codepoint,
                        scalar, sizeof(scalar), &scalar_length) != MG_UTF8_OK ||
                    scalar_length > sizeof(encoded) - encoded_length) {
                    scalar[0] = '?';
                    scalar_length = 1U;
                }
                memcpy(encoded + encoded_length, scalar, scalar_length);
                encoded_length += scalar_length;
                column++;
            }
            if (encoded_length && terminal_write_styled(encoded, encoded_length,
                    (mg_terminal_color_t)foreground,
                    (mg_terminal_color_t)background) !=
                    (mg_result_t)encoded_length) {
                (void)terminal_update_end();
                return false;
            }
        }
    }
    result = terminal_update_end();
    if (result != MG_OK) return false;
    memcpy(runtime->previous_frame, runtime->frame,
           (usize)runtime->screen_rows * runtime->screen_columns *
               sizeof(*runtime->frame));
    runtime->frame_valid = true;
    return true;
}

static bool pager_resize(text_pager_runtime_t *runtime, u32 rows, u32 columns)
{
    usize cell_count;
    text_pager_cell_t *frame;
    text_pager_cell_t *previous;
    mg_result_t result;

    if (!runtime || rows < 2U || rows > PAGER_SCREEN_ROWS || !columns ||
        columns > PAGER_SCREEN_COLUMNS) return false;
    if (runtime->screen_rows == rows && runtime->screen_columns == columns)
        return true;
    if (runtime->document.columns != columns) {
        result = text_pager_document_relayout(&runtime->document, columns);
        if (result != MG_OK) return false;
    }
    if ((usize)rows > (usize)-1 / columns) return false;
    cell_count = (usize)rows * columns;
    if (cell_count > (usize)-1 / sizeof(*frame)) return false;
    frame = calloc(cell_count, sizeof(*frame));
    previous = calloc(cell_count, sizeof(*previous));
    if (!frame || !previous) {
        free(frame);
        free(previous);
        return false;
    }
    free(runtime->frame);
    free(runtime->previous_frame);
    runtime->frame = frame;
    runtime->previous_frame = previous;
    runtime->screen_rows = rows;
    runtime->screen_columns = columns;
    runtime->frame_valid = false;
    pager_set_top(runtime, runtime->top_row);
    return true;
}

static void pager_set_query_byte(text_pager_runtime_t *runtime,
                                 const u8 *encoded, usize length)
{
    if (!runtime || !encoded || !length ||
        length > TEXT_PAGER_MAX_QUERY_BYTES - runtime->query_length) return;
    memcpy(runtime->query + runtime->query_length, encoded, length);
    runtime->query_length += length;
    runtime->query[runtime->query_length] = '\0';
    runtime->status[0] = '\0';
}

static void pager_query_backspace(text_pager_runtime_t *runtime)
{
    usize start;

    if (!runtime || !runtime->query_length) return;
    start = runtime->query_length - 1U;
    while (start && ((u8)runtime->query[start] & 0xC0U) == 0x80U) start--;
    runtime->query_length = start;
    runtime->query[start] = '\0';
    runtime->status[0] = '\0';
}

static void pager_begin_search(text_pager_runtime_t *runtime)
{
    runtime->search_prompt = true;
    runtime->searching = false;
    runtime->query_length = 0U;
    runtime->query[0] = '\0';
    runtime->anchor_top_row = runtime->top_row;
    runtime->anchor_offset = runtime->document.rows[runtime->top_row].byte_offset;
    runtime->has_current_match = false;
    runtime->status[0] = '\0';
}

static void pager_accept_search(text_pager_runtime_t *runtime)
{
    usize match;

    runtime->search_prompt = false;
    if (!runtime->query_length) {
        runtime->status[0] = '\0';
        return;
    }
    match = pager_find_closest_match(&runtime->document, runtime->query,
                                     runtime->query_length,
                                     runtime->anchor_offset);
    if (match == (usize)-1) {
        runtime->searching = false;
        runtime->has_current_match = false;
        pager_set_top(runtime, runtime->anchor_top_row);
        (void)snprintf(runtime->status, sizeof(runtime->status),
                       "No matches for: %s", runtime->query);
        return;
    }
    runtime->searching = true;
    runtime->has_current_match = true;
    runtime->current_match = match;
    pager_set_top(runtime, pager_row_for_offset(&runtime->document, match));
    runtime->status[0] = '\0';
}

static bool pager_search_move(text_pager_runtime_t *runtime, bool forward)
{
    usize match;

    if (!runtime || !runtime->searching || !runtime->query_length ||
        !runtime->has_current_match) return false;
    if (forward) {
        usize start = runtime->current_match + runtime->query_length;
        match = pager_find_next_match(&runtime->document, runtime->query,
                                      runtime->query_length, start);
    } else {
        match = pager_find_previous_match(&runtime->document, runtime->query,
                                          runtime->query_length,
                                          runtime->current_match);
    }
    if (match == (usize)-1) return false;
    runtime->current_match = match;
    pager_set_top(runtime, pager_row_for_offset(&runtime->document, match));
    return true;
}

static bool pager_is_modifier_event(const mg_terminal_key_event_t *event)
{
    return event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_CTRL |
        MG_TERMINAL_MODIFIER_RIGHT_CTRL | MG_TERMINAL_MODIFIER_LEFT_ALT |
        MG_TERMINAL_MODIFIER_RIGHT_ALT | MG_TERMINAL_MODIFIER_LEFT_GUI |
        MG_TERMINAL_MODIFIER_RIGHT_GUI);
}

static bool pager_handle_search_prompt(text_pager_runtime_t *runtime,
                                       const mg_terminal_key_event_t *event)
{
    if (event->keycode == MG_TERMINAL_KEY_ESCAPE) {
        runtime->search_prompt = false;
        runtime->query_length = 0U;
        runtime->query[0] = '\0';
        runtime->searching = false;
        runtime->has_current_match = false;
        pager_set_top(runtime, runtime->anchor_top_row);
        runtime->status[0] = '\0';
        return false;
    }
    if (event->keycode == MG_TERMINAL_KEY_ENTER) {
        pager_accept_search(runtime);
        return false;
    }
    if (event->keycode == MG_TERMINAL_KEY_BACKSPACE ||
        event->codepoint == '\b') {
        pager_query_backspace(runtime);
        return false;
    }
    if (pager_is_modifier_event(event) || !event->codepoint ||
        event->codepoint < 0x20U || event->codepoint == 0x7FU) return false;
    {
        u8 encoded[4];
        usize encoded_length = 0U;
        if (mg_utf8_encode_one(event->codepoint, encoded, sizeof(encoded),
                               &encoded_length) == MG_UTF8_OK)
            pager_set_query_byte(runtime, encoded, encoded_length);
    }
    return false;
}

static bool pager_handle_event(text_pager_runtime_t *runtime,
                               const mg_terminal_key_event_t *event)
{
    usize page;
    bool handled = true;

    if (!runtime || !event) return false;
    if (runtime->search_prompt)
        return pager_handle_search_prompt(runtime, event);
    /* Application navigation keys are intentionally unmodified. In
     * particular, do not treat an Alt/Ctrl/GUI chord that happens to carry
     * the same printable codepoint as a pager command. */
    if (pager_is_modifier_event(event)) return false;
    if (event->keycode == MG_TERMINAL_KEY_ESCAPE) {
        if (runtime->searching) {
            runtime->searching = false;
            runtime->has_current_match = false;
            runtime->query_length = 0U;
            runtime->query[0] = '\0';
            runtime->status[0] = '\0';
            return false;
        }
        return true;
    }
    if (event->codepoint == 'q') return true;
    if (event->codepoint == '?' ||
        (event->keycode == 0x38U &&
         (event->modifiers & (MG_TERMINAL_MODIFIER_LEFT_SHIFT |
                              MG_TERMINAL_MODIFIER_RIGHT_SHIFT)))) {
        pager_begin_search(runtime);
        return false;
    }
    if (runtime->searching && !pager_is_modifier_event(event) &&
        event->codepoint == 'n') {
        if (!pager_search_move(runtime, true))
            (void)snprintf(runtime->status, sizeof(runtime->status),
                           "No further match.");
        else runtime->status[0] = '\0';
        return false;
    }
    if (runtime->searching && !pager_is_modifier_event(event) &&
        event->codepoint == 'b') {
        if (!pager_search_move(runtime, false))
            (void)snprintf(runtime->status, sizeof(runtime->status),
                           "No earlier match.");
        else runtime->status[0] = '\0';
        return false;
    }

    page = runtime->screen_rows - 1U;
    if (event->keycode == MG_TERMINAL_KEY_DOWN || event->codepoint == 'j') {
        pager_set_top(runtime, runtime->top_row + 1U);
    } else if (event->keycode == MG_TERMINAL_KEY_UP ||
               event->codepoint == 'k') {
        pager_set_top(runtime, runtime->top_row ? runtime->top_row - 1U : 0U);
    } else if (event->keycode == MG_TERMINAL_KEY_PAGE_DOWN ||
               event->codepoint == ' ') {
        pager_set_top(runtime, runtime->top_row + page);
    } else if (event->keycode == MG_TERMINAL_KEY_PAGE_UP) {
        pager_set_top(runtime, runtime->top_row > page
            ? runtime->top_row - page : 0U);
    } else if (event->keycode == MG_TERMINAL_KEY_HOME ||
               event->codepoint == 'g') {
        pager_set_top(runtime, 0U);
    } else if (event->keycode == MG_TERMINAL_KEY_END ||
               event->codepoint == 'G') {
        pager_set_top(runtime, pager_max_top(runtime));
    } else {
        handled = false;
    }
    if (handled) runtime->status[0] = '\0';
    return false;
}

mg_result_t text_pager_run(const char *title, const char *path,
                           const void *text, usize length)
{
    text_pager_runtime_t runtime = {0};
    mg_terminal_size_t size;
    mg_terminal_key_event_t event;
    mg_result_t result;
    bool entered = false;
    bool raw = false;
    bool failed = false;

    if ((!text && length) || length > TEXT_PAGER_MAX_DOCUMENT_BYTES)
        return MG_ERR_BAD_ARGUMENT;
    runtime.title = title;
    runtime.path = path;
    result = terminal_get_size(&size);
    if (result != MG_OK || size.version != MG_TERMINAL_API_VERSION ||
        size.rows < 2U || size.rows > PAGER_SCREEN_ROWS || !size.columns ||
        size.columns > PAGER_SCREEN_COLUMNS) return MG_ERR_UNSUPPORTED;
    result = text_pager_document_init(&runtime.document, text, length,
                                      size.columns, PAGER_TAB_WIDTH);
    if (result != MG_OK) return result;
    if (!pager_resize(&runtime, size.rows, size.columns)) {
        text_pager_document_destroy(&runtime.document);
        return MG_ERR_NO_MEMORY;
    }
    result = terminal_alternate_enter();
    if (result != MG_OK) goto cleanup;
    entered = true;
    result = terminal_input_raw(true);
    if (result != MG_OK) goto cleanup;
    raw = true;
    (void)terminal_cursor_set_visible(false);

    for (;;) {
        result = terminal_get_size(&size);
        if (result != MG_OK || size.version != MG_TERMINAL_API_VERSION ||
            !pager_resize(&runtime, size.rows, size.columns)) {
            result = MG_ERR_UNSUPPORTED;
            failed = true;
            break;
        }
        if (!pager_build_frame(&runtime) || !pager_flush_frame(&runtime)) {
            result = MG_ERR_IO;
            failed = true;
            break;
        }
        result = terminal_read_key_event(250U, &event);
        if (result == MG_ERR_TIMEOUT || result == MG_ERR_WOULD_BLOCK) continue;
        if (result != MG_OK) {
            failed = true;
            break;
        }
        if (event.version != MG_TERMINAL_API_VERSION ||
            event.flags & ~(MG_TERMINAL_KEY_EVENT_REPEAT |
                            MG_TERMINAL_KEY_EVENT_COOKED_BYTE)) continue;
        if (pager_handle_event(&runtime, &event)) {
            result = MG_OK;
            break;
        }
    }

cleanup:
    if (raw && terminal_input_raw(false) != MG_OK) failed = true;
    if (entered) (void)terminal_cursor_set_visible(true);
    if (entered && terminal_alternate_leave() != MG_OK) failed = true;
    free(runtime.frame);
    free(runtime.previous_frame);
    text_pager_document_destroy(&runtime.document);
    return failed ? (result == MG_OK ? MG_ERR_IO : result) : result;
}
