/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

#define REED_MAX_BUFFER_BYTES       (2U * 1024U * 1024U)
#define REED_MAX_PATH_BYTES         256U
#define REED_UNDO_MAX_ENTRIES       64U
#define REED_UNDO_MAX_BYTES         (8U * 1024U * 1024U)

typedef struct reed_snapshot {
    char *text;
    usize length;
    usize cursor;
    bool final_newline;
} reed_snapshot_t;

typedef struct reed_snapshot_stack {
    reed_snapshot_t *items;
    usize count;
    usize capacity;
    usize bytes;
} reed_snapshot_stack_t;

/* The gap is the mutable part of the buffer.  All positions are byte
 * offsets, but the editing helpers only return UTF-8 code-point boundaries.
 * final_newline is a file-format terminator, not a visible empty line in the
 * editable text. */
typedef struct reed_buffer {
    char *storage;
    usize capacity;
    usize gap_start;
    usize gap_end;
    char *pathname;
    usize pathname_length;
    char *saved_text;
    usize saved_length;
    bool final_newline;
    bool saved_final_newline;
    reed_snapshot_stack_t undo;
    reed_snapshot_stack_t redo;
    usize cursor;
    bool modified;
} reed_buffer_t;

bool reed_buffer_init(reed_buffer_t *buffer);
void reed_buffer_destroy(reed_buffer_t *buffer);

usize reed_buffer_length(const reed_buffer_t *buffer);
bool reed_buffer_copy(const reed_buffer_t *buffer, char *output,
                      usize capacity, usize *out_length);
bool reed_buffer_copy_slice(const reed_buffer_t *buffer, usize position,
                            char *output, usize length);
bool reed_buffer_copy_allocated(const reed_buffer_t *buffer, char **out_text,
                                usize *out_length);
/* Copies the exact bytes that saving this buffer would write. */
bool reed_buffer_copy_file_allocated(const reed_buffer_t *buffer,
                                     char **out_text, usize *out_length);
bool reed_buffer_replace(reed_buffer_t *buffer, const char *text,
                         usize length);
bool reed_buffer_has_final_newline(const reed_buffer_t *buffer);
bool reed_buffer_set_final_newline(reed_buffer_t *buffer, bool enabled);
bool reed_buffer_set_path(reed_buffer_t *buffer, const char *pathname);
/* Takes ownership of a path copy prepared before persistent creation. */
void reed_buffer_commit_path(reed_buffer_t *buffer, char *pathname,
                             usize length);
const char *reed_buffer_path(const reed_buffer_t *buffer);
bool reed_buffer_mark_saved(reed_buffer_t *buffer);
/* Takes ownership of a copy prepared before a filesystem save. No allocation. */
void reed_buffer_commit_saved(reed_buffer_t *buffer, char *saved_text,
                              usize saved_length);
bool reed_buffer_is_modified(const reed_buffer_t *buffer);

bool reed_buffer_record_edit(reed_buffer_t *buffer);
void reed_buffer_clear_history(reed_buffer_t *buffer);
/* Ensure an insertion at a code-point boundary will not need to allocate. */
bool reed_buffer_reserve_insert(reed_buffer_t *buffer, usize position,
                                usize length);
bool reed_buffer_insert(reed_buffer_t *buffer, usize position,
                        const char *text, usize length);
bool reed_buffer_delete(reed_buffer_t *buffer, usize position, usize length);
bool reed_buffer_undo(reed_buffer_t *buffer);
bool reed_buffer_redo(reed_buffer_t *buffer);

bool reed_buffer_cursor_set(reed_buffer_t *buffer, usize position);
usize reed_buffer_cursor(const reed_buffer_t *buffer);
usize reed_buffer_next_boundary(const reed_buffer_t *buffer, usize position);
usize reed_buffer_previous_boundary(const reed_buffer_t *buffer,
                                    usize position);
bool reed_buffer_codepoint(const reed_buffer_t *buffer, usize position,
                           u32 *out_codepoint, usize *out_next);

usize reed_buffer_line_count(const reed_buffer_t *buffer);
bool reed_buffer_line_bounds(const reed_buffer_t *buffer, usize line,
                             usize *out_start, usize *out_end);
usize reed_buffer_line_column(const reed_buffer_t *buffer, usize position);
usize reed_buffer_line_at(const reed_buffer_t *buffer, usize position);
bool reed_buffer_position_for_line_column(const reed_buffer_t *buffer,
                                          usize line, usize column,
                                          usize *out_position);

bool reed_buffer_valid_utf8(const char *text, usize length);
