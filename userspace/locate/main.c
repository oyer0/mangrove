#include <mangrove.h>
#include <stdio.h>
#include <string.h>

static bool is_builtin(const char *name)
{
    return strcmp(name, "jump") == 0 || strcmp(name, "help") == 0 ||
           strcmp(name, "exit") == 0;
}

int main(int argc, char **argv)
{
    char path[256];
    mg_path_info_t info;

    if (argc != 2) {
        printf("Usage: locate <name>\n");
        return 1;
    }
    if (is_builtin(argv[1])) {
        printf("%s (Shoot builtin)\n", argv[1]);
        return 0;
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
