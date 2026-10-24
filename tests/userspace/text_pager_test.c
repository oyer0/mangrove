/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <mg/utf8.h>
#include "../../userspace/common/text_pager.c"

static void test_layout_and_resize(void)
{
    text_pager_document_t document = {0};
    static const char wrapped[] = "abcdef";

    assert(text_pager_document_init(&document, wrapped, sizeof(wrapped) - 1U,
                                    3U, 4U) == MG_OK);
    assert(document.row_count == 2U);
    assert(document.rows[0].byte_offset == 0U &&
           document.rows[0].cells == 3U);
    assert(document.rows[1].byte_offset == 3U &&
           document.rows[1].cells == 3U);
    assert(text_pager_document_relayout(&document, 2U) == MG_OK);
    assert(document.row_count == 3U);
    assert(document.rows[1].byte_offset == 2U &&
           document.rows[1].cells == 2U);
    text_pager_document_destroy(&document);
}

static void test_newlines_tabs_and_utf8(void)
{
    text_pager_document_t document = {0};
    static const char lines[] = "ab\ncd\n";
    static const char tabbed[] = "a\tb";
    static const char crlf[] = "a\r\nb";
    static const u8 utf8[] = {0xC3U, 0xA9U, 'x'};
    static const u8 malformed[] = {0xFFU, 'x'};

    assert(text_pager_document_init(&document, lines, sizeof(lines) - 1U,
                                    2U, 4U) == MG_OK);
    assert(document.row_count == 3U);
    assert(document.rows[0].cells == 2U && document.rows[1].cells == 2U &&
           document.rows[2].cells == 0U);
    text_pager_document_destroy(&document);

    assert(text_pager_document_init(&document, tabbed, sizeof(tabbed) - 1U,
                                    3U, 4U) == MG_OK);
    assert(document.row_count == 2U);
    assert(document.rows[0].cells == 3U);
    assert(document.rows[1].byte_offset == 1U &&
           document.rows[1].tab_remaining == 1U &&
           document.rows[1].logical_column == 3U);
    text_pager_document_destroy(&document);

    assert(text_pager_document_init(&document, crlf, sizeof(crlf) - 1U,
                                    10U, 4U) == MG_OK);
    assert(document.row_count == 2U && document.rows[1].byte_offset == 3U);
    text_pager_document_destroy(&document);

    assert(text_pager_document_init(&document, utf8, sizeof(utf8), 1U, 4U) ==
           MG_OK);
    assert(document.row_count == 2U && document.rows[1].byte_offset == 2U);
    text_pager_document_destroy(&document);

    assert(text_pager_document_init(&document, malformed, sizeof(malformed),
                                    4U, 4U) == MG_OK);
    assert(document.row_count == 1U && document.rows[0].cells == 2U);
    text_pager_document_destroy(&document);

    assert(text_pager_document_init(&document, NULL, 0U, 4U, 4U) == MG_OK);
    assert(document.row_count == 1U && document.rows[0].cells == 0U);
    text_pager_document_destroy(&document);
}

static void test_literal_search_and_wrapped_highlight(void)
{
    text_pager_document_t document = {0};
    text_pager_runtime_t runtime = {0};
    static const char text[] = "cat x cat";
    static const char wrapped[] = "abcdef";
    text_pager_cell_t cells[6] = {0};

    assert(text_pager_document_init(&document, text, sizeof(text) - 1U,
                                    4U, 4U) == MG_OK);
    assert(pager_find_closest_match(&document, "cat", 3U, 4U) == 6U);
    assert(pager_find_next_match(&document, "cat", 3U, 3U) == 6U);
    assert(pager_find_previous_match(&document, "cat", 3U, 6U) == 0U);
    assert(pager_find_next_match(&document, "missing", 7U, 0U) ==
           (usize)-1);
    text_pager_document_destroy(&document);

    assert(text_pager_document_init(&document, wrapped,
                                    sizeof(wrapped) - 1U, 3U, 4U) == MG_OK);
    runtime.document = document;
    runtime.screen_columns = 3U;
    runtime.searching = true;
    memcpy(runtime.query, "cde", 4U);
    runtime.query_length = 3U;
    runtime.frame = cells;
    pager_render_text_row(&runtime, cells, 0U, 0U);
    pager_render_text_row(&runtime, cells, 1U, 1U);
    assert(cells[2].background == MG_TERMINAL_COLOR_BRIGHT_YELLOW);
    assert(cells[3].background == MG_TERMINAL_COLOR_BRIGHT_YELLOW);
    assert(cells[4].background == MG_TERMINAL_COLOR_BRIGHT_YELLOW);
    text_pager_document_destroy(&document);
}

static void test_search_state_and_nonwrapping_navigation(void)
{
    text_pager_runtime_t runtime = {0};
    static const char text[] = "cat x cat";
    mg_terminal_key_event_t event;

    assert(text_pager_document_init(&runtime.document, text,
                                    sizeof(text) - 1U, 4U, 4U) == MG_OK);
    runtime.screen_rows = 3U;
    pager_begin_search(&runtime);
    event = (mg_terminal_key_event_t){.codepoint = 'c'};
    (void)pager_handle_search_prompt(&runtime, &event);
    event.codepoint = 'a';
    (void)pager_handle_search_prompt(&runtime, &event);
    event.codepoint = 't';
    (void)pager_handle_search_prompt(&runtime, &event);
    event.keycode = MG_TERMINAL_KEY_ENTER;
    event.codepoint = 0U;
    (void)pager_handle_search_prompt(&runtime, &event);
    assert(runtime.searching && runtime.current_match == 0U);
    event.keycode = 0U;
    event.codepoint = 'n';
    assert(!pager_handle_event(&runtime, &event));
    assert(runtime.current_match == 6U);
    event.codepoint = 'n';
    assert(!pager_handle_event(&runtime, &event));
    assert(runtime.current_match == 6U &&
           strcmp(runtime.status, "No further match.") == 0);
    event.codepoint = 'b';
    assert(!pager_handle_event(&runtime, &event));
    assert(runtime.current_match == 0U);
    event.keycode = MG_TERMINAL_KEY_ESCAPE;
    event.codepoint = 0U;
    assert(!pager_handle_event(&runtime, &event));
    assert(!runtime.searching && runtime.top_row == 0U);
    assert(pager_handle_event(&runtime, &event));
    text_pager_document_destroy(&runtime.document);
}

static void test_navigation_ignores_command_modifiers(void)
{
    text_pager_runtime_t runtime = {0};
    static const char text[] = "one\ntwo\nthree\nfour\nfive";
    mg_terminal_key_event_t event = {0};

    assert(text_pager_document_init(&runtime.document, text,
                                    sizeof(text) - 1U, 8U, 4U) == MG_OK);
    runtime.screen_rows = 3U;
    event.codepoint = 'j';
    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_ALT;
    assert(!pager_handle_event(&runtime, &event));
    assert(runtime.top_row == 0U);
    event.codepoint = 'q';
    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_CTRL;
    assert(!pager_handle_event(&runtime, &event));
    event.modifiers = 0U;
    assert(pager_handle_event(&runtime, &event));
    text_pager_document_destroy(&runtime.document);
}

int main(void)
{
    test_layout_and_resize();
    test_newlines_tabs_and_utf8();
    test_literal_search_and_wrapped_highlight();
    test_search_state_and_nonwrapping_navigation();
    test_navigation_ignores_command_modifiers();
    return 0;
}
