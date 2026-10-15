/* SPDX-License-Identifier: GPL-3.0-or-later */
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

/* Keep native relative authority and actual component traversal. In
 * particular, never convert getcwd into a replacement-sensitive lookup, or
 * erase a nonexistent/forbidden component by lexically collapsing '..'. */
bool command_resolve_path(const char *input, char *output, usize capacity)
{
    if (!input || !*input || !output || !capacity) return false;
    usize length = strlen(input);
    if (length >= 256 || length >= capacity) return false;
    memmove(output, input, length + 1U);
    return true;
}
