#pragma once

#include <mg/error.h>
#include <editor_actions.h>

/* Session-local, caller-owned history storage. */
typedef struct mg_line_history {
    char *storage;
    usize entry_capacity;
    usize capacity;
    usize count;
    usize first;
    isize index;
} mg_line_history_t;

/* Reusable single-line editor state.  The caller owns buffer and prompt. */
typedef struct mg_line_editor {
    char *buffer;
    usize capacity;
    usize length;
    usize cursor;
    usize rendered_length;
    usize selection_anchor;
    bool selection_active;
    const char *prompt;
    mg_line_history_t *history;
} mg_line_editor_t;

void line_editor_init(mg_line_editor_t *editor, char *buffer,
                      usize capacity, const char *prompt);
void line_editor_set_prompt(mg_line_editor_t *editor, const char *prompt);
void line_editor_history_init(mg_line_history_t *history, char *storage,
                              usize entry_capacity, usize capacity);
void line_editor_set_history(mg_line_editor_t *editor,
                             mg_line_history_t *history);
/* Applies a semantic action without knowing which physical key produced it. */
bool line_editor_apply_action(mg_line_editor_t *editor,
                              editor_action_t action);
/* Reads one submitted line, returning its length or a negative error. */
mg_result_t line_editor_read_line(mg_line_editor_t *editor);
