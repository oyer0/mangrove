#include <mg/process.h>
#include "commands.h"

bool execute_exit(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    (void)command;
    process_exit(0);
}
