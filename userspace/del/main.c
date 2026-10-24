/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/path.h"

int main(int argc, char **argv)
{
    char path[256];
    mg_path_info_t info;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2) {
        command_usage_error(argv[0], "del <file|empty-directory>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    if (!command_resolve_path(argv[1], path, sizeof(path))) {
        printf("Could not delete \"%s\": invalid path.\n", argv[1]);
        return 1;
    }
    result = path_info(path, &info);
    if (result_is_error(result)) {
        printf("Could not delete \"%s\": %s.\n", argv[1],
               error_string(result));
        return 1;
    }
    if (info.type != MG_PATH_TYPE_FILE &&
        info.type != MG_PATH_TYPE_DIRECTORY) {
        printf("Could not delete \"%s\": unsupported object type.\n",
               argv[1]);
        return 1;
    }

    result = path_remove(path);
    if (result == MG_ERR_NOT_EMPTY) {
        printf("Could not delete \"%s\": directory is not empty; recursive removal is not enabled.\n",
               argv[1]);
        return 1;
    }
    if (result_is_error(result)) {
        printf("Could not delete \"%s\": %s.\n", argv[1],
               error_string(result));
        return 1;
    }
    return 0;
}
