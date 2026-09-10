/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/terminal.h>
#include <editor_actions.h>

typedef struct mg_line_editor mg_line_editor_t;

typedef mg_result_t (*mg_line_completion_fn)(mg_line_editor_t *editor,
                                             void *context);

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
struct mg_line_editor {
    char *buffer;
    usize capacity;
    usize length;
    /* When active, cursor is the selection head (the moving caret end). */
    usize cursor;
    usize rendered_length;
    usize selection_anchor;
    bool selection_active;
    bool cursor_suppressed;
    bool cursor_needs_sync;
    bool temporary_display_active;
    const char *prompt;
    mg_line_history_t *history;
    mg_terminal_color_t prompt_foreground;
    mg_terminal_color_t prompt_background;
    bool prompt_styled;
    mg_line_completion_fn completion;
    void *completion_context;
    bool prompt_drawn;
};

void line_editor_init(mg_line_editor_t *editor, char *buffer,
                      usize capacity, const char *prompt);
void line_editor_set_prompt(mg_line_editor_t *editor, const char *prompt);
void line_editor_set_prompt_style(mg_line_editor_t *editor,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background);
void line_editor_set_completion(mg_line_editor_t *editor,
                                mg_line_completion_fn completion,
                                void *context);
void line_editor_history_init(mg_line_history_t *history, char *storage,
                              usize entry_capacity, usize capacity);
void line_editor_set_history(mg_line_editor_t *editor,
                             mg_line_history_t *history);
/* Applies a semantic action without knowing which physical key produced it. */
bool line_editor_apply_action(mg_line_editor_t *editor,
                              editor_action_t action);
/* Prepares the line editor for the next input line and draws the prompt into current console stream. */
mg_result_t line_editor_prepare_next_prompt(mg_line_editor_t *editor);
/* Clears the currently rendered input row before a completion listing. */
mg_result_t line_editor_clear_line(mg_line_editor_t *editor);
/* Marks and clears completion rows rendered as ephemeral terminal UI. */
void line_editor_set_temporary_display(mg_line_editor_t *editor, bool active);
mg_result_t line_editor_clear_temporary_display(mg_line_editor_t *editor);
/* Reads one submitted line, returning its length or a negative error. */
mg_result_t line_editor_read_line(mg_line_editor_t *editor);
