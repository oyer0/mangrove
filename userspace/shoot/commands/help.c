#include "../help.h"
#include "commands.h"

bool execute_help(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    return render_help(command);
}
