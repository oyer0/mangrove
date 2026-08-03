#include <stdio.h>
#include <string.h>
#include "shell.h"
#include "version.h"

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            shoot_print_version();
            return 0;
        }
        printf("Unknown option: %s\nUsage: shoot [-v | --version]\n", argv[1]);
        return 1;
    }

    shell_run();
    return 0;
}
