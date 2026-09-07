#include <mangrove.h>
#include <string.h>
#include "path.h"

bool command_expand_home_path(const char *input, bool allow_home,
                              char *output, usize capacity)
{
    mg_identity_t identity;
    usize input_length;
    usize home_length;
    usize suffix_length;

    if (!input || !output || capacity == 0) return false;
    input_length = strlen(input);
    if (input_length >= capacity) return false;

    if (!allow_home || input[0] != '~' ||
        (input[1] != '\0' && input[1] != '/')) {
        memcpy(output, input, input_length + 1U);
        return true;
    }

    if (result_is_error(process_get_identity(&identity)) ||
        identity.uid == MG_UID_SYSTEM || identity.home[0] != '/') return false;
    home_length = strlen(identity.home);
    if (home_length == 0 || strcmp(identity.home, "/") == 0 ||
        home_length >= capacity) return false;
    suffix_length = input_length - 1U;
    if (home_length + suffix_length >= capacity) return false;
    memcpy(output, identity.home, home_length);
    memcpy(output + home_length, input + 1, suffix_length + 1U);
    return true;
}

bool command_build_executable_path(const char *name, char *path,
                                   usize capacity)
{
    usize name_length;

    if (!name || !path || capacity < 6U) return false;
    name_length = strlen(name);
    if (name_length == 0) return false;
    if (name[0] == '/') {
        if (name_length + 1U > capacity) return false;
        memcpy(path, name, name_length + 1U);
        return true;
    }
    if (name_length + 6U > capacity) return false;
    strcpy(path, "/bin/");
    strcpy(path + 5, name);
    return true;
}

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
