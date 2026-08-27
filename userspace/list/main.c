#include <mangrove.h>
#include <stdio.h>
#include "../common/path.h"

int main(int argc, char **argv)
{
    char path[256];
    mg_handle_t directory;
    mg_directory_entry_t entry;
    mg_result_t result;

    if (argc > 2) {
        printf("Usage: list [path]\n");
        return 1;
    }
    if (!command_resolve_path(argc == 1 ? "." : argv[1], path, sizeof(path))) {
        printf("Could not list \"%s\": invalid path.\n", argc == 1 ? "." : argv[1]);
        return 1;
    }
    result = directory_open(path);
    if (result_is_error(result)) {
        printf("Could not list \"%s\": %s.\n", argc == 1 ? "." : argv[1],
               error_string(result));
        return 1;
    }
    directory = (mg_handle_t)result;
    for (;;) {
        result = directory_read(directory, &entry);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result_is_error(result)) {
            printf("Could not read directory: %s.\n", error_string(result));
            (void)handle_close(directory);
            return 1;
        }
        if (entry.type == MG_PATH_TYPE_DIRECTORY) printf("%s/\n", entry.name);
        else printf("%s\n", entry.name);
    }
    (void)handle_close(directory);
    return 0;
}
