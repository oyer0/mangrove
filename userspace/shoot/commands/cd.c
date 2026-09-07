/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/process.h>
#include "../../common/path.h"
#include "commands.h"

bool execute_cd(shell_state_t *state, const shell_command_t *command)
{
    char expanded_path[256];
    mg_result_t result;
    usize cwd_size = 0;
    const char *path = command->argument_count == 0 ? "~" :
                       command->arguments[0];
    bool allow_home = command->argument_count == 0 ||
                      command->argument_home_expand[0];

    if (!command_expand_home_path(path, allow_home,
                                  expanded_path, sizeof(expanded_path))) {
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
    result = process_getcwd(state->cwd, sizeof(state->cwd), &cwd_size);
    if (result_is_error(result)) {
        printf("Could not read current directory: %s.\n", error_string(result));
        return true;
    }
    return true;
}
