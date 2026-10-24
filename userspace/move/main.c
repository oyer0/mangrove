/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/path.h"

#define MOVE_MAX_SOURCES 64U

static const char *source_basename(const char *path)
{
    const char *slash;

    if (!path || !*path) return NULL;
    slash = strrchr(path, '/');
    return slash ? slash + 1U : path;
}

static bool destination_child_path(const char *directory, const char *name,
                                   char *output, usize capacity)
{
    usize directory_length;
    usize name_length;
    bool root;

    if (!directory || !name || !*name || !output || !capacity) return false;
    directory_length = strlen(directory);
    name_length = strlen(name);
    while (directory_length > 1U && directory[directory_length - 1U] == '/')
        directory_length--;
    root = directory_length == 1U && directory[0] == '/';
    if (root) {
        if (name_length + 2U > capacity) return false;
        output[0] = '/';
        memcpy(output + 1U, name, name_length + 1U);
        return true;
    }
    if (directory_length > (usize)-1 - name_length - 1U ||
        directory_length + name_length + 2U > capacity) return false;
    memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    memcpy(output + directory_length + 1U, name, name_length + 1U);
    return true;
}

int main(int argc, char **argv)
{
    usize source_count;
    char destination[MG_PATH_MAX_BYTES + 1U];
    mg_path_info_t destination_info;
    mg_result_t destination_result;
    bool destination_is_directory;
    bool failed = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc < 3 || (usize)(argc - 2) > MOVE_MAX_SOURCES) {
        command_usage_error(argv[0], "move <source>... <destination>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }

    source_count = (usize)argc - 2U;
    for (usize index = 1U; index < (usize)argc; index++) {
        if (argv[index][0] == '-') {
            command_usage_error(argv[0], "move <source>... <destination>",
                                argv[index]);
            return 1;
        }
    }
    if (!command_resolve_path(argv[argc - 1], destination,
                              sizeof(destination))) {
        printf("Could not move to \"%s\": invalid path.\n",
               argv[argc - 1]);
        return 1;
    }

    destination_result = path_info(destination, &destination_info);
    destination_is_directory = destination_result == MG_OK &&
        destination_info.type == MG_PATH_TYPE_DIRECTORY;
    if (source_count > 1U && !destination_is_directory) {
        if (destination_result == MG_OK) {
            printf("Could not move to \"%s\": multiple sources require an existing destination directory.\n",
                   argv[argc - 1]);
        } else if (destination_result == MG_ERR_NOT_FOUND) {
            printf("Could not move to \"%s\": destination directory does not exist.\n",
                   argv[argc - 1]);
        } else {
            printf("Could not move to \"%s\": destination lookup failed: %s.\n",
                   argv[argc - 1], error_string(destination_result));
        }
        return 1;
    }
    if (destination_result != MG_OK &&
        destination_result != MG_ERR_NOT_FOUND) {
        printf("Could not move to \"%s\": destination lookup failed: %s.\n",
               argv[argc - 1], error_string(destination_result));
        return 1;
    }

    for (usize index = 0; index < source_count; index++) {
        char source[MG_PATH_MAX_BYTES + 1U];
        char target[MG_PATH_MAX_BYTES + 1U];
        const char *source_argument = argv[index + 1U];
        const char *target_argument = argv[argc - 1];
        mg_result_t result;

        if (!command_resolve_path(source_argument, source, sizeof(source))) {
            printf("Could not move \"%s\": invalid source path.\n",
                   source_argument);
            failed = true;
            continue;
        }
        if (destination_is_directory) {
            const char *basename = source_basename(source);
            if (!basename || !destination_child_path(
                    destination, basename, target, sizeof(target))) {
                printf("Could not move \"%s\": could not construct destination path.\n",
                       source_argument);
                failed = true;
                continue;
            }
            target_argument = target;
        }

        result = path_move(source, target_argument);
        if (result_is_error(result)) {
            printf("Could not move \"%s\" to \"%s\": %s.\n",
                   source_argument, target_argument, error_string(result));
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
