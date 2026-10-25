/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "builtin.h"

typedef enum shell_parse_result {
    SHELL_PARSE_OK,
    SHELL_PARSE_EMPTY,
    SHELL_PARSE_TOO_MANY_ARGUMENTS,
    SHELL_PARSE_INVALID_SYNTAX,
    SHELL_PARSE_NO_MEMORY,
} shell_parse_result_t;

typedef enum shell_assignment_result {
    SHELL_ASSIGNMENT_NOT_PRESENT,
    SHELL_ASSIGNMENT_OK,
    SHELL_ASSIGNMENT_INVALID,
    SHELL_ASSIGNMENT_NO_MEMORY,
} shell_assignment_result_t;

shell_parse_result_t shell_parse_command(shell_state_t *state,
                                          const char *line,
                                          shell_command_t *command);
shell_assignment_result_t shell_try_assignment(shell_state_t *state,
                                                const char *line);
bool shell_evaluate_condition(shell_state_t *state, const char *text,
                              bool *out_value);
bool shell_line_keyword(const char *line, const char *keyword,
                        const char **out_rest);
bool shell_line_is_blank_or_comment(const char *line);
