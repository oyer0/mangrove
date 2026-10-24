/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <mg/identity.h>
#include <mg/filesystem.h>
#include "../../userspace/common/app_directory.h"

static mg_identity_t identity;
static struct { char path[256]; bool exists; } dirs[8];
static usize dir_count;
static usize create_count;

mg_result_t process_get_identity(mg_identity_t *out)
{
    if (!out) return MG_ERR_BAD_ARGUMENT;
    *out = identity;
    return MG_OK;
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    if (!strcmp(path, identity.home)) {
        *info = (mg_path_info_t){.type=MG_PATH_TYPE_DIRECTORY,
                                .owner_uid=identity.uid};
        return MG_OK;
    }
    for (usize i = 0; i < dir_count; i++)
        if (dirs[i].exists && !strcmp(dirs[i].path, path)) {
            *info = (mg_path_info_t){.type=MG_PATH_TYPE_DIRECTORY,
                                    .owner_uid=identity.uid};
            return MG_OK;
        }
    return MG_ERR_NOT_FOUND;
}

mg_result_t directory_create(const char *path)
{
    assert(dir_count < sizeof(dirs) / sizeof(dirs[0]));
    (void)strcpy(dirs[dir_count].path, path);
    dirs[dir_count++].exists = true;
    create_count++;
    return MG_OK;
}

static void setup(void)
{
    memset(&identity, 0, sizeof(identity));
    identity.uid = 42U;
    strcpy(identity.username, "tester");
    strcpy(identity.home, "/home/tester");
    memset(dirs, 0, sizeof(dirs));
    dir_count = create_count = 0;
}

int main(void)
{
    char path[MG_PATH_MAX_BYTES + 1U];

    setup();
    assert(app_directory_path(MG_APP_DIRECTORY_CONFIG, "reed", path,
                              sizeof(path)) == MG_OK);
    assert(!strcmp(path, "/home/tester/.config/reed"));
    assert(create_count == 0U);
    assert(app_directory_path(MG_APP_DIRECTORY_DATA, "shoot", path,
                              sizeof(path)) == MG_OK);
    assert(!strcmp(path, "/home/tester/.data/shoot"));
    assert(app_directory_path(MG_APP_DIRECTORY_STATE, "shoot", path,
                              sizeof(path)) == MG_OK);
    assert(!strcmp(path, "/home/tester/.state/shoot"));
    assert(app_directory_path(MG_APP_DIRECTORY_CACHE, "shoot", path,
                              sizeof(path)) == MG_OK);
    assert(!strcmp(path, "/home/tester/.cache/shoot"));
    assert(app_directory_path(MG_APP_DIRECTORY_CONFIG, "../reed", path,
                              sizeof(path)) == MG_ERR_BAD_ARGUMENT);
    assert(app_directory_path((mg_app_directory_kind_t)9, "reed", path,
                              sizeof(path)) == MG_ERR_BAD_ARGUMENT);
    assert(app_directory_path(MG_APP_DIRECTORY_CONFIG, "reed", path, 8U) ==
           MG_ERR_BAD_ARGUMENT);
    assert(app_directory_ensure(MG_APP_DIRECTORY_STATE, "shoot", path,
                                sizeof(path)) == MG_OK);
    assert(create_count == 2U);
    assert(app_directory_ensure(MG_APP_DIRECTORY_STATE, "shoot", path,
                                sizeof(path)) == MG_OK);
    assert(create_count == 2U);
    identity.uid = MG_UID_SYSTEM;
    assert(app_directory_path(MG_APP_DIRECTORY_STATE, "shoot", path,
                              sizeof(path)) == MG_ERR_ACCESS_DENIED);
    setup();
    strcpy(identity.home, "/home/other");
    assert(app_directory_path(MG_APP_DIRECTORY_CONFIG, "reed", path,
                              sizeof(path)) == MG_ERR_BAD_ARGUMENT);
    return 0;
}
