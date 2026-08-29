#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"

static bool is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0 || strcmp(name, "help") == 0 ||
           strcmp(name, "exit") == 0;
}

int main(int argc, char **argv)
{
    char path[256];
    mg_path_info_t info;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2) {
        command_usage_error(argv[0], "locate <name>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    if (is_builtin(argv[1])) {
        printf("%s (Shoot builtin)\n", argv[1]);
        return 0;
    }
    if (!strcmp(argv[1], "sprout") || !strcmp(argv[1], "shoot")) {
        printf("not found\n");
        return 1;
    }
    if (strlen(argv[1]) == 0 || strlen(argv[1]) + 6 >= sizeof(path)) {
        printf("not found\n");
        return 1;
    }
    strcpy(path, "/bin/");
    strcpy(path + 5, argv[1]);
    if (result_is_error(path_info(path, &info)) || info.type != MG_PATH_TYPE_FILE) {
        printf("not found\n");
        return 1;
    }
    printf("%s\n", path);
    return 0;
}
