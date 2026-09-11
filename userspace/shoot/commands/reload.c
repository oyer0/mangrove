/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "../config.h"
#include "../history.h"
#include "commands.h"

bool execute_reload(shell_state_t *state, const shell_command_t *command)
{
    if (!state || !command) return true;
    if (!shoot_config_reload(&state->config)) {
        printf("Could not reload Shoot configuration; keeping current settings.\n");
        return true;
    }
    shoot_history_apply_config(state->history, &state->config);
    printf("Shoot configuration reloaded.\n");
    return true;
}
