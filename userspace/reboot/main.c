#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>
#include "../common/help.h"

int main(int argc, char **argv)
{
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "reboot", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    result = system_reboot();
    if (result_is_error(result)) {
        printf("Could not reboot: %s.\n", error_string(result));
        return 1;
    }
    printf("Could not reboot: reset did not complete.\n");
    return 1;
}
