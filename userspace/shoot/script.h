/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "shell.h"

#define SHOOT_MAX_SCRIPT_NESTING 64U

typedef enum shell_statement_kind {
    SHELL_STATEMENT_COMMAND,
    SHELL_STATEMENT_IF,
    SHELL_STATEMENT_ELSE,
    SHELL_STATEMENT_WHILE,
    SHELL_STATEMENT_END,
} shell_statement_kind_t;

typedef struct shell_statement {
    shell_statement_kind_t kind;
    usize line_number;
    usize matching;
    usize text_offset;
    usize text_length;
} shell_statement_t;

typedef struct shell_script {
    shell_statement_t *statements;
    usize count;
    usize capacity;
    char *source;
    usize source_length;
    usize source_capacity;
    usize open_blocks[SHOOT_MAX_SCRIPT_NESTING];
    usize depth;
} shell_script_t;

bool shell_script_init(shell_script_t *script);
void shell_script_destroy(shell_script_t *script);
bool shell_script_append(shell_script_t *script, const char *line,
                         usize line_number, const char **out_error);
bool shell_script_complete(const shell_script_t *script);
bool shell_script_execute(shell_script_t *script, shell_state_t *state,
                          const char *source);
int shell_run_script(const char *path);
