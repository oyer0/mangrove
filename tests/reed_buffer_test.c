/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userspace/reed/buffer.h"

static int fail(const char *name)
{
    printf("Reed buffer test failed: %s\n", name);
    return 1;
}

static int expect_text(const reed_buffer_t *buffer, const char *expected)
{
    char actual[128];
    usize length;

    if (!reed_buffer_copy(buffer, actual, sizeof(actual), &length) ||
        strcmp(actual, expected) != 0 || length != strlen(expected)) return 1;
    return 0;
}

static int expect_file_text(const reed_buffer_t *buffer, const char *expected)
{
    char *actual = NULL;
    usize length = 0;
    int failed;

    if (!reed_buffer_copy_file_allocated(buffer, &actual, &length)) return 1;
    failed = length != strlen(expected) || strcmp(actual, expected) != 0;
    free(actual);
    return failed;
}

int main(void)
{
    reed_buffer_t buffer;
    reed_buffer_t eol;
    usize position;
    const char truncated_utf8[] = {(char)0xE2};

    if (!reed_buffer_init(&buffer)) return fail("init");
    if (reed_buffer_valid_utf8(truncated_utf8, sizeof(truncated_utf8)))
        return fail("truncated UTF-8");
    if (!reed_buffer_record_edit(&buffer) ||
        !reed_buffer_insert(&buffer, 0, "one\ntwo", 7) ||
        expect_text(&buffer, "one\ntwo")) return fail("insert");
    if (!reed_buffer_position_for_line_column(&buffer, 1, 2, &position) ||
        position != 6) return fail("line position");
    if (reed_buffer_line_count(&buffer) != 2 ||
        reed_buffer_line_at(&buffer, position) != 1 ||
        reed_buffer_line_column(&buffer, position) != 2)
        return fail("line mapping");
    if (!reed_buffer_undo(&buffer) || expect_text(&buffer, ""))
        return fail("undo");
    if (!reed_buffer_redo(&buffer) || expect_text(&buffer, "one\ntwo"))
        return fail("redo");
    if (!reed_buffer_record_edit(&buffer) ||
        !reed_buffer_delete(&buffer, 3, 1) ||
        expect_text(&buffer, "onetwo")) return fail("delete");
    if (!reed_buffer_undo(&buffer) || !reed_buffer_record_edit(&buffer) ||
        !reed_buffer_insert(&buffer, 0, "x", 1) ||
        reed_buffer_redo(&buffer)) return fail("redo branch");
    reed_buffer_destroy(&buffer);

    if (!reed_buffer_init(&eol)) return fail("final newline init");
    if (!reed_buffer_has_final_newline(&eol) ||
        reed_buffer_line_count(&eol) != 1) return fail("final newline default");
    if (!reed_buffer_insert(&eol, 0, "hello", 5) ||
        expect_file_text(&eol, "hello\n") ||
        reed_buffer_line_count(&eol) != 1) return fail("final newline one line");
    if (!reed_buffer_mark_saved(&eol) || reed_buffer_is_modified(&eol))
        return fail("final newline saved");
    if (!reed_buffer_record_edit(&eol) ||
        !reed_buffer_insert(&eol, 5, "\n", 1) ||
        expect_file_text(&eol, "hello\n\n") ||
        reed_buffer_line_count(&eol) != 2) return fail("final newline second line");
    if (!reed_buffer_undo(&eol) || expect_file_text(&eol, "hello\n") ||
        reed_buffer_line_count(&eol) != 1 || reed_buffer_is_modified(&eol))
        return fail("final newline undo");
    if (!reed_buffer_redo(&eol) || expect_file_text(&eol, "hello\n\n") ||
        reed_buffer_line_count(&eol) != 2) return fail("final newline redo");
    reed_buffer_destroy(&eol);

    if (!reed_buffer_init(&eol) || !reed_buffer_replace(&eol, "hello", 5) ||
        !reed_buffer_set_final_newline(&eol, false) ||
        expect_file_text(&eol, "hello") || reed_buffer_has_final_newline(&eol) ||
        !reed_buffer_mark_saved(&eol) || reed_buffer_is_modified(&eol) ||
        !reed_buffer_set_final_newline(&eol, true) ||
        !reed_buffer_is_modified(&eol) || expect_file_text(&eol, "hello\n"))
        return fail("no final newline");
    reed_buffer_destroy(&eol);
    return 0;
}
