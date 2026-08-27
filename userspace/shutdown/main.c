#include <mangrove.h>
#include <mg/error.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    mg_result_t result;

    if (argc != 1) {
        printf("Usage: shutdown\n");
        return 1;
    }
    result = system_poweroff();
    if (result_is_error(result)) {
        printf("Could not shut down: %s.\n", error_string(result));
        return 1;
    }
    printf("Could not shut down: poweroff did not complete.\n");
    return 1;
}
