#include <mangrove.h>
#include <stdio.h>
#include "../common/path.h"

int main(int argc, char **argv)
{
    char path[256];
    char buffer[512];
    mg_handle_t file;
    mg_result_t result;

    if (argc != 2) {
        printf("Usage: read <file>\n");
        return 1;
    }
    if (!command_resolve_path(argv[1], path, sizeof(path))) {
        printf("Could not read \"%s\": invalid path.\n", argv[1]);
        return 1;
    }
    result = file_open(path, MG_OPEN_READ);
    if (result_is_error(result)) {
        printf("Could not read \"%s\": %s.\n", argv[1], error_string(result));
        return 1;
    }
    file = (mg_handle_t)result;
    for (;;) {
        result = object_read(file, buffer, sizeof(buffer) - 1);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result_is_error(result)) {
            printf("\nRead error: %s.\n", error_string(result));
            (void)handle_close(file);
            return 1;
        }
        buffer[result] = '\0';
        printf("%s", buffer);
    }
    (void)handle_close(file);
    return 0;
}
