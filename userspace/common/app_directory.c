/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_directory.h"

#include <mangrove.h>
#include <mg/identity.h>
#include <string.h>
#include <stdlib.h>

static const char *const roots[] = {
    ".config", ".data", ".state", ".cache",
};

static bool name_valid(const char *name)
{
    if (!name || name[0] < 'a' || name[0] > 'z') return false;
    for (const char *cursor = name + 1; *cursor; cursor++)
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') ||
              *cursor == '_' || *cursor == '-')) return false;
    return strlen(name) < MG_IDENTITY_USERNAME_CAPACITY;
}

static mg_result_t current_home(mg_identity_t *identity)
{
    mg_path_info_t info;
    usize username_length;
    usize home_length;
    mg_result_t result;

    if (!identity) return MG_ERR_BAD_ARGUMENT;
    result = process_get_identity(identity);
    if (result < 0) return result;
    if (identity->uid == MG_UID_SYSTEM || !name_valid(identity->username))
        return MG_ERR_ACCESS_DENIED;
    username_length = strlen(identity->username);
    home_length = strlen(identity->home);
    if (home_length != 6U + username_length ||
        memcmp(identity->home, "/home/", 6U) != 0 ||
        memcmp(identity->home + 6U, identity->username,
               username_length) != 0 ||
        identity->home[home_length] != '\0') return MG_ERR_BAD_ARGUMENT;
    result = path_info(identity->home, &info);
    if (result < 0) return result;
    if (info.type != MG_PATH_TYPE_DIRECTORY || info.owner_uid != identity->uid)
        return MG_ERR_ACCESS_DENIED;
    return MG_OK;
}

static mg_result_t build_path(mg_app_directory_kind_t kind,
                              const char *application, char *path,
                              usize capacity, mg_identity_t *identity)
{
    const char *root;
    usize home_length;
    usize root_length;
    usize app_length;
    usize needed;
    mg_result_t result;

    if ((u32)kind >= sizeof(roots) / sizeof(roots[0]) ||
        !name_valid(application) || !path || !capacity)
        return MG_ERR_BAD_ARGUMENT;
    result = current_home(identity);
    if (result < 0) return result;
    root = roots[kind];
    home_length = strlen(identity->home);
    root_length = strlen(root);
    app_length = strlen(application);
    needed = home_length + 1U + root_length + 1U + app_length;
    if (needed > MG_PATH_MAX_BYTES || needed >= capacity)
        return MG_ERR_BAD_ARGUMENT;
    memcpy(path, identity->home, home_length);
    path[home_length] = '/';
    memcpy(path + home_length + 1U, root, root_length);
    path[home_length + root_length + 1U] = '/';
    memcpy(path + home_length + root_length + 2U, application,
           app_length + 1U);
    return MG_OK;
}

mg_result_t app_directory_path(mg_app_directory_kind_t kind,
                               const char *application, char *path,
                               usize capacity)
{
    mg_identity_t identity;
    return build_path(kind, application, path, capacity, &identity);
}

static mg_result_t ensure_directory(const char *path, mg_uid_t owner)
{
    mg_path_info_t info;
    mg_result_t result = path_info(path, &info);

    if (result == MG_ERR_NOT_FOUND) {
        result = directory_create(path);
        if (result != MG_OK && result != MG_ERR_ALREADY_EXISTS) return result;
        result = path_info(path, &info);
    }
    if (result < 0) return result;
    if (info.type != MG_PATH_TYPE_DIRECTORY || info.owner_uid != owner)
        return MG_ERR_ACCESS_DENIED;
    return MG_OK;
}

mg_result_t app_directory_ensure(mg_app_directory_kind_t kind,
                                 const char *application, char *path,
                                 usize capacity)
{
    mg_identity_t identity;
    char *root = NULL;
    usize root_length;
    mg_result_t result = build_path(kind, application, path, capacity,
                                    &identity);

    if (result < 0) return result;
    root_length = strlen(identity.home) + 1U + strlen(roots[kind]);
    if (root_length > MG_PATH_MAX_BYTES) return MG_ERR_NAME_TOO_LONG;
    root = (char *)malloc(root_length + 1U);
    if (!root) return MG_ERR_NO_MEMORY;
    memcpy(root, identity.home, strlen(identity.home));
    root[strlen(identity.home)] = '/';
    strcpy(root + strlen(identity.home) + 1U, roots[kind]);
    result = ensure_directory(root, identity.uid);
    free(root);
    if (result < 0) return result;
    return ensure_directory(path, identity.uid);
}
