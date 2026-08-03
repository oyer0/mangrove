#include <stdio.h>
#include "commands.h"

bool execute_where(shell_state_t *state, const shell_command_t *command)
{
    (void)command;
    puts(state->cwd);
    return true;
}
