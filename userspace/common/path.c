#include <mangrove.h>
#include <string.h>
#include "path.h"

bool command_resolve_path(const char *input, char *output, usize capacity)
{
    char cwd[256];
    char raw[512];
    char *components[32];
    usize top = 0;
    usize length;
    const char *base;
    char *cursor;
    usize cwd_size = 0;

    if (!input || !output || capacity < 2 || strlen(input) >= 256) return false;

    if (input[0] == '/') {
        if (strlen(input) >= sizeof(raw)) return false;
        strcpy(raw, input);
    } else {
        if (result_is_error(process_getcwd(cwd, sizeof(cwd), &cwd_size))) return false;
        base = cwd[0] ? cwd : "/";
        length = strlen(base);
        if (length + 1 + strlen(input) >= sizeof(raw)) return false;
        memcpy(raw, base, length);
        if (length == 0 || raw[length - 1] != '/') raw[length++] = '/';
        strcpy(raw + length, input);
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
            if (length + 1 >= capacity) return false;
            output[length++] = '/';
        }
        if (length + component_length >= capacity) return false;
        memcpy(output + length, components[i], component_length);
        length += component_length;
        output[length] = '\0';
    }
    return true;
}
