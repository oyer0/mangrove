/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "buffer.h"

#include <stdlib.h>
#include <string.h>
#include <mg/utf8.h>

#define REED_INITIAL_CAPACITY 1024U

static usize gap_length(const reed_buffer_t *buffer)
{
    return buffer->gap_end - buffer->gap_start;
}

usize reed_buffer_length(const reed_buffer_t *buffer)
{
    if (!buffer || !buffer->storage || buffer->gap_end < buffer->gap_start)
        return 0;
    return buffer->capacity - gap_length(buffer);
}

static bool utf8_decode_at(const char *text, usize length, usize position,
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

bool reed_buffer_valid_utf8(const char *text, usize length)
{
    usize position = 0;

    if (length && !text) return false;
    while (position < length) {
        if (!utf8_decode_at(text, length, position, NULL, &position))
            return false;
    }
    return true;
}

static char buffer_byte(const reed_buffer_t *buffer, usize position)
{
    if (position < buffer->gap_start)
        return buffer->storage[position];
    return buffer->storage[position + gap_length(buffer)];
}

static bool buffer_codepoint_boundary(const reed_buffer_t *buffer,
                                      usize position)
{
    usize length = reed_buffer_length(buffer);
    if (!buffer || position > length) return false;
    return position == length ||
        (((u8)buffer_byte(buffer, position) & 0xc0U) != 0x80U);
}

bool reed_buffer_copy_slice(const reed_buffer_t *buffer, usize position,
                            char *output, usize length)
{
    if (!buffer || !output || position > reed_buffer_length(buffer) ||
        length > reed_buffer_length(buffer) - position) return false;
    for (usize index = 0; index < length; index++)
        output[index] = buffer_byte(buffer, position + index);
    return true;
}

bool reed_buffer_copy(const reed_buffer_t *buffer, char *output,
                      usize capacity, usize *out_length)
{
    usize length;

    if (!buffer || !output || !capacity) return false;
    length = reed_buffer_length(buffer);
    if (length + 1U > capacity) {
        if (out_length) *out_length = length;
        return false;
    }
    if (length) {
        if (!reed_buffer_copy_slice(buffer, 0, output, length)) return false;
    }
    output[length] = '\0';
    if (out_length) *out_length = length;
    return true;
}

bool reed_buffer_copy_allocated(const reed_buffer_t *buffer, char **out_text,
                                usize *out_length)
{
    char *text;
    usize length;

    if (!buffer || !out_text) return false;
    length = reed_buffer_length(buffer);
    text = (char *)malloc(length + 1U);
    if (!text) return false;
    if (length && !reed_buffer_copy_slice(buffer, 0, text, length)) {
        free(text);
        return false;
    }
    text[length] = '\0';
    *out_text = text;
    if (out_length) *out_length = length;
    return true;
}

static bool ensure_gap(reed_buffer_t *buffer, usize needed)
{
    usize new_capacity;
    usize length;
    char *replacement;

    if (!buffer || needed <= gap_length(buffer)) return true;
    length = reed_buffer_length(buffer);
    if (length > REED_MAX_BUFFER_BYTES ||
        needed > REED_MAX_BUFFER_BYTES - length)
        return false;
    new_capacity = buffer->capacity ? buffer->capacity : REED_INITIAL_CAPACITY;
    while (new_capacity - length < needed) {
        if (new_capacity >= REED_MAX_BUFFER_BYTES / 2U) {
            new_capacity = REED_MAX_BUFFER_BYTES;
            break;
        }
        new_capacity *= 2U;
    }
    if (new_capacity < length + needed) return false;
    replacement = (char *)malloc(new_capacity);
    if (!replacement) return false;
    if (buffer->gap_start)
        memcpy(replacement, buffer->storage, buffer->gap_start);
    if (buffer->capacity - buffer->gap_end)
        memcpy(replacement + new_capacity -
                   (buffer->capacity - buffer->gap_end),
               buffer->storage + buffer->gap_end,
               buffer->capacity - buffer->gap_end);
    free(buffer->storage);
    buffer->storage = replacement;
    buffer->gap_end = new_capacity - (buffer->capacity - buffer->gap_end);
    buffer->capacity = new_capacity;
    return true;
}

bool reed_buffer_reserve_insert(reed_buffer_t *buffer, usize position,
                                usize length)
{
    if (!buffer || !buffer_codepoint_boundary(buffer, position)) return false;
    return ensure_gap(buffer, length);
}

static bool move_gap(reed_buffer_t *buffer, usize position)
{
    usize length;

    if (!buffer || position > reed_buffer_length(buffer)) return false;
    if (position == buffer->gap_start) return true;
    length = reed_buffer_length(buffer);
    if (position < buffer->gap_start) {
        usize amount = buffer->gap_start - position;
        memmove(buffer->storage + buffer->gap_end - amount,
                buffer->storage + position, amount);
        buffer->gap_start -= amount;
        buffer->gap_end -= amount;
    } else {
        usize amount = position - buffer->gap_start;
        memmove(buffer->storage + buffer->gap_start,
                buffer->storage + buffer->gap_end, amount);
        buffer->gap_start += amount;
        buffer->gap_end += amount;
    }
    return buffer->gap_start <= length && buffer->gap_end <= buffer->capacity;
}

static void snapshot_free(reed_snapshot_t *snapshot)
{
    if (!snapshot) return;
    free(snapshot->text);
    snapshot->text = NULL;
    snapshot->length = 0;
    snapshot->cursor = 0;
    snapshot->final_newline = false;
}

static void stack_clear(reed_snapshot_stack_t *stack)
{
    if (!stack) return;
    for (usize index = 0; index < stack->count; index++)
        snapshot_free(&stack->items[index]);
    free(stack->items);
    memset(stack, 0, sizeof(*stack));
}

void reed_buffer_clear_history(reed_buffer_t *buffer)
{
    if (!buffer) return;
    stack_clear(&buffer->undo);
    stack_clear(&buffer->redo);
}

static bool stack_reserve(reed_snapshot_stack_t *stack, usize count)
{
    reed_snapshot_t *items;
    usize capacity;

    if (count <= stack->capacity) return true;
    capacity = stack->capacity ? stack->capacity * 2U : 8U;
    while (capacity < count) capacity *= 2U;
    if (capacity > REED_UNDO_MAX_ENTRIES) capacity = REED_UNDO_MAX_ENTRIES;
    items = (reed_snapshot_t *)realloc(stack->items,
                                       capacity * sizeof(*items));
    if (!items) return false;
    stack->items = items;
    stack->capacity = capacity;
    return true;
}

static bool stack_push(reed_snapshot_stack_t *stack, reed_snapshot_t *snapshot)
{
    if (!stack || !snapshot || !snapshot->text ||
        snapshot->length > REED_UNDO_MAX_BYTES) return false;
    while (stack->count >= REED_UNDO_MAX_ENTRIES ||
           stack->bytes > REED_UNDO_MAX_BYTES - snapshot->length) {
        if (!stack->count) return false;
        stack->bytes -= stack->items[0].length;
        snapshot_free(&stack->items[0]);
        memmove(stack->items, stack->items + 1,
                (stack->count - 1U) * sizeof(*stack->items));
        stack->count--;
    }
    if (!stack_reserve(stack, stack->count + 1U)) return false;
    stack->items[stack->count++] = *snapshot;
    stack->bytes += snapshot->length;
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

static bool snapshot_make(const reed_buffer_t *buffer, reed_snapshot_t *out)
{
    if (!buffer || !out || !reed_buffer_copy_allocated(buffer, &out->text,
                                                        &out->length))
        return false;
    out->cursor = buffer->cursor;
    out->final_newline = buffer->final_newline;
    return true;
}

static void refresh_modified(reed_buffer_t *buffer);

static bool restore_snapshot(reed_buffer_t *buffer, const reed_snapshot_t *snapshot)
{
    if (!buffer || !snapshot || !reed_buffer_replace(buffer, snapshot->text,
                                                       snapshot->length))
        return false;
    buffer->cursor = snapshot->cursor > snapshot->length
        ? snapshot->length : snapshot->cursor;
    buffer->final_newline = snapshot->final_newline;
    refresh_modified(buffer);
    return true;
}

static void refresh_modified(reed_buffer_t *buffer)
{
    char *text = NULL;
    usize length = 0;

    if (!buffer || !reed_buffer_copy_allocated(buffer, &text, &length)) {
        if (buffer) buffer->modified = true;
        return;
    }
    buffer->modified = length != buffer->saved_length ||
        (length != 0 && memcmp(text, buffer->saved_text, length) != 0) ||
        buffer->final_newline != buffer->saved_final_newline;
    free(text);
}

bool reed_buffer_init(reed_buffer_t *buffer)
{
    if (!buffer) return false;
    memset(buffer, 0, sizeof(*buffer));
    buffer->capacity = REED_INITIAL_CAPACITY;
    buffer->storage = (char *)malloc(buffer->capacity);
    if (!buffer->storage) return false;
    buffer->gap_end = buffer->capacity;
    buffer->saved_text = (char *)malloc(1);
    if (!buffer->saved_text) {
        free(buffer->storage);
        memset(buffer, 0, sizeof(*buffer));
        return false;
    }
    buffer->saved_text[0] = '\0';
    buffer->final_newline = true;
    buffer->saved_final_newline = true;
    return true;
}

void reed_buffer_destroy(reed_buffer_t *buffer)
{
    if (!buffer) return;
    free(buffer->storage);
    free(buffer->pathname);
    free(buffer->saved_text);
    stack_clear(&buffer->undo);
    stack_clear(&buffer->redo);
    memset(buffer, 0, sizeof(*buffer));
}

bool reed_buffer_replace(reed_buffer_t *buffer, const char *text,
                         usize length)
{
    char *replacement;
    if (!buffer || (length && !text) || length > REED_MAX_BUFFER_BYTES ||
        !reed_buffer_valid_utf8(text, length)) return false;
    /* Replacing the whole document needs room for the new text, not the sum
     * of old and new lengths. Keep the old state until allocation succeeds. */
    if (length > buffer->capacity) {
        replacement = (char *)malloc(length);
        if (!replacement) return false;
        memcpy(replacement, text, length);
        free(buffer->storage);
        buffer->storage = replacement;
        buffer->capacity = length;
    } else if (length) {
        memmove(buffer->storage, text, length);
    }
    buffer->gap_start = 0;
    buffer->gap_end = buffer->capacity;
    buffer->gap_start = length;
    buffer->cursor = length;
    refresh_modified(buffer);
    return true;
}

bool reed_buffer_has_final_newline(const reed_buffer_t *buffer)
{
    return buffer && buffer->final_newline;
}

bool reed_buffer_set_final_newline(reed_buffer_t *buffer, bool enabled)
{
    if (!buffer) return false;
    buffer->final_newline = enabled;
    refresh_modified(buffer);
    return true;
}

bool reed_buffer_copy_file_allocated(const reed_buffer_t *buffer,
                                     char **out_text, usize *out_length)
{
    char *text;
    usize length;
    usize file_length;

    if (!buffer || !out_text) return false;
    length = reed_buffer_length(buffer);
    if (length > REED_MAX_BUFFER_BYTES -
                    (buffer->final_newline ? 1U : 0U)) return false;
    file_length = length + (buffer->final_newline ? 1U : 0U);
    text = (char *)malloc(file_length + 1U);
    if (!text) return false;
    if (length && !reed_buffer_copy_slice(buffer, 0, text, length)) {
        free(text);
        return false;
    }
    if (buffer->final_newline) text[length++] = '\n';
    text[length] = '\0';
    *out_text = text;
    if (out_length) *out_length = file_length;
    return true;
}

bool reed_buffer_set_path(reed_buffer_t *buffer, const char *pathname)
{
    usize length;
    char *copy;

    if (!buffer || !pathname) return false;
    length = strlen(pathname);
    if (!length || length >= REED_MAX_PATH_BYTES) return false;
    copy = (char *)malloc(length + 1U);
    if (!copy) return false;
    memcpy(copy, pathname, length + 1U);
    reed_buffer_commit_path(buffer, copy, length);
    return true;
}

void reed_buffer_commit_path(reed_buffer_t *buffer, char *pathname,
                             usize length)
{
    if (!buffer || !pathname) return;
    free(buffer->pathname);
    buffer->pathname = pathname;
    buffer->pathname_length = length;
}

const char *reed_buffer_path(const reed_buffer_t *buffer)
{
    return buffer && buffer->pathname ? buffer->pathname : NULL;
}

bool reed_buffer_mark_saved(reed_buffer_t *buffer)
{
    char *copy = NULL;
    usize length;

    if (!buffer || !reed_buffer_copy_allocated(buffer, &copy, &length))
        return false;
    reed_buffer_commit_saved(buffer, copy, length);
    return true;
}

void reed_buffer_commit_saved(reed_buffer_t *buffer, char *saved_text,
                              usize saved_length)
{
    if (!buffer || !saved_text) return;
    free(buffer->saved_text);
    buffer->saved_text = saved_text;
    buffer->saved_length = saved_length;
    buffer->saved_final_newline = buffer->final_newline;
    buffer->modified = false;
}

bool reed_buffer_is_modified(const reed_buffer_t *buffer)
{
    return buffer && buffer->modified;
}

bool reed_buffer_record_edit(reed_buffer_t *buffer)
{
    reed_snapshot_t snapshot = {0};

    if (!buffer || !snapshot_make(buffer, &snapshot)) return false;
    if (!stack_push(&buffer->undo, &snapshot)) {
        snapshot_free(&snapshot);
        return false;
    }
    stack_clear(&buffer->redo);
    return true;
}

bool reed_buffer_insert(reed_buffer_t *buffer, usize position,
                        const char *text, usize length)
{
    if (!buffer || (length && !text) ||
        !buffer_codepoint_boundary(buffer, position) ||
        !reed_buffer_valid_utf8(text, length) || !ensure_gap(buffer, length) ||
        !move_gap(buffer, position)) return false;
    if (length) memcpy(buffer->storage + buffer->gap_start, text, length);
    buffer->gap_start += length;
    buffer->cursor = position + length;
    refresh_modified(buffer);
    return true;
}

bool reed_buffer_delete(reed_buffer_t *buffer, usize position, usize length)
{
    usize total;

    if (!buffer || !buffer_codepoint_boundary(buffer, position)) return false;
    total = reed_buffer_length(buffer);
    if (length > total - position ||
        !buffer_codepoint_boundary(buffer, position + length) ||
        !move_gap(buffer, position)) return false;
    buffer->gap_end += length;
    buffer->cursor = position;
    refresh_modified(buffer);
    return true;
}

bool reed_buffer_undo(reed_buffer_t *buffer)
{
    reed_snapshot_t current = {0};
    reed_snapshot_t target;

    if (!buffer || !buffer->undo.count || !snapshot_make(buffer, &current))
        return false;
    if (!stack_push(&buffer->redo, &current)) {
        snapshot_free(&current);
        return false;
    }
    target = buffer->undo.items[buffer->undo.count - 1U];
    buffer->undo.count--;
    buffer->undo.bytes -= target.length;
    if (!restore_snapshot(buffer, &target)) {
        buffer->undo.items[buffer->undo.count++] = target;
        buffer->undo.bytes += target.length;
        return false;
    }
    snapshot_free(&target);
    return true;
}

bool reed_buffer_redo(reed_buffer_t *buffer)
{
    reed_snapshot_t current = {0};
    reed_snapshot_t target;

    if (!buffer || !buffer->redo.count || !snapshot_make(buffer, &current))
        return false;
    if (!stack_push(&buffer->undo, &current)) {
        snapshot_free(&current);
        return false;
    }
    target = buffer->redo.items[buffer->redo.count - 1U];
    buffer->redo.count--;
    buffer->redo.bytes -= target.length;
    if (!restore_snapshot(buffer, &target)) {
        buffer->redo.items[buffer->redo.count++] = target;
        buffer->redo.bytes += target.length;
        return false;
    }
    snapshot_free(&target);
    return true;
}

bool reed_buffer_cursor_set(reed_buffer_t *buffer, usize position)
{
    if (!buffer_codepoint_boundary(buffer, position)) return false;
    buffer->cursor = position;
    return true;
}

usize reed_buffer_cursor(const reed_buffer_t *buffer)
{
    return buffer ? buffer->cursor : 0;
}

usize reed_buffer_next_boundary(const reed_buffer_t *buffer, usize position)
{
    usize length = reed_buffer_length(buffer);
    usize decoded_length;

    if (!buffer || position >= length) return length;
    /* Decode through a small bounded temporary suffix. */
    {
        char bytes[4];
        usize available = length - position;
        usize copy = available > sizeof(bytes) ? sizeof(bytes) : available;
        for (usize index = 0; index < copy; index++)
            bytes[index] = buffer_byte(buffer, position + index);
        if (utf8_decode_at(bytes, copy, 0, NULL, &decoded_length))
            return position + decoded_length;
    }
    return position + 1U;
}

usize reed_buffer_previous_boundary(const reed_buffer_t *buffer,
                                    usize position)
{
    usize length = reed_buffer_length(buffer);
    usize first;

    if (!buffer || position == 0 || position > length) return 0;
    first = position > 4U ? position - 4U : 0U;
    for (usize start = first; start < position; start++) {
        u8 bytes[4];
        usize count = position - start;
        usize consumed;
        u32 scalar;
        if (count > sizeof(bytes)) continue;
        for (usize index = 0; index < count; index++)
            bytes[index] = (u8)buffer_byte(buffer, start + index);
        if (mg_utf8_decode_one(bytes, count, &scalar, &consumed) == MG_UTF8_OK &&
            consumed == count) return start;
    }
    return position - 1U;
}

bool reed_buffer_codepoint(const reed_buffer_t *buffer, usize position,
                           u32 *out_codepoint, usize *out_next)
{
    char bytes[4];
    usize length;
    usize available;
    usize copy;

    if (!buffer) return false;
    length = reed_buffer_length(buffer);
    if (position >= length) return false;
    available = length - position;
    copy = available > sizeof(bytes) ? sizeof(bytes) : available;
    for (usize index = 0; index < copy; index++)
        bytes[index] = buffer_byte(buffer, position + index);
    if (!utf8_decode_at(bytes, copy, 0, out_codepoint, &copy)) return false;
    if (out_next) *out_next = position + copy;
    return true;
}

usize reed_buffer_line_count(const reed_buffer_t *buffer)
{
    usize count = 1;
    usize length = reed_buffer_length(buffer);

    if (!buffer) return 0;
    for (usize index = 0; index < length; index++)
        if (buffer_byte(buffer, index) == '\n') count++;
    return count;
}

bool reed_buffer_line_bounds(const reed_buffer_t *buffer, usize line,
                             usize *out_start, usize *out_end)
{
    usize current = 0;
    usize start = 0;
    usize length;

    if (!buffer || !out_start || !out_end) return false;
    length = reed_buffer_length(buffer);
    for (usize index = 0; index <= length; index++) {
        if (index == length || buffer_byte(buffer, index) == '\n') {
            if (current == line) {
                *out_start = start;
                *out_end = index;
                return true;
            }
            current++;
            start = index + 1U;
        }
    }
    return false;
}

usize reed_buffer_line_column(const reed_buffer_t *buffer, usize position)
{
    usize line;
    usize start;
    usize end;
    usize column = 0;

    if (!buffer) return 0;
    line = reed_buffer_line_at(buffer, position);
    if (!reed_buffer_line_bounds(buffer, line, &start, &end)) return 0;
    if (position > end) position = end;
    while (start < position) {
        usize next = reed_buffer_next_boundary(buffer, start);
        if (next > position || next > end) break;
        start = next;
        column++;
    }
    return column;
}

usize reed_buffer_line_at(const reed_buffer_t *buffer, usize position)
{
    usize line = 0;
    usize length;

    if (!buffer) return 0;
    length = reed_buffer_length(buffer);
    if (position > length) position = length;
    for (usize index = 0; index < position; index++)
        if (buffer_byte(buffer, index) == '\n') line++;
    return line;
}

bool reed_buffer_position_for_line_column(const reed_buffer_t *buffer,
                                          usize line, usize column,
                                          usize *out_position)
{
    usize start;
    usize end;
    usize position;

    if (!buffer || !out_position ||
        !reed_buffer_line_bounds(buffer, line, &start, &end)) return false;
    position = start;
    while (column && position < end) {
        usize next = reed_buffer_next_boundary(buffer, position);
        if (next > end) break;
        position = next;
        column--;
    }
    *out_position = position;
    return true;
}
