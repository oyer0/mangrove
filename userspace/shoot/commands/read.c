#include <stdio.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include "commands.h"

static bool build_canonical_path(const char *cwd, const char *input,
                                 char *output, usize output_size)
{
    char raw[512];
    char *components[32];
    usize top = 0;
    usize length;
    const char *base;
    char *cursor;

    if (!cwd || !input || !output || output_size < 2 ||
        strlen(input) >= 256) return false;

    raw[0] = '\0';
    if (input[0] != '/') {
        base = cwd[0] ? cwd : "/";
        strncpy(raw, base, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        length = strlen(raw);
        if (length > 0 && raw[length - 1] != '/') {
            if (length + 1 >= sizeof(raw)) return false;
            raw[length++] = '/';
            raw[length] = '\0';
        }
        if (length + strlen(input) >= sizeof(raw)) return false;
        strcpy(raw + length, input);
    } else {
        if (strlen(input) >= sizeof(raw)) return false;
        strcpy(raw, input);
    }

    cursor = raw;
    while (*cursor) {
        char *component;
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        component = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (*cursor) *cursor++ = '\0';
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (top > 0) top--;
            continue;
        }
        if (top == sizeof(components) / sizeof(components[0])) return false;
        components[top++] = component;
    }

    output[0] = '/';
    output[1] = '\0';
    length = 1;
    for (usize i = 0; i < top; i++) {
        usize component_length = strlen(components[i]);
        if (length > 1) {
            if (length + 1 >= output_size) return false;
            output[length++] = '/';
        }
        if (length + component_length >= output_size) return false;
        memcpy(output + length, components[i], component_length);
        length += component_length;
        output[length] = '\0';
    }
    return true;
}

bool execute_read(shell_state_t *state, const shell_command_t *command)
{
    const char *target = command->arguments[0];
    char resolved[256];
    mg_result_t result;
    mg_handle_t file;
    char buffer[512];

    if (target[0] != '/') {
        if (!build_canonical_path(state->cwd, target, resolved, sizeof(resolved))) {
            printf("Could not read \"%s\": invalid path.\n", target);
            return true;
        }
        target = resolved;
    }

    result = file_open(target, MG_OPEN_READ);
    if (result_is_error(result)) {
        printf("Could not read \"%s\": %s.\n", command->arguments[0], error_string(result));
        return true;
    }
    file = (mg_handle_t)result;

    for (;;) {
        result = object_read(file, buffer, sizeof(buffer) - 1);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result_is_error(result)) {
            printf("\nRead error: %s.\n", error_string(result));
            break;
        }
        buffer[result] = '\0';
        printf("%s", buffer);
    }

    handle_close(file);
    return true;
}
