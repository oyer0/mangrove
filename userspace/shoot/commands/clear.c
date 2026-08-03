#include <stdio.h>
#include "commands.h"

bool execute_clear(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    (void)command;
    printf("\x1b[2J");
    return true;
}
