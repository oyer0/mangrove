#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/path.h"

int main(int argc, char **argv)
{
    char path[256];
    mg_handle_t directory;
    mg_directory_entry_t entries[MG_DIRECTORY_BATCH_MAX];
    usize entry_count;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        command_usage_error(argv[0], "ls [path]", argc == 2 ? argv[1] : NULL);
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
        result = directory_read_batch(directory, entries,
                                      MG_DIRECTORY_BATCH_MAX, &entry_count);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result_is_error(result)) {
            printf("Could not read directory: %s.\n", error_string(result));
            (void)handle_close(directory);
            return 1;
        }
        for (usize i = 0; i < entry_count; i++) {
            if (entries[i].type == MG_PATH_TYPE_DIRECTORY)
                printf("%s/\n", entries[i].name);
            else
                printf("%s\n", entries[i].name);
        }
    }
    (void)handle_close(directory);
    return 0;
}
