#include "../builtin.h"
#include "../version.h"
#include "commands.h"

bool execute_version(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    (void)command;
    shoot_print_system_version();
    return true;
}
