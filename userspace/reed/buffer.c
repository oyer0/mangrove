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
    usize first = position < buffer->gap_start ? buffer->gap_start - position : 0;
    if (first > length) first = length;
    if (first) memcpy(output, buffer->storage + position, first);
    if (length > first)
        memcpy(output + first, buffer->storage + position + first +
               gap_length(buffer), length - first);
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

static usize count_newlines(const char *text, usize length)
{
    usize count = 0;
    for (usize i = 0; i < length; i++) if (text[i] == '\n') count++;
    return count;
}

static bool reserve_lines(reed_buffer_t *buffer, usize count)
{
    if (count <= buffer->newline_capacity) return true;
    if (count > (usize)-1 / sizeof(usize)) return false;
    usize capacity = buffer->newline_capacity ? buffer->newline_capacity : 32U;
    while (capacity < count) {
        if (capacity > (usize)-1 / sizeof(usize) / 2U) { capacity = count; break; }
        capacity *= 2U;
    }
    usize *lines = realloc(buffer->newlines, capacity * sizeof(*lines));
    if (!lines) return false;
    buffer->newlines = lines;
    buffer->newline_capacity = capacity;
    return true;
}

/* Index updates are allocation-free after reserve_lines. Offsets refer to
 * logical bytes, never the movable gap. */
static void splice_lines(reed_buffer_t *buffer, usize position, usize removed,
                         const char *text, usize inserted)
{
    usize first = reed_buffer_line_at(buffer, position);
    usize last = reed_buffer_line_at(buffer, position + removed);
    usize added = count_newlines(text, inserted);
    usize tail = buffer->newline_count - last;
    if (tail) memmove(buffer->newlines + first + added, buffer->newlines + last,
                      tail * sizeof(usize));
    for (usize i = first + added; i < first + added + tail; i++)
        buffer->newlines[i] = buffer->newlines[i] - removed + inserted;
    for (usize i = 0, at = first; i < inserted; i++)
        if (text[i] == '\n') buffer->newlines[at++] = position + i;
    buffer->newline_count = first + added + tail;
    buffer->change_position = position;
    buffer->change_removed = removed;
    buffer->change_inserted = inserted;
    buffer->change_within_line = first == last && !added;
    buffer->revision++;
}

static void snapshot_free(reed_snapshot_t *snapshot)
{
    if (!snapshot) return;
    reed_change_t *change = snapshot->first;
    while (change) {
        reed_change_t *next = change->next;
        free(change);
        change = next;
    }
    memset(snapshot, 0, sizeof(*snapshot));
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
    buffer->recording = false;
    buffer->pending_group = false;
}

static bool stack_reserve(reed_snapshot_stack_t *stack, usize count)
{
    if (count <= stack->capacity) return true;
    usize capacity = stack->capacity ? stack->capacity : 8U;
    while (capacity < count) {
        if (capacity > (usize)-1 / 2U) {
            capacity = count;
            break;
        }
        capacity *= 2U;
    }
    if (capacity < count) capacity = count;
    if (capacity > (usize)-1 / sizeof(*stack->items)) return false;
    reed_snapshot_t *items = realloc(stack->items, capacity * sizeof(*items));
    if (!items) return false;
    stack->items = items;
    stack->capacity = capacity;
    return true;
}

static bool stack_append(reed_snapshot_stack_t *stack,
                         reed_snapshot_t snapshot)
{
    if (!stack || stack->count >= stack->capacity ||
        snapshot.length > (usize)-1 - stack->bytes) return false;
    stack->items[stack->count++] = snapshot;
    stack->bytes += snapshot.length;
    return true;
}

static reed_change_t *change_make(const reed_buffer_t *buffer, usize position,
                                  usize removed, const char *text, usize inserted)
{
    if (removed > (usize)-1 - sizeof(reed_change_t) ||
        inserted > (usize)-1 - sizeof(reed_change_t) - removed) return NULL;
    reed_change_t *change = malloc(sizeof(*change) + removed + inserted);
    if (!change) return NULL;
    *change = (reed_change_t){.position = position, .removed = removed,
                              .inserted = inserted};
    if (removed) reed_buffer_copy_slice(buffer, position, change->data, removed);
    if (inserted) memcpy(change->data + removed, text, inserted);
    change->removed_lines = count_newlines(change->data, removed);
    change->inserted_lines = count_newlines(text, inserted);
    return change;
}

static bool change_append(reed_snapshot_t *snapshot, reed_change_t *change)
{
    usize bytes;
    if (!snapshot || !change || change->removed > (usize)-1 - sizeof(*change) ||
        change->inserted > (usize)-1 - sizeof(*change) - change->removed)
        return false;
    bytes = sizeof(*change) + change->removed + change->inserted;
    if (bytes > (usize)-1 - snapshot->length) return false;
    change->previous = snapshot->last;
    if (snapshot->last) snapshot->last->next = change;
    else snapshot->first = change;
    snapshot->last = change;
    snapshot->length += bytes;
    return true;
}

static void refresh_modified(reed_buffer_t *buffer)
{
    if (!buffer) return;
    usize length = reed_buffer_length(buffer);
    buffer->modified = length != buffer->saved_length ||
        buffer->final_newline != buffer->saved_final_newline;
    if (!buffer->modified && length) {
        usize suffix = length - buffer->gap_start;
        buffer->modified = memcmp(buffer->storage, buffer->saved_text,
                                   buffer->gap_start) != 0 ||
            (suffix && memcmp(buffer->storage + buffer->gap_end,
                             buffer->saved_text + buffer->gap_start, suffix) != 0);
    }
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
    free(buffer->newlines);
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
    usize lines = count_newlines(text, length);
    if (!reserve_lines(buffer, lines)) return false;
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
    buffer->newline_count = 0;
    for (usize i = 0; i < length; i++)
        if (text[i] == '\n') buffer->newlines[buffer->newline_count++] = i;
    buffer->revision++;
    buffer->change_within_line = false;
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
    if (buffer->pending_group && buffer->final_newline != enabled) {
        reed_snapshot_t snapshot = {.cursor = buffer->pending_cursor,
            .final_newline = buffer->pending_final_newline};
        if (!stack_append(&buffer->undo, snapshot)) return false;
        stack_clear(&buffer->redo);
        buffer->pending_group = false;
    }
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
    if (!buffer) return false;
    if (buffer->undo.count == (usize)-1) return false;
    usize count = buffer->undo.count + 1U;
    if (!stack_reserve(&buffer->undo, count)) return false;
    buffer->pending_cursor = buffer->cursor;
    buffer->pending_final_newline = buffer->final_newline;
    buffer->pending_group = true;
    buffer->recording = true;
    return true;
}

bool reed_buffer_prepare_edit(reed_buffer_t *buffer, usize position,
                               usize remove_length, const char *text,
                               usize length, reed_buffer_edit_t *edit)
{
    if (!buffer || !edit || !buffer_codepoint_boundary(buffer, position))
        return false;
    usize total = reed_buffer_length(buffer);
    if (remove_length > total - position ||
        !buffer_codepoint_boundary(buffer, position + remove_length) ||
        !reed_buffer_valid_utf8(text, length) ||
        length > REED_MAX_BUFFER_BYTES - (total - remove_length)) return false;
    usize reserve = length > remove_length ? length - remove_length : 0;
    if (buffer->undo.count == (usize)-1) return false;
    usize entries = buffer->undo.count + 1U;
    if (!ensure_gap(buffer, reserve) ||
        !reserve_lines(buffer, buffer->newline_count + count_newlines(text, length)) ||
        !stack_reserve(&buffer->undo, entries)) return false;
    reed_change_t *change = change_make(buffer, position, remove_length, text, length);
    if (!change) return false;
    reed_snapshot_t snapshot = {.cursor = buffer->cursor,
                                .final_newline = buffer->final_newline};
    if (!change_append(&snapshot, change) ||
        snapshot.length > (usize)-1 - buffer->undo.bytes) {
        free(change);
        return false;
    }
    *edit = (reed_buffer_edit_t){buffer, snapshot, position, remove_length,
                                 length, text};
    return true;
}

void reed_buffer_discard_edit(reed_buffer_edit_t *edit)
{
    if (!edit || !edit->buffer) return;
    snapshot_free(&edit->snapshot);
    memset(edit, 0, sizeof(*edit));
}

static void splice(reed_buffer_t *buffer, usize position, usize removed,
                   const char *text, usize inserted)
{
    splice_lines(buffer, position, removed, text, inserted);
    (void)move_gap(buffer, position);
    buffer->gap_end += removed;
    if (inserted) memcpy(buffer->storage + buffer->gap_start, text, inserted);
    buffer->gap_start += inserted;
    buffer->cursor = position + inserted;
}

void reed_buffer_commit_edit(reed_buffer_edit_t *edit)
{
    if (!edit || !edit->buffer) return;
    reed_buffer_t *buffer = edit->buffer;
    stack_append(&buffer->undo, edit->snapshot);
    stack_clear(&buffer->redo);
    buffer->recording = true;
    buffer->pending_group = false;
    splice(buffer, edit->position, edit->remove_length, edit->text, edit->length);
    refresh_modified(buffer);
    memset(edit, 0, sizeof(*edit));
}

static bool edit_range(reed_buffer_t *buffer, usize position, usize removed,
                       const char *text, usize inserted)
{
    usize total = reed_buffer_length(buffer);
    if (!buffer || !buffer_codepoint_boundary(buffer, position) ||
        removed > total - position ||
        !buffer_codepoint_boundary(buffer, position + removed) ||
        !reed_buffer_valid_utf8(text, inserted) ||
        inserted > REED_MAX_BUFFER_BYTES - (total - removed) ||
        !ensure_gap(buffer, inserted > removed ? inserted - removed : 0) ||
        !reserve_lines(buffer, buffer->newline_count + count_newlines(text, inserted)))
        return false;
    if (buffer->recording) {
        reed_change_t *change = change_make(buffer, position, removed, text, inserted);
        if (!change) return false;
        usize delta_bytes = sizeof(*change) + removed + inserted;
        reed_snapshot_t *group = NULL;
        if (buffer->pending_group) {
            if (buffer->undo.count >= buffer->undo.capacity ||
                delta_bytes > (usize)-1 - buffer->undo.bytes) {
                free(change);
                return false;
            }
        } else if (buffer->undo.count) {
            group = &buffer->undo.items[buffer->undo.count - 1U];
            if (delta_bytes > (usize)-1 - group->length ||
                delta_bytes > (usize)-1 - buffer->undo.bytes) {
                free(change);
                return false;
            }
        } else {
            free(change);
            return false;
        }
        if (buffer->pending_group) {
            reed_snapshot_t snapshot = {.cursor = buffer->pending_cursor,
                .final_newline = buffer->pending_final_newline};
            if (!stack_append(&buffer->undo, snapshot)) {
                free(change);
                return false;
            }
            group = &buffer->undo.items[buffer->undo.count - 1U];
            stack_clear(&buffer->redo);
            buffer->pending_group = false;
        }
        if (!change_append(group, change)) {
            /* Size was preflighted above, so this is only reachable if the
             * history state was concurrently changed, which is unsupported. */
            free(change);
            return false;
        }
        buffer->undo.bytes += delta_bytes;
    }
    splice(buffer, position, removed, text, inserted);
    refresh_modified(buffer);
    return true;
}

bool reed_buffer_insert(reed_buffer_t *buffer, usize position,
                        const char *text, usize length)
{
    return edit_range(buffer, position, 0, text, length);
}

bool reed_buffer_delete(reed_buffer_t *buffer, usize position, usize length)
{
    return edit_range(buffer, position, length, NULL, 0);
}

static bool history_move(reed_buffer_t *buffer, reed_snapshot_stack_t *from,
                         reed_snapshot_stack_t *to, bool reverse)
{
    if (!from->count) return false;
    reed_snapshot_t *target = &from->items[from->count - 1];
    usize length = reed_buffer_length(buffer), maximum = length;
    usize lines = buffer->newline_count, maximum_lines = lines;
    for (reed_change_t *c = reverse ? target->last : target->first; c;
         c = reverse ? c->previous : c->next) {
        usize removed = reverse ? c->inserted : c->removed;
        usize inserted = reverse ? c->removed : c->inserted;
        if (removed > length || inserted > REED_MAX_BUFFER_BYTES - (length - removed))
            return false;
        length = length - removed + inserted;
        lines = lines - (reverse ? c->inserted_lines : c->removed_lines) +
                         (reverse ? c->removed_lines : c->inserted_lines);
        if (length > maximum) maximum = length;
        if (lines > maximum_lines) maximum_lines = lines;
    }
    if (to->count == (usize)-1 ||
        target->length > (usize)-1 - to->bytes) return false;
    usize count = to->count + 1U;
    if (!stack_reserve(to, count) || !reserve_lines(buffer, maximum_lines) ||
        !ensure_gap(buffer, maximum - reed_buffer_length(buffer))) return false;
    usize cursor = buffer->cursor;
    bool final_newline = buffer->final_newline;
    for (reed_change_t *c = reverse ? target->last : target->first; c;
         c = reverse ? c->previous : c->next)
        splice(buffer, c->position, reverse ? c->inserted : c->removed,
               c->data + (reverse ? 0 : c->removed), reverse ? c->removed : c->inserted);
    buffer->cursor = target->cursor;
    buffer->final_newline = target->final_newline;
    target->cursor = cursor;
    target->final_newline = final_newline;
    reed_snapshot_t moved = *target;
    from->bytes -= moved.length;
    from->count--;
    stack_append(to, moved);
    buffer->recording = false;
    buffer->pending_group = false;
    refresh_modified(buffer);
    return true;
}

bool reed_buffer_undo(reed_buffer_t *buffer)
{
    return buffer && history_move(buffer, &buffer->undo, &buffer->redo, true);
}

bool reed_buffer_redo(reed_buffer_t *buffer)
{
    return buffer && history_move(buffer, &buffer->redo, &buffer->undo, false);
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
    return buffer ? buffer->newline_count + 1U : 0;
}

bool reed_buffer_line_bounds(const reed_buffer_t *buffer, usize line,
                             usize *out_start, usize *out_end)
{
    if (!buffer || !out_start || !out_end || line > buffer->newline_count) return false;
    *out_start = line ? buffer->newlines[line - 1] + 1U : 0;
    *out_end = line < buffer->newline_count ? buffer->newlines[line] : reed_buffer_length(buffer);
    return true;
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
    if (!buffer) return 0;
    usize low = 0, high = buffer->newline_count;
    while (low < high) {
        usize middle = low + (high - low) / 2;
        if (buffer->newlines[middle] < position) low = middle + 1;
        else high = middle;
    }
    return low;
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
