/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../userspace/reed/buffer.h"

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
    reed_buffer_t unicode;
    reed_buffer_t long_history;
    usize position;
    const char truncated_utf8[] = {(char)0xE2};
    static const char multilingual[] =
        "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x99\x82";

    if (!reed_buffer_init(&buffer)) return fail("init");
    if (reed_buffer_valid_utf8(truncated_utf8, sizeof(truncated_utf8)))
        return fail("truncated UTF-8");
    if (!reed_buffer_init(&unicode) ||
        !reed_buffer_insert(&unicode, 0, multilingual,
                            sizeof(multilingual) - 1U) ||
        !expect_file_text(&unicode, multilingual))
        return fail("multilingual UTF-8 insert");
    if (reed_buffer_length(&unicode) != 10U ||
        reed_buffer_next_boundary(&unicode, 0U) != 1U ||
        reed_buffer_next_boundary(&unicode, 1U) != 3U ||
        reed_buffer_next_boundary(&unicode, 3U) != 6U ||
        reed_buffer_next_boundary(&unicode, 6U) != 10U ||
        reed_buffer_previous_boundary(&unicode, 10U) != 6U ||
        reed_buffer_previous_boundary(&unicode, 6U) != 3U ||
        reed_buffer_previous_boundary(&unicode, 3U) != 1U)
        return fail("multilingual byte boundaries");
    if (reed_buffer_cursor_set(&unicode, 2U) ||
        reed_buffer_insert(&unicode, 2U, "x", 1U) ||
        reed_buffer_delete(&unicode, 1U, 1U) ||
        reed_buffer_delete(&unicode, 3U, 1U))
        return fail("reject mid-codepoint edits");
    if (!reed_buffer_cursor_set(&unicode, 6U) ||
        !reed_buffer_delete(&unicode, 1U, 2U) ||
        !expect_file_text(&unicode, "A\xE4\xB8\xAD\xF0\x9F\x99\x82") ||
        reed_buffer_cursor_set(&unicode, 2U))
        return fail("delete complete codepoint");
    reed_buffer_destroy(&unicode);
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

    if (!reed_buffer_init(&long_history)) return fail("long history init");
    for (usize index = 0; index < 160U; index++) {
        if (!reed_buffer_record_edit(&long_history) ||
            !reed_buffer_insert(&long_history,
                                reed_buffer_length(&long_history), "x", 1U))
            return fail("history grows beyond 64 groups");
    }
    if (long_history.undo.count != 160U) return fail("history group count");
    for (usize index = 0; index < 160U; index++)
        if (!reed_buffer_undo(&long_history)) return fail("long history undo");
    if (reed_buffer_length(&long_history) != 0U || long_history.redo.count != 160U)
        return fail("long history undo result");
    for (usize index = 0; index < 160U; index++)
        if (!reed_buffer_redo(&long_history)) return fail("long history redo");
    if (reed_buffer_length(&long_history) != 160U)
        return fail("long history redo result");
    reed_buffer_destroy(&long_history);

    if (!reed_buffer_init(&long_history)) return fail("large history init");
    {
        const usize large_edit_length = 9U * 1024U * 1024U;
        char *large_edit = (char *)malloc(large_edit_length);
        if (!large_edit) return fail("large history allocation");
        memset(large_edit, 'z', large_edit_length);
        if (!reed_buffer_record_edit(&long_history) ||
            !reed_buffer_insert(&long_history, 0, large_edit, large_edit_length) ||
            !reed_buffer_record_edit(&long_history) ||
            !reed_buffer_insert(&long_history, large_edit_length, "!", 1U) ||
            long_history.undo.count != 2U ||
            long_history.undo.bytes <= 8U * 1024U * 1024U ||
            !reed_buffer_undo(&long_history) ||
            !reed_buffer_undo(&long_history) ||
            reed_buffer_length(&long_history) != 0U ||
            !reed_buffer_redo(&long_history) ||
            !reed_buffer_redo(&long_history) ||
            reed_buffer_length(&long_history) != large_edit_length + 1U) {
            free(large_edit);
            return fail("history exceeds former byte target");
        }
        free(large_edit);
    }
    reed_buffer_destroy(&long_history);

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
