#include <stdio.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/process.h>
#include "commands.h"

bool execute_jump(shell_state_t *state, const shell_command_t *command)
{
    mg_result_t result;
    usize cwd_size = 0;
    const char *path = command->arguments[0];

    result = process_chdir(path);
    if (result_is_error(result)) {
        printf("Could not jump to \"%s\": %s.\n", path, error_string(result));
        return true;
    }
    result = process_getcwd(state->cwd, sizeof(state->cwd), &cwd_size);
    if (result_is_error(result)) {
        printf("Could not read current directory: %s.\n", error_string(result));
        return true;
    }
    puts("Directory changed.");
    return true;
}
