/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "../config.h"
#include "../history.h"
#include "commands.h"

bool execute_reload(shell_state_t *state, const shell_command_t *command)
{
    char diagnostic[512];
    bool loaded;

    if (!state || !command) return true;
    loaded = shoot_config_reload(&state->config, diagnostic,
                                 sizeof(diagnostic));
    shoot_history_apply_config(state->history, &state->config);
    if (!loaded) {
        printf("Shoot: %s\n", diagnostic[0] ? diagnostic :
               "configuration was rejected; using built-in defaults.");
        return true;
    }
    if (diagnostic[0]) printf("Shoot: %s\n", diagnostic);
    printf("Shoot configuration reloaded.\n");
    return true;
}
