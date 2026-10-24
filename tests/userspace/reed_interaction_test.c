/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Exercise Reed's production keymap, motion, selection, and search helpers. */
#include "../../userspace/reed/editor.c"

static long fail_malloc_after = -1;

#if !defined(__APPLE__)
void *__real_malloc(size_t size);
#endif

void *__wrap_malloc(size_t size)
{
    if (fail_malloc_after == 0) {
        fail_malloc_after = -1;
        return NULL;
    }
    if (fail_malloc_after > 0) fail_malloc_after--;
    return __real_malloc(size);
}

static mg_terminal_key_event_t key(u32 keycode, u32 modifiers)
{
    mg_terminal_key_event_t event = {0};
    event.keycode = keycode;
    event.modifiers = modifiers;
    return event;
}

static void editor_with_text(reed_editor_t *editor, const char *text)
{
    usize length = strlen(text);

    memset(editor, 0, sizeof(*editor));
    assert(reed_buffer_init(&editor->buffer));
    assert(reed_buffer_replace(&editor->buffer, text, length));
    editor->screen_rows = 24U;
    editor->screen_columns = 80U;
}

static void expect_text(const reed_buffer_t *buffer, const char *expected)
{
    char actual[256];

    assert(reed_buffer_copy(buffer, actual, sizeof(actual), NULL));
    assert(strcmp(actual, expected) == 0);
}

static void test_complete_keymap(void)
{
    mg_terminal_key_event_t event;

#define EXPECT(keycode_, modifiers_, action_) do { \
    event = key((keycode_), (modifiers_)); \
    assert(normal_action(&event) == (action_)); \
} while (0)
#define EXPECT_NONE(keycode_, modifiers_) \
    EXPECT((keycode_), (modifiers_), REED_ACTION_NONE)

    EXPECT(HID_KEY_H, 0, REED_ACTION_MOVE_LEFT);
    EXPECT(HID_KEY_J, 0, REED_ACTION_MOVE_DOWN);
    EXPECT(HID_KEY_K, 0, REED_ACTION_MOVE_UP);
    EXPECT(HID_KEY_L, 0, REED_ACTION_MOVE_RIGHT);
    EXPECT(HID_KEY_LEFT, 0, REED_ACTION_MOVE_LEFT);
    EXPECT(HID_KEY_DOWN, 0, REED_ACTION_MOVE_DOWN);
    EXPECT(HID_KEY_UP, 0, REED_ACTION_MOVE_UP);
    EXPECT(HID_KEY_RIGHT, 0, REED_ACTION_MOVE_RIGHT);
    EXPECT(HID_KEY_B, 0, REED_ACTION_WORD_PREVIOUS_END);
    EXPECT(HID_KEY_B, MG_TERMINAL_MODIFIER_RIGHT_SHIFT,
           REED_ACTION_WORD_CURRENT_PREVIOUS_START);
    EXPECT(HID_KEY_N, 0, REED_ACTION_WORD_CURRENT_NEXT_END);
    EXPECT(HID_KEY_N, MG_TERMINAL_MODIFIER_LEFT_SHIFT,
           REED_ACTION_WORD_NEXT_START);
    EXPECT(HID_KEY_A, 0, REED_ACTION_LINE_START);
    EXPECT(HID_KEY_E, 0, REED_ACTION_LINE_END);
    EXPECT(HID_KEY_A, MG_TERMINAL_MODIFIER_LEFT_SHIFT,
           REED_ACTION_LINE_START_INSERT);
    EXPECT(HID_KEY_E, MG_TERMINAL_MODIFIER_RIGHT_SHIFT,
           REED_ACTION_LINE_END_INSERT);
    EXPECT(HID_KEY_HOME, 0, REED_ACTION_LINE_START);
    EXPECT(HID_KEY_END, 0, REED_ACTION_LINE_END);
    EXPECT(HID_KEY_HOME, MG_TERMINAL_MODIFIER_LEFT_CTRL,
           REED_ACTION_DOCUMENT_START);
    EXPECT(HID_KEY_END, MG_TERMINAL_MODIFIER_RIGHT_CTRL,
           REED_ACTION_DOCUMENT_END);
    EXPECT(HID_KEY_G, 0, REED_ACTION_DOCUMENT_START);
    EXPECT(HID_KEY_G, MG_TERMINAL_MODIFIER_LEFT_SHIFT,
           REED_ACTION_DOCUMENT_END);
    EXPECT(HID_KEY_LEFT_BRACKET, 0, REED_ACTION_PARAGRAPH_PREVIOUS);
    EXPECT(HID_KEY_RIGHT_BRACKET, 0, REED_ACTION_PARAGRAPH_NEXT);
    EXPECT(HID_KEY_PAGE_UP, 0, REED_ACTION_PAGE_UP);
    EXPECT(HID_KEY_PAGE_DOWN, 0, REED_ACTION_PAGE_DOWN);
    EXPECT(HID_KEY_O, 0, REED_ACTION_OPEN_BELOW);
    EXPECT(HID_KEY_O, MG_TERMINAL_MODIFIER_LEFT_SHIFT,
           REED_ACTION_OPEN_ABOVE);
    EXPECT(HID_KEY_S, 0, REED_ACTION_SELECT_CHARACTER);
    EXPECT(HID_KEY_S, MG_TERMINAL_MODIFIER_LEFT_SHIFT,
           REED_ACTION_SELECT_LINE);
    EXPECT(HID_KEY_S, MG_TERMINAL_MODIFIER_RIGHT_ALT,
           REED_ACTION_SELECT_ALL);
    EXPECT(HID_KEY_D, 0, REED_ACTION_DELETE_CHAR);
    EXPECT(HID_KEY_BACKSPACE, 0, REED_ACTION_DELETE_LEFT);
    EXPECT(HID_KEY_DELETE, 0, REED_ACTION_DELETE_RIGHT);
    EXPECT(HID_KEY_U, 0, REED_ACTION_UNDO);
    EXPECT(HID_KEY_U, MG_TERMINAL_MODIFIER_LEFT_SHIFT, REED_ACTION_REDO);
    EXPECT(HID_KEY_SLASH, 0, REED_ACTION_COMMAND);
    EXPECT(HID_KEY_SLASH, MG_TERMINAL_MODIFIER_LEFT_SHIFT,
           REED_ACTION_SEARCH_PROMPT);

    /* Clipboard operations and replaced legacy aliases remain unbound. */
    EXPECT_NONE(0x06U, 0); /* c */
    EXPECT_NONE(0x1bU, 0); /* x */
    EXPECT_NONE(0x19U, 0); /* v */
    EXPECT_NONE(0x1aU, 0); /* w */
    EXPECT_NONE(0x1aU, MG_TERMINAL_MODIFIER_LEFT_ALT);
    EXPECT_NONE(HID_KEY_O, MG_TERMINAL_MODIFIER_LEFT_ALT);
    EXPECT_NONE(HID_KEY_O, MG_TERMINAL_MODIFIER_LEFT_ALT |
                           MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    EXPECT_NONE(0x33U, 0); /* ; */
    EXPECT_NONE(0x33U, MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    EXPECT_NONE(HID_KEY_B, MG_TERMINAL_MODIFIER_LEFT_ALT);
    EXPECT_NONE(HID_KEY_A, MG_TERMINAL_MODIFIER_LEFT_ALT);
    EXPECT_NONE(HID_KEY_E, MG_TERMINAL_MODIFIER_LEFT_ALT);
    EXPECT_NONE(HID_KEY_S, MG_TERMINAL_MODIFIER_LEFT_ALT |
                          MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    EXPECT_NONE(0x14U, MG_TERMINAL_MODIFIER_LEFT_ALT); /* Alt+Q */
    EXPECT_NONE(HID_KEY_LEFT, MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    EXPECT_NONE(HID_KEY_S, MG_TERMINAL_MODIFIER_LEFT_CTRL);
    EXPECT_NONE(HID_KEY_G, MG_TERMINAL_MODIFIER_LEFT_GUI);

#undef EXPECT_NONE
#undef EXPECT
}

static void force_zero_gap(reed_buffer_t *buffer)
{
    usize length = reed_buffer_length(buffer);

    buffer->capacity = length;
    buffer->gap_start = length;
    buffer->gap_end = length;
}

static void test_open_lines(void)
{
    reed_editor_t editor;
    char *saved;
    usize saved_length;

    editor_with_text(&editor, "first\nlast");
    assert(reed_buffer_cursor_set(&editor.buffer, 2U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O
    });
    expect_text(&editor.buffer, "first\n\nlast");
    assert(editor.mode == REED_MODE_INSERT && editor.insert_recorded);
    assert(reed_buffer_cursor(&editor.buffer) == 6U);
    assert(insert_codepoint(&editor, 'x'));
    finish_insert(&editor);
    expect_text(&editor.buffer, "first\nx\nlast");
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "first\nlast");
    assert(reed_buffer_redo(&editor.buffer));
    expect_text(&editor.buffer, "first\nx\nlast");
    reed_buffer_destroy(&editor.buffer);

    /* Shift+O opens before the current line, including at the first line. */
    editor_with_text(&editor, "first\nlast");
    assert(reed_buffer_cursor_set(&editor.buffer, 0U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    expect_text(&editor.buffer, "\nfirst\nlast");
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    assert(editor.mode == REED_MODE_INSERT);
    assert(insert_codepoint(&editor, 'x'));
    finish_insert(&editor);
    expect_text(&editor.buffer, "x\nfirst\nlast");
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "first\nlast");
    assert(reed_buffer_redo(&editor.buffer));
    expect_text(&editor.buffer, "x\nfirst\nlast");
    reed_buffer_destroy(&editor.buffer);

    /* Opening below an unterminated final line preserves its file format. */
    editor_with_text(&editor, "first\nlast");
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    assert(!reed_buffer_has_final_newline(&editor.buffer));
    assert(reed_buffer_cursor_set(&editor.buffer,
                                 reed_buffer_length(&editor.buffer)));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O
    });
    expect_text(&editor.buffer, "first\nlast\n");
    assert(reed_buffer_line_count(&editor.buffer) == 3U);
    assert(reed_buffer_cursor(&editor.buffer) == reed_buffer_length(
        &editor.buffer));
    assert(reed_buffer_has_final_newline(&editor.buffer));
    assert(reed_buffer_copy_file_allocated(&editor.buffer, &saved,
                                           &saved_length));
    assert(saved_length == 12U &&
           memcmp(saved, "first\nlast\n\n", 12U) == 0);
    free(saved);
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "first\nlast");
    assert(!reed_buffer_has_final_newline(&editor.buffer));
    assert(reed_buffer_redo(&editor.buffer));
    expect_text(&editor.buffer, "first\nlast\n");
    assert(reed_buffer_has_final_newline(&editor.buffer));
    reed_buffer_destroy(&editor.buffer);

    /* Existing final-newline state remains a file terminator, not a line. */
    editor_with_text(&editor, "last");
    assert(reed_buffer_set_final_newline(&editor.buffer, true));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    expect_text(&editor.buffer, "\nlast");
    assert(reed_buffer_has_final_newline(&editor.buffer));
    assert(reed_buffer_copy_file_allocated(&editor.buffer, &saved,
                                           &saved_length));
    assert(saved_length == 6U && memcmp(saved, "\nlast\n", 6U) == 0);
    free(saved);
    reed_buffer_destroy(&editor.buffer);

    editor_with_text(&editor, "");
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O
    });
    expect_text(&editor.buffer, "\n");
    assert(reed_buffer_line_count(&editor.buffer) == 2U);
    assert(reed_buffer_cursor(&editor.buffer) == 1U);
    assert(editor.mode == REED_MODE_INSERT);
    assert(reed_buffer_has_final_newline(&editor.buffer));
    reed_buffer_destroy(&editor.buffer);

    editor_with_text(&editor, "");
    assert(reed_buffer_set_final_newline(&editor.buffer, false));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    expect_text(&editor.buffer, "\n");
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    assert(editor.mode == REED_MODE_INSERT);
    assert(reed_buffer_has_final_newline(&editor.buffer));
    reed_buffer_destroy(&editor.buffer);
}

static void test_open_line_allocation_failure(void)
{
    reed_editor_t editor;
    usize undo_count;

    editor_with_text(&editor, "abc");
    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    force_zero_gap(&editor.buffer);
    undo_count = editor.buffer.undo.count;
    fail_malloc_after = 0;
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O
    });
    assert(fail_malloc_after == -1);
    expect_text(&editor.buffer, "abc");
    assert(editor.mode == REED_MODE_NORMAL);
    assert(reed_buffer_cursor(&editor.buffer) == 1U);
    assert(editor.buffer.undo.count == undo_count);

    /* Gap growth succeeds, but failure to allocate the undo snapshot must
     * still leave the text and history unchanged. */
    force_zero_gap(&editor.buffer);
    fail_malloc_after = 1;
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_O
    });
    assert(fail_malloc_after == -1);
    expect_text(&editor.buffer, "abc");
    assert(editor.mode == REED_MODE_NORMAL);
    assert(editor.buffer.undo.count == undo_count);
    reed_buffer_destroy(&editor.buffer);
}

static void test_word_boundaries(void)
{
    reed_editor_t editor;
    const char *text = "word next";

    editor_with_text(&editor, text);
    assert(word_current_next_end(&editor.buffer, 2U) == 4U);
    assert(word_next_start(&editor.buffer, 2U) == 5U);
    assert(word_current_next_end(&editor.buffer, 4U) == 9U);
    assert(word_next_start(&editor.buffer, 4U) == 5U);
    assert(word_current_previous_start(&editor.buffer, 2U) == 0U);
    assert(word_previous_end(&editor.buffer, 2U) == 0U);
    reed_buffer_destroy(&editor.buffer);

    editor_with_text(&editor, "word word");
    assert(word_current_previous_start(&editor.buffer, 7U) == 5U);
    reed_buffer_destroy(&editor.buffer);

    editor_with_text(&editor, "hello world");
    assert(word_previous_end(&editor.buffer, 8U) == 5U);
    reed_buffer_destroy(&editor.buffer);

    editor_with_text(&editor, "one,two 3_three\n\xc3\xa9!");
    assert(word_current_next_end(&editor.buffer, 1U) == 3U);
    assert(word_next_start(&editor.buffer, 1U) == 4U);
    assert(word_current_next_end(&editor.buffer, 4U) == 7U);
    assert(word_current_previous_start(&editor.buffer, 16U) == 16U);
    assert(word_current_next_end(&editor.buffer, 16U) == 18U);
    assert(reed_buffer_previous_boundary(&editor.buffer, 18U) == 16U);
    reed_buffer_destroy(&editor.buffer);
}

static void test_navigation_dispatch(void)
{
    reed_editor_t editor;

    editor_with_text(&editor, "ab\ncdef\nxy");
    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_H});
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_L});
    assert(reed_buffer_cursor(&editor.buffer) == 1U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_J});
    assert(reed_buffer_cursor(&editor.buffer) == 4U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_K});
    assert(reed_buffer_cursor(&editor.buffer) == 1U);

    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_A});
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_E});
    assert(reed_buffer_cursor(&editor.buffer) == 2U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_G});
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_G,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    assert(reed_buffer_cursor(&editor.buffer) == 10U);

    editor.screen_rows = 4U; /* Two text rows per viewport movement. */
    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    editor.desired_column = 1U;
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_PAGE_DOWN
    });
    assert(reed_buffer_cursor(&editor.buffer) == 9U);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_PAGE_UP
    });
    assert(reed_buffer_cursor(&editor.buffer) == 1U);
    reed_buffer_destroy(&editor.buffer);
}

static void test_paragraph_navigation(void)
{
    reed_editor_t editor;
    mg_terminal_key_event_t event;

    editor_with_text(&editor, "one\ncontinued\n\n \nnext\nline\n\nlast");
    assert(reed_buffer_cursor_set(&editor.buffer, 9U));
    event = key(HID_KEY_LEFT_BRACKET, 0);
    dispatch_normal(&editor, &event);
    assert(reed_buffer_cursor(&editor.buffer) == 0U);

    event = key(HID_KEY_RIGHT_BRACKET, 0);
    dispatch_normal(&editor, &event);
    assert(reed_buffer_cursor(&editor.buffer) == 17U);
    assert(reed_buffer_cursor_set(&editor.buffer, 18U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_LEFT_BRACKET
    });
    assert(reed_buffer_cursor(&editor.buffer) == 17U);

    assert(reed_buffer_cursor_set(&editor.buffer, 14U)); /* blank line */
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_RIGHT_BRACKET
    });
    assert(reed_buffer_cursor(&editor.buffer) == 17U);
    assert(reed_buffer_cursor_set(&editor.buffer, 0U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_LEFT_BRACKET
    });
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    reed_buffer_destroy(&editor.buffer);
}

static void test_selection_and_undo(void)
{
    reed_editor_t editor;
    mg_terminal_key_event_t event;
    usize start;
    usize end;

    editor_with_text(&editor, "alpha\nbeta\ngamma");
    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    event = key(HID_KEY_S, 0);
    dispatch_normal(&editor, &event);
    assert(editor.mode == REED_MODE_SELECTION);
    assert(editor.selection == REED_SELECTION_CHARACTER);
    assert(editor.selection_anchor == 1U);
    event = key(HID_KEY_RIGHT, 0);
    dispatch_selection(&editor, &event);
    assert(selection_bounds(&editor, &start, &end));
    assert(start == 1U && end == 2U);
    event = key(HID_KEY_I, 0);
    dispatch_selection(&editor, &event);
    assert(editor.mode == REED_MODE_SELECTION);
    event = key(HID_KEY_RIGHT, MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    dispatch_selection(&editor, &event);
    assert(reed_buffer_cursor(&editor.buffer) == 2U);

    event = key(HID_KEY_S, MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    dispatch_selection(&editor, &event);
    assert(editor.selection == REED_SELECTION_LINE);
    assert(editor.selection_anchor == 1U);
    assert(reed_buffer_cursor(&editor.buffer) == 2U);
    event = key(HID_KEY_J, 0);
    dispatch_selection(&editor, &event);
    assert(selection_bounds(&editor, &start, &end));
    assert(start == 0U && end == 11U);
    event = key(HID_KEY_S, 0);
    dispatch_selection(&editor, &event);
    assert(editor.selection == REED_SELECTION_CHARACTER);
    assert(editor.selection_anchor == 1U);
    event = key(HID_KEY_ESCAPE, 0);
    dispatch_selection(&editor, &event);
    assert(editor.mode == REED_MODE_NORMAL);
    assert(editor.selection == REED_SELECTION_NONE);
    assert(reed_buffer_cursor(&editor.buffer) == 8U);

    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_S});
    dispatch_selection(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_RIGHT});
    dispatch_selection(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_RIGHT});
    dispatch_selection(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_D});
    expect_text(&editor.buffer, "aha\nbeta\ngamma");
    assert(editor.mode == REED_MODE_NORMAL);
    assert(reed_buffer_cursor(&editor.buffer) == 1U);
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "alpha\nbeta\ngamma");
    assert(reed_buffer_redo(&editor.buffer));
    expect_text(&editor.buffer, "aha\nbeta\ngamma");

    /* Select all is a character selection and can be deleted as one edit. */
    editor.mode = REED_MODE_NORMAL;
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_S,
        .modifiers = MG_TERMINAL_MODIFIER_RIGHT_ALT
    });
    assert(editor.selection == REED_SELECTION_CHARACTER);
    assert(selection_bounds(&editor, &start, &end));
    assert(start == 0U && end == reed_buffer_length(&editor.buffer));
    dispatch_selection(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_D});
    expect_text(&editor.buffer, "");
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "aha\nbeta\ngamma");

    /* A line selection deletes its complete newline boundary in one edit. */
    editor.mode = REED_MODE_NORMAL;
    assert(reed_buffer_cursor_set(&editor.buffer, 7U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_S,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    dispatch_selection(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_J
    });
    dispatch_selection(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_D
    });
    expect_text(&editor.buffer, "aha");
    assert(editor.mode == REED_MODE_NORMAL);
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "aha\nbeta\ngamma");

    reed_buffer_destroy(&editor.buffer);
}

static void test_normal_deletion_and_insert_edges(void)
{
    reed_editor_t editor;

    editor_with_text(&editor, "abc\ndef");
    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_D});
    expect_text(&editor.buffer, "ac\ndef");
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "abc\ndef");

    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_BACKSPACE
    });
    expect_text(&editor.buffer, "bc\ndef");
    assert(reed_buffer_undo(&editor.buffer));
    expect_text(&editor.buffer, "abc\ndef");

    assert(reed_buffer_cursor_set(&editor.buffer, 0U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_DELETE});
    expect_text(&editor.buffer, "bc\ndef");
    assert(reed_buffer_undo(&editor.buffer));

    assert(reed_buffer_cursor_set(&editor.buffer, 5U));
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_A,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    assert(editor.mode == REED_MODE_INSERT);
    assert(reed_buffer_cursor(&editor.buffer) == 4U);
    finish_insert(&editor);
    dispatch_normal(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_E,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    assert(editor.mode == REED_MODE_INSERT);
    assert(reed_buffer_cursor(&editor.buffer) == 7U);
    reed_buffer_destroy(&editor.buffer);
}

static void test_selection_rendering(void)
{
    reed_editor_t editor;
    reed_frame_cell_t frame[4] = {0};

    editor_with_text(&editor, "abc");
    editor.selection = REED_SELECTION_CHARACTER;
    editor.selection_anchor = 0U;
    editor.mode = REED_MODE_SELECTION;
    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    assert(render_line_to_frame(&editor, 0U, 0U, 4U, frame));
    assert(frame[0].background == MG_TERMINAL_COLOR_BLUE);
    assert(frame[1].background == MG_TERMINAL_COLOR_BLACK);
    reed_buffer_destroy(&editor.buffer);

    editor_with_text(&editor, "abc\ndef");
    editor.selection = REED_SELECTION_LINE;
    editor.selection_anchor = 0U;
    editor.mode = REED_MODE_SELECTION;
    assert(reed_buffer_cursor_set(&editor.buffer, 5U));
    memset(frame, 0, sizeof(frame));
    assert(render_line_to_frame(&editor, 0U, 0U, 4U, frame));
    assert(frame[0].background == MG_TERMINAL_COLOR_BLUE);
    assert(frame[3].background == MG_TERMINAL_COLOR_BLUE);
    reed_buffer_destroy(&editor.buffer);
}

static void test_incremental_search(void)
{
    reed_editor_t editor;
    bool current = true;
    mg_terminal_key_event_t event;

    editor_with_text(&editor, "cat cat");
    assert(reed_buffer_cursor_set(&editor.buffer, 2U));
    event = key(HID_KEY_SLASH, MG_TERMINAL_MODIFIER_LEFT_SHIFT);
    dispatch_normal(&editor, &event);
    assert(editor.mode == REED_MODE_SEARCH);
    assert(editor.prompt == REED_PROMPT_SEARCH);
    memcpy(editor.input, "cat", 4U);
    editor.input_length = 3U;
    editor.input_cursor = 3U;
    assert(query_matches_at(&editor, 0U));
    assert(query_matches_at(&editor, 4U));
    assert(position_in_search_match(&editor, 1U, &current) && !current);
    accept_search(&editor);
    assert(editor.mode == REED_MODE_SEARCH);
    assert(editor.prompt == REED_PROMPT_NONE);
    assert(editor.search_match == 4U); /* Equal-distance tie chooses forward. */
    assert(reed_buffer_cursor(&editor.buffer) == 7U);

    dispatch_search(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_N});
    assert(reed_buffer_cursor(&editor.buffer) == 7U);
    assert(strcmp(editor.status, "No further match.") == 0);
    dispatch_search(&editor, &(mg_terminal_key_event_t){.keycode = HID_KEY_B});
    assert(editor.search_match == 0U);
    assert(reed_buffer_cursor(&editor.buffer) == 3U);
    dispatch_search(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_N,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    assert(editor.search_match == 4U);
    assert(reed_buffer_cursor(&editor.buffer) == 7U);
    assert(reed_buffer_cursor_set(&editor.buffer, 2U));
    dispatch_search(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_SLASH,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT
    });
    assert(editor.mode == REED_MODE_NORMAL);
    assert(reed_buffer_cursor(&editor.buffer) == 2U);
    assert(!position_in_search_match(&editor, 0U, NULL));

    assert(reed_buffer_cursor_set(&editor.buffer, 1U));
    start_search(&editor);
    memcpy(editor.input, "absent", 7U);
    editor.input_length = 6U;
    editor.input_cursor = 6U;
    accept_search(&editor);
    assert(editor.mode == REED_MODE_NORMAL);
    assert(reed_buffer_cursor(&editor.buffer) == 1U);
    assert(!editor.search_active);
    reed_buffer_destroy(&editor.buffer);
}

static void test_search_prompt_input_and_modifiers(void)
{
    reed_editor_t editor;

    editor_with_text(&editor, "cat cat");
    assert(reed_buffer_cursor_set(&editor.buffer, 2U));
    start_search(&editor);
    assert(input_insert_bytes(&editor, "c", 1U));
    assert(input_insert_bytes(&editor, "a", 1U));
    assert(input_insert_bytes(&editor, "t", 1U));
    assert(strcmp(editor.input, "cat") == 0);
    assert(query_matches_at(&editor, 0U) && query_matches_at(&editor, 4U));
    input_delete_left(&editor);
    assert(strcmp(editor.input, "ca") == 0);
    assert(query_matches_at(&editor, 0U) && query_matches_at(&editor, 4U));
    editor.input_cursor = input_previous_boundary(&editor,
                                                  editor.input_cursor);
    input_delete_right(&editor);
    assert(strcmp(editor.input, "c") == 0);
    cancel_prompt(&editor);
    assert(editor.mode == REED_MODE_NORMAL);
    assert(reed_buffer_cursor(&editor.buffer) == 2U);

    editor.search_active = true;
    memcpy(editor.search_text, "cat", 4U);
    editor.search_length = 3U;
    editor.search_match = 0U;
    assert(reed_buffer_cursor_set(&editor.buffer, 0U));
    dispatch_search(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_N,
        .modifiers = MG_TERMINAL_MODIFIER_LEFT_GUI
    });
    assert(reed_buffer_cursor(&editor.buffer) == 0U);
    dispatch_search(&editor, &(mg_terminal_key_event_t){
        .keycode = HID_KEY_N
    });
    assert(reed_buffer_cursor(&editor.buffer) == 3U);
    reed_buffer_destroy(&editor.buffer);
}

static void test_command_interface(void)
{
    const char *argument;
    reed_editor_t editor;

    assert(parse_command_line("s", &argument) == REED_COMMAND_SAVE);
    assert(!*argument);
    assert(parse_command_line("sq", &argument) == REED_COMMAND_SAVE_QUIT);
    assert(parse_command_line("q", &argument) == REED_COMMAND_QUIT);
    assert(parse_command_line("saveas /tmp/x", &argument) ==
           REED_COMMAND_SAVE_AS && strcmp(argument, "/tmp/x") == 0);
    assert(parse_command_line("  open file", &argument) == REED_COMMAND_OPEN &&
           strcmp(argument, "file") == 0);
    assert(parse_command_line("find text", &argument) == REED_COMMAND_FIND &&
           strcmp(argument, "text") == 0);
    assert(parse_command_line("s extra", &argument) == REED_COMMAND_SAVE &&
           strcmp(argument, "extra") == 0);
    assert(parse_command_line("s/foo", &argument) == REED_COMMAND_UNKNOWN);
    assert(parse_command_line("", &argument) == REED_COMMAND_UNKNOWN);

    editor_with_text(&editor, "");
    memcpy(editor.input, "sav", 4U);
    editor.input_length = 3U;
    editor.input_cursor = 3U;
    complete_command_name(&editor);
    assert(strcmp(editor.input, "saveas") == 0);
    editor.input_length = 1U;
    editor.input_cursor = 1U;
    editor.input[1] = '\0';
    complete_command_name(&editor);
    assert(strcmp(editor.input, "s") == 0); /* Ambiguous prefix is unchanged. */
    reed_buffer_destroy(&editor.buffer);
}

int main(void)
{
    test_complete_keymap();
    test_open_lines();
    test_open_line_allocation_failure();
    test_word_boundaries();
    test_navigation_dispatch();
    test_paragraph_navigation();
    test_selection_and_undo();
    test_normal_deletion_and_insert_edges();
    test_selection_rendering();
    test_incremental_search();
    test_search_prompt_input_and_modifiers();
    test_command_interface();
    puts("Reed interaction: keymap, navigation, selection, search, and commands passed");
    return 0;
}
