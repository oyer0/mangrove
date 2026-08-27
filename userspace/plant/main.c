#include <mangrove.h>
#include <stdio.h>
#include "../common/path.h"

int main(int argc, char **argv)
{
    char path[256];
    mg_result_t result;

    if (argc != 2) {
        printf("Usage: plant <name>\n");
        return 1;
    }
    if (!command_resolve_path(argv[1], path, sizeof(path))) {
        printf("Could not create \"%s\": invalid path.\n", argv[1]);
        return 1;
    }
    result = file_create(path);
    if (result == MG_ERR_ALREADY_EXISTS) {
        printf("Could not create \"%s\": already exists.\n", argv[1]);
        return 1;
    }
    if (result_is_error(result)) {
        printf("Could not create \"%s\": %s.\n", argv[1], error_string(result));
        return 1;
    }
    return 0;
}
