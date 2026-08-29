#include <stdio.h>
#include <mangrove_version.h>
#include "../common/help.h"

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "version", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    printf("%s %s %s %s %s\n",
           MANGROVE_NAME, MANGROVE_VERSION,
           PITH_NAME, MANGROVE_VERSION, MANGROVE_ARCH);
    return 0;
}
