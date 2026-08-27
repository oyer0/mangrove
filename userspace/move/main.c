#include <mangrove.h>
#include <stdio.h>
#include "../common/path.h"

int main(int argc, char **argv)
{
    char source[256];
    char destination[256];
    mg_result_t result;

    if (argc != 3) {
        printf("Usage: move <source> <destination>\n");
        return 1;
    }
    if (!command_resolve_path(argv[1], source, sizeof(source))) {
        printf("Could not move \"%s\": invalid path.\n", argv[1]);
        return 1;
    }
    if (!command_resolve_path(argv[2], destination, sizeof(destination))) {
        printf("Could not move to \"%s\": invalid path.\n", argv[2]);
        return 1;
    }
    result = path_move(source, destination);
    if (result_is_error(result)) {
        printf("Could not move \"%s\" to \"%s\": %s.\n",
               argv[1], argv[2], error_string(result));
        return 1;
    }
    return 0;
}
