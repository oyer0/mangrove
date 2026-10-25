/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/path.h"

#define COPY_BUFFER_SIZE 4096U

static int copy_file(const char *source_path, const char *destination_path)
{
    mg_path_info_t source_info;
    mg_path_info_t destination_info;
    mg_handle_t source = 0;
    mg_handle_t destination = 0;
    mg_result_t result;
    u64 remaining;
    u8 buffer[COPY_BUFFER_SIZE];
    bool destination_created = false;
    bool failed = false;

    result = path_info(source_path, &source_info);
    if (result_is_error(result)) {
        printf("Could not copy \"%s\": source: %s.\n",
               source_path, error_string(result));
        return 1;
    }
    if (source_info.type != MG_PATH_TYPE_FILE) {
        printf("Could not copy \"%s\": source is not a regular file.\n",
               source_path);
        return 1;
    }

    result = path_info(destination_path, &destination_info);
    if (result == MG_OK) {
        if (destination_info.identifier == source_info.identifier) {
            printf("Could not copy \"%s\": source and destination are the same file.\n",
                   source_path);
        } else if (destination_info.type == MG_PATH_TYPE_DIRECTORY) {
            printf("Could not copy \"%s\": destination is a directory.\n",
                   destination_path);
        } else {
            printf("Could not copy \"%s\": destination already exists.\n",
                   destination_path);
        }
        return 1;
    }
    if (result != MG_ERR_NOT_FOUND && result_is_error(result)) {
        printf("Could not copy to \"%s\": invalid destination: %s.\n",
               destination_path, error_string(result));
        return 1;
    }

    result = file_open(source_path, MG_OPEN_READ);
    if (result_is_error(result)) {
        printf("Could not copy \"%s\": could not open source: %s.\n",
               source_path, error_string(result));
        return 1;
    }
    source = (mg_handle_t)result;

    result = file_create(destination_path);
    if (result == MG_ERR_ALREADY_EXISTS) {
        printf("Could not copy to \"%s\": destination already exists.\n",
               destination_path);
        handle_close(source);
        return 1;
    }
    if (result_is_error(result)) {
        printf("Could not copy to \"%s\": could not create destination: %s.\n",
               destination_path, error_string(result));
        handle_close(source);
        return 1;
    }
    destination_created = true;
    remaining = source_info.size;

    result = file_open(destination_path, MG_OPEN_WRITE);
    if (result_is_error(result)) {
        printf("Could not copy to \"%s\": could not open destination: %s.\n",
               destination_path, error_string(result));
        failed = true;
    } else {
        destination = (mg_handle_t)result;
    }

    while (!failed && remaining > 0) {
        usize request = remaining < sizeof(buffer) ? (usize)remaining : sizeof(buffer);
        result = object_read(source, buffer, request);
        if (result == MG_ERR_END_OF_FILE || result == 0) {
            printf("Could not copy \"%s\": read failed: I/O failure.\n",
                   source_path);
            failed = true;
            break;
        }
        if (result_is_error(result) || (u64)result > remaining ||
            (usize)result > sizeof(buffer)) {
            printf("Could not copy \"%s\": read failed: %s.\n",
                   source_path,
                   result_is_error(result) ? error_string(result) : "invalid byte count");
            failed = true;
            break;
        }

        result = object_write_all(destination, buffer, (usize)result);
        if (result_is_error(result)) {
            printf("Could not copy to \"%s\": write failed: %s.\n",
                   destination_path, error_string(result));
            failed = true;
        } else {
            remaining -= (u64)result;
        }
    }

    if (destination) handle_close(destination);
    handle_close(source);
    if (failed && destination_created) path_remove(destination_path);
    return failed ? 1 : 0;
}

#define COPY_MAX_SOURCES 64U

static const char *source_basename(const char *path)
{
    const char *slash;

    if (!path || !*path) return NULL;
    slash = strrchr(path, '/');
    return slash ? slash + 1U : path;
}

int main(int argc, char **argv)
{
    usize source_count;
    const char *destination = NULL;
    mg_path_info_t destination_info;
    mg_result_t destination_result;
    bool destination_is_directory;
    bool failed = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc < 3 || (usize)(argc - 2) > COPY_MAX_SOURCES) {
        command_usage_error(argv[0], "copy <source>... <destination>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }

    source_count = (usize)argc - 2U;
    for (usize index = 1U; index < (usize)argc; index++) {
        if (argv[index][0] == '-') {
            command_usage_error(argv[0], "copy <source>... <destination>",
                                argv[index]);
            return 1;
        }
    }
    destination = argv[argc - 1];

    destination_result = path_info(destination, &destination_info);
    destination_is_directory = destination_result == MG_OK &&
        destination_info.type == MG_PATH_TYPE_DIRECTORY;
    if (source_count > 1U && !destination_is_directory) {
        if (destination_result == MG_OK) {
            printf("Could not copy to \"%s\": multiple sources require an existing destination directory.\n",
                   argv[argc - 1]);
        } else if (destination_result == MG_ERR_NOT_FOUND) {
            printf("Could not copy to \"%s\": destination directory does not exist.\n",
                   argv[argc - 1]);
        } else {
            printf("Could not copy to \"%s\": destination lookup failed: %s.\n",
                   argv[argc - 1], error_string(destination_result));
        }
        return 1;
    }
    if (destination_result != MG_OK &&
        destination_result != MG_ERR_NOT_FOUND) {
        printf("Could not copy to \"%s\": destination lookup failed: %s.\n",
               argv[argc - 1], error_string(destination_result));
        return 1;
    }

    for (usize index = 0; index < source_count; index++) {
        COMMAND_PATH_AUTO char *target = NULL;
        const char *source_argument = argv[index + 1U];
        const char *target_argument = argv[argc - 1];

        if (destination_is_directory) {
            const char *basename = source_basename(source_argument);
            if (!basename || !command_path_join_alloc(
                    destination, basename, &target)) {
                printf("Could not copy \"%s\": could not construct destination path.\n",
                       source_argument);
                failed = true;
                continue;
            }
            target_argument = target;
        }

        if (copy_file(source_argument, target_argument) != 0) failed = true;
    }
    return failed ? 1 : 0;
}
