#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    mg_result_t result;

    if (argc != 1) {
        printf("Usage: reboot\n");
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
