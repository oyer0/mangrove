/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "editor.h"

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc > 2) {
        command_usage_error(argv[0], "reed [FILE]", argv[2]);
        return 1;
    }
    return reed_run(argc == 2 ? argv[1] : NULL);
}
