/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdlib.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/error.h>
#include "path.h"

void command_path_release(char **path)
{
    if (path && *path) {
        free(*path);
        *path = NULL;
    }
}

bool command_path_copy_alloc(const char *input, char **out_path)
{
    usize length;
    char *copy;
    if (!out_path) return false;
    *out_path = NULL;
    if (!input || !(length = strlen(input)) || length > MG_PATH_MAX_BYTES)
        return false;
    copy = (char *)malloc(length + 1U);
    if (!copy) return false;
    memcpy(copy, input, length + 1U);
    *out_path = copy;
    return true;
}

bool command_path_join_alloc(const char *parent, const char *name,
                             char **out_path)
{
    usize parent_length, name_length, separator, total;
    char *joined;
    if (!out_path) return false;
    *out_path = NULL;
    if (!parent || !name || !*name) return false;
    parent_length = strlen(parent);
    name_length = strlen(name);
    while (parent_length > 1U && parent[parent_length - 1U] == '/')
        parent_length--;
    separator = parent_length == 1U && parent[0] == '/' ? 0U : 1U;
    if (parent_length > MG_PATH_MAX_BYTES ||
        name_length > MG_PATH_MAX_BYTES - parent_length ||
        separator > MG_PATH_MAX_BYTES - parent_length - name_length)
        return false;
    total = parent_length + separator + name_length;
    if (!total || total > MG_PATH_MAX_BYTES) return false;
    joined = (char *)malloc(total + 1U);
    if (!joined) return false;
    memcpy(joined, parent, parent_length);
    usize offset = parent_length;
    if (separator) joined[offset++] = '/';
    memcpy(joined + offset, name, name_length);
    joined[total] = '\0';
    *out_path = joined;
    return true;
}

mg_result_t command_getcwd_alloc(char **out_path)
{
    char probe[1];
    usize required = 0;
    if (!out_path) return MG_ERR_BAD_ARGUMENT;
    *out_path = NULL;
    mg_result_t result = process_getcwd(probe, sizeof(probe), &required);
    if (result != MG_ERR_BUFFER_TOO_SMALL) return result;
    if (!required || required > MG_PATH_MAX_BYTES + 1U)
        return MG_ERR_NAME_TOO_LONG;
    char *path = (char *)malloc(required);
    if (!path) return MG_ERR_NO_MEMORY;
    result = process_getcwd(path, required, &required);
    if (result_is_error(result)) {
        free(path);
        return result;
    }
    *out_path = path;
    return MG_OK;
}

bool command_expand_home_path_alloc(const char *input, bool allow_home,
                                    char **out_path)
{
    mg_identity_t identity;
    usize input_length, home_length, suffix_length, result_length;
    char *path;

    if (!out_path) return false;
    *out_path = NULL;
    if (!input || !(input_length = strlen(input)) ||
        input_length > MG_PATH_MAX_BYTES) return false;
    if (!allow_home || input[0] != '~' ||
        (input[1] != '\0' && input[1] != '/'))
        return command_path_copy_alloc(input, out_path);
    if (result_is_error(process_get_identity(&identity)) ||
        identity.uid == MG_UID_SYSTEM || identity.home[0] != '/') return false;
    home_length = strlen(identity.home);
    if (!home_length || strcmp(identity.home, "/") == 0) return false;
    suffix_length = input_length - 1U;
    if (home_length > MG_PATH_MAX_BYTES ||
        suffix_length > MG_PATH_MAX_BYTES - home_length) return false;
    result_length = home_length + suffix_length;
    path = (char *)malloc(result_length + 1U);
    if (!path) return false;
    memcpy(path, identity.home, home_length);
    memcpy(path + home_length, input + 1U, suffix_length + 1U);
    *out_path = path;
    return true;
}

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

    if (!name || !path || !capacity) return false;
    name_length = strlen(name);
    if (name_length == 0) return false;
    if (strchr(name, '/')) {
        if (name_length >= capacity) return false;
        memcpy(path, name, name_length + 1U);
        return true;
    }
    if (capacity < 6U || name_length > capacity - 6U) return false;
    strcpy(path, "/bin/");
    strcpy(path + 5, name);
    return true;
}

bool command_build_executable_path_alloc(const char *name, char **out_path)
{
    if (!out_path) return false;
    *out_path = NULL;
    if (!name || !*name) return false;
    if (strchr(name, '/')) return command_path_copy_alloc(name, out_path);
    return command_path_join_alloc("/bin", name, out_path);
}

/* Keep native relative authority and actual component traversal. In
 * particular, never convert getcwd into a replacement-sensitive lookup, or
 * erase a nonexistent/forbidden component by lexically collapsing '..'. */
bool command_resolve_path(const char *input, char *output, usize capacity)
{
    if (!input || !*input || !output || !capacity) return false;
    usize length = strlen(input);
    if (length > MG_PATH_MAX_BYTES || length >= capacity) return false;
    memmove(output, input, length + 1U);
    return true;
}
