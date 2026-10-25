/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/process.h>
#include "../../common/path.h"
#include "commands.h"

bool execute_jump(shell_state_t *state, const shell_command_t *command)
{
    COMMAND_PATH_AUTO char *expanded_path = NULL;
    mg_result_t result;
    char *new_cwd = NULL;
    const char *path = command->argument_count == 0 ? "~" :
                       command->arguments[0];
    bool allow_home = command->argument_count == 0 ||
                      command->argument_home_expand[0];

    if (!command_expand_home_path_alloc(path, allow_home, &expanded_path)) {
        printf("Could not change directory to \"%s\": home directory unavailable.\n",
               path);
        return true;
    }

    result = process_chdir(expanded_path);
    if (result_is_error(result)) {
        printf("Could not change directory to \"%s\": %s.\n", path,
               error_string(result));
        return true;
    }
    result = command_getcwd_alloc(&new_cwd);
    if (result_is_error(result)) {
        free(state->cwd);
        state->cwd = NULL;
        printf("Could not read current directory: %s.\n", error_string(result));
        return true;
    }
    free(state->cwd);
    state->cwd = new_cwd;
    return true;
}
