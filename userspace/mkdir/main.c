/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/path.h"

int main(int argc, char **argv)
{
    bool failed = false;
    bool have_target = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    for (int index = 1; index < argc; index++) {
        char path[256];
        mg_result_t result;

        if (!argv[index] || argv[index][0] == '-') {
            printf("Unknown option: %s.\n",
                   argv[index] ? argv[index] : "(null)");
            failed = true;
            continue;
        }
        have_target = true;
        if (!command_resolve_path(argv[index], path, sizeof(path))) {
            printf("Could not create directory \"%s\": invalid path.\n",
                   argv[index]);
            failed = true;
            continue;
        }
        result = directory_create(path);
        if (result == MG_ERR_ALREADY_EXISTS) {
            printf("Could not create directory \"%s\": already exists.\n",
                   argv[index]);
            failed = true;
            continue;
        }
        if (result_is_error(result)) {
            printf("Could not create directory \"%s\": %s.\n", argv[index],
                   error_string(result));
            failed = true;
        }
    }
    if (!have_target) {
        command_usage_error(argv[0], "mkdir <directory>...", NULL);
        return 1;
    }
    return failed ? 1 : 0;
}
