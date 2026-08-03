#include <stdio.h>
#include <string.h>
#include <mg/filesystem.h>
#include "commands.h"

static bool build_external_path(const char *name, char *path, usize path_capacity)
{
    usize name_length;

    if (!name || !path || path_capacity < 6) return false;
    name_length = strlen(name);
    if (name_length == 0 || name_length + 6 > path_capacity) return false;
    strcpy(path, "/bin/");
    strcpy(path + 5, name);
    return true;
}

bool execute_locate(shell_state_t *state, const shell_command_t *command)
{
    char path[256];
    mg_path_info_t info;
    mg_result_t result;

    (void)state;
    if (!build_external_path(command->arguments[0], path, sizeof(path))) {
        puts("not found");
        return true;
    }
    result = path_info(path, &info);
    if (result_is_error(result) || info.type != MG_PATH_TYPE_FILE) {
        puts("not found");
        return true;
    }
    puts(path);
    return true;
}
