#pragma once

#include <mg/types.h>

/* Resolves a user path against the caller's inherited current directory. */
bool command_resolve_path(const char *input, char *output, usize capacity);
