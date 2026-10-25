/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>
#include <mg/error.h>

void command_path_release(char **path);
#define COMMAND_PATH_AUTO __attribute__((cleanup(command_path_release)))

bool command_path_copy_alloc(const char *input, char **out_path);
bool command_path_join_alloc(const char *parent, const char *name,
                             char **out_path);
mg_result_t command_getcwd_alloc(char **out_path);

/* Validates/copies a native path without converting relative authority or
 * collapsing components. The kernel resolves it against retained CWD identity. */
bool command_resolve_path(const char *input, char *output, usize capacity);

/* Expands the current user's home directory for a shell path token.  The
 * caller supplies whether the token's leading character was eligible for
 * shell expansion; single-quoted tokens pass false. */
bool command_expand_home_path(const char *input, bool allow_home,
                              char *output, usize capacity);
bool command_expand_home_path_alloc(const char *input, bool allow_home,
                                    char **out_path);

/* Builds /bin for a bare command; preserves explicit absolute/relative paths. */
bool command_build_executable_path(const char *name, char *path,
                                   usize capacity);
bool command_build_executable_path_alloc(const char *name, char **out_path);
