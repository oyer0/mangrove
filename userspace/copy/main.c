#include <mangrove.h>
#include <stdio.h>

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

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: copy <source> <destination>\n");
        return 1;
    }
    return copy_file(argv[1], argv[2]);
}
