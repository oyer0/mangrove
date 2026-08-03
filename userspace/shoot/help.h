#pragma once

#include <types.h>
#include "builtin.h"

void append_padded(char *buf, usize *pos, usize max, const char *str, usize width);
bool render_help(const shell_command_t *command);
