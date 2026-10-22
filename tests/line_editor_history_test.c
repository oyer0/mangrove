/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/line_editor.h>
#include <assert.h>
#include <string.h>

int main(void)
{
    char storage[3U * 32U];
    char draft[32];
    char buffer[32];
    mg_line_history_t history;
    mg_line_editor_t editor;

    line_editor_history_init(&history, storage, 32U, 3U);
    line_editor_history_set_draft(&history, draft, sizeof(draft));
    assert(line_editor_history_add_text(&history, "first command"));
    assert(line_editor_history_add_text(&history, "history -a"));

    line_editor_init(&editor, buffer, sizeof(buffer), "developer ~: ");
    line_editor_set_history(&editor, &history);
    memcpy(buffer, "ty", sizeof("ty"));
    editor.length = sizeof("ty") - 1U;
    editor.cursor = 1U;

    assert(line_editor_apply_action(&editor, EDITOR_ACTION_HISTORY_PREVIOUS));
    assert(!strcmp(buffer, "history -a"));
    assert(history.index == 1 && history.draft_valid);
    assert(!strcmp(draft, "ty") && history.draft_cursor == 1U);

    assert(line_editor_apply_action(&editor, EDITOR_ACTION_HISTORY_PREVIOUS));
    assert(!strcmp(buffer, "first command") && history.index == 0);
    assert(line_editor_apply_action(&editor, EDITOR_ACTION_HISTORY_NEXT));
    assert(!strcmp(buffer, "history -a") && history.index == 1);
    assert(line_editor_apply_action(&editor, EDITOR_ACTION_HISTORY_NEXT));
    assert(!strcmp(buffer, "ty") && editor.length == 2U &&
           editor.cursor == 1U && history.index == -1 && !history.draft_valid);
    return 0;
}
