/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "builtin.h"

typedef struct shell_expanded_arguments {
    const char *arguments[SHOOT_MAX_ARGUMENTS];
    bool owned[SHOOT_MAX_ARGUMENTS];
    usize count;
} shell_expanded_arguments_t;

/* Expand pathname metacharacters in argument words. On any error, no partial
 * result is returned and all allocated names are released. */
mg_result_t shell_expand_arguments(const shell_command_t *command,
                                  shell_expanded_arguments_t *out,
                                  const char **out_failed_pattern);
void shell_expanded_arguments_destroy(shell_expanded_arguments_t *expanded);
