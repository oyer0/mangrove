#include <stdio.h>
#include "../common/help.h"

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "clear", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    printf("\x1b[2J");
    return 0;
}
