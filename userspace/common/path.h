#pragma once

#include <mg/types.h>

/* Resolves a user path against the caller's inherited current directory. */
bool command_resolve_path(const char *input, char *output, usize capacity);

/* Expands the current user's home directory for a shell path token.  The
 * caller supplies whether the token's leading character was eligible for
 * shell expansion; single-quoted tokens pass false. */
bool command_expand_home_path(const char *input, bool allow_home,
                              char *output, usize capacity);

/* Builds the installed /bin path for a bare command name, or preserves an
 * already absolute executable path. */
bool command_build_executable_path(const char *name, char *path,
                                   usize capacity);
