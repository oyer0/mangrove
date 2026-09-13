/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "shell.h"

int main(int argc, char **argv)
{
    if (argc == 1) {
        shell_run();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("Usage: shoot [script.shoot]\n");
        return 0;
    }
    if (argc != 2) {
        printf("Usage: shoot [script.shoot]\n");
        return 1;
    }
    return shell_run_script(argv[1]);
}
