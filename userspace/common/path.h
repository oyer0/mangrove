/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

/* Validates/copies a native path without converting relative authority or
 * collapsing components. The kernel resolves it against retained CWD identity. */
bool command_resolve_path(const char *input, char *output, usize capacity);

/* Expands the current user's home directory for a shell path token.  The
 * caller supplies whether the token's leading character was eligible for
 * shell expansion; single-quoted tokens pass false. */
bool command_expand_home_path(const char *input, bool allow_home,
                              char *output, usize capacity);

/* Builds /bin for a bare command; preserves explicit absolute/relative paths. */
bool command_build_executable_path(const char *name, char *path,
                                   usize capacity);
