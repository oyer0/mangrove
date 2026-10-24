/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/filesystem.h>
#include <mg/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "../common/text_pager.h"

int main(int argc, char **argv)
{
    char *text = NULL;
    usize length = 0U;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2 || !argv[1] || !argv[1][0]) {
        command_usage_error(argv[0], "view FILE",
                            argc > 1 ? argv[1] : NULL);
        return 1;
    }

    result = text_pager_read_file(argv[1], &text, &length);
    if (result != MG_OK) {
        printf("view: %s: %s\n", argv[1], error_string(result));
        return 1;
    }
    result = text_pager_run(argv[1], argv[1], text, length);
    free(text);
    if (result != MG_OK) {
        printf("view: could not display %s: %s\n", argv[1],
               error_string(result));
        return 1;
    }
    return 0;
}
