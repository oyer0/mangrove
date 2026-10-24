/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/error.h>
#include <mg/filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "../common/text_pager.h"

#define INFO_PROGRAM_NAME_CAPACITY 64U

static bool valid_program_name(const char *name)
{
    usize length = 0U;

    if (!name || !name[0]) return false;
    while (name[length]) {
        char character = name[length];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_')) return false;
        if (++length >= INFO_PROGRAM_NAME_CAPACITY) return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    char path[MG_PATH_MAX_BYTES + 1U];
    char *text = NULL;
    usize length = 0U;
    mg_result_t result;
    int path_length;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2 || !valid_program_name(argc > 1 ? argv[1] : NULL)) {
        command_usage_error(argv[0], "info PROGRAM",
                            argc > 1 ? argv[1] : NULL);
        return 1;
    }
    path_length = snprintf(path, sizeof(path), "/share/info/%s", argv[1]);
    if (path_length < 0 || (usize)path_length >= sizeof(path)) {
        printf("info: program name is too long.\n");
        return 1;
    }
    result = text_pager_read_file(path, &text, &length);
    if (result != MG_OK) {
        if (result == MG_ERR_NOT_FOUND)
            printf("info: no manual is installed for %s.\n", argv[1]);
        else
            printf("info: %s: %s\n", path, error_string(result));
        return 1;
    }
    result = text_pager_run(argv[1], path, text, length);
    free(text);
    if (result != MG_OK) {
        printf("info: could not display %s: %s\n", path,
               error_string(result));
        return 1;
    }
    return 0;
}
