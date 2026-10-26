/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#define main copy_command_main
#include "../../userspace/copy/main.c"
#undef main

static char created[16][256];
static usize created_count;
static char attempted[32][256];
static usize attempted_count;
static usize opened_sources;
static usize closed_handles;
static bool help_requested;
static bool usage_requested;
static const char *failed_create_path;

bool command_help_requested(int argc, char **argv)
{
    return argc == 2 && argv && argv[1] &&
           (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));
}

int command_print_help(const char *argv0)
{
    (void)argv0;
    help_requested = true;
    return 0;
}

void command_usage_error(const char *argv0, const char *usage,
                         const char *option)
{
    (void)argv0;
    assert(!strcmp(usage, "copy <source>... <destination>"));
    (void)option;
    usage_requested = true;
}

bool command_resolve_path(const char *input, char *output, usize capacity)
{
    usize length;

    if (!input || !*input || !output) return false;
    length = strlen(input);
    if (length >= capacity) return false;
    memcpy(output, input, length + 1U);
    return true;
}

bool command_path_join_alloc(const char *parent, const char *name,
                             char **out_path)
{
    usize length = strlen(parent);
    while (length > 1U && parent[length - 1U] == '/') length--;
    usize name_length = strlen(name);
    *out_path = malloc(length + name_length + 2U);
    if (!*out_path) return false;
    memcpy(*out_path, parent, length);
    (*out_path)[length] = '/';
    memcpy(*out_path + length + 1U, name, name_length + 1U);
    return true;
}

void command_path_release(char **path)
{
    if (path && *path) {
        free(*path);
        *path = NULL;
    }
}

bool result_is_error(mg_result_t result) { return result < 0; }
const char *error_string(mg_result_t result)
{
    if (result == MG_ERR_IO) return "injected failure";
    if (result == MG_ERR_NOT_FOUND) return "not found";
    return "unexpected error";
}

static bool is_created(const char *path)
{
    for (usize index = 0; index < created_count; index++)
        if (!strcmp(created[index], path)) return true;
    return false;
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!strcmp(path, "dest") || !strcmp(path, "dest/"))
        info->type = MG_PATH_TYPE_DIRECTORY;
    else if (!strcmp(path, "directory-source"))
        info->type = MG_PATH_TYPE_DIRECTORY;
    else if (is_created(path)) {
        info->type = MG_PATH_TYPE_FILE;
        info->identifier = 100U + (u64)(path[0] ? path[0] : 0);
    } else if (!strcmp(path, "missing") || !strcmp(path, "out") ||
               !strncmp(path, "dest/", 5U)) {
        return MG_ERR_NOT_FOUND;
    } else if (!strcmp(path, "dest-file")) {
        info->type = MG_PATH_TYPE_FILE;
        info->identifier = 90U;
    } else {
        info->type = MG_PATH_TYPE_FILE;
        info->size = 0U;
        info->identifier = !strcmp(path, "src1") ? 1U :
                           !strcmp(path, "src2") ? 2U :
                           !strcmp(path, "src3") ? 3U :
                           !strcmp(path, "other/src1") ? 4U : 5U;
    }
    return MG_OK;
}

mg_result_t file_open(const char *path, u32 flags)
{
    if (flags == MG_OPEN_READ) {
        opened_sources++;
        if (!strcmp(path, "bad-source")) return MG_ERR_IO;
        return 1;
    }
    assert(flags == MG_OPEN_WRITE);
    for (usize index = 0; index < created_count; index++)
        if (!strcmp(path, created[index])) return 2;
    return MG_ERR_NOT_FOUND;
}

mg_result_t file_create(const char *path)
{
    assert(attempted_count < sizeof(attempted) / sizeof(attempted[0]));
    strcpy(attempted[attempted_count++], path);
    if (failed_create_path && !strcmp(path, failed_create_path)) return MG_ERR_IO;
    if (is_created(path)) return MG_ERR_ALREADY_EXISTS;
    assert(created_count < sizeof(created) / sizeof(created[0]));
    strcpy(created[created_count++], path);
    return MG_OK;
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    (void)handle; (void)buffer; (void)length;
    return MG_ERR_IO;
}

mg_result_t object_write_all(mg_handle_t handle, const void *buffer,
                             usize length)
{
    (void)handle; (void)buffer; (void)length;
    return MG_OK;
}

mg_result_t handle_close(mg_handle_t handle)
{
    assert(handle == 1 || handle == 2);
    closed_handles++;
    return MG_OK;
}

mg_result_t path_remove(const char *path)
{
    for (usize index = 0; index < created_count; index++) {
        if (!strcmp(path, created[index])) {
            created[index][0] = '\0';
            return MG_OK;
        }
    }
    return MG_ERR_NOT_FOUND;
}

static void reset(void)
{
    memset(created, 0, sizeof(created));
    memset(attempted, 0, sizeof(attempted));
    created_count = attempted_count = opened_sources = closed_handles = 0U;
    help_requested = usage_requested = false;
    failed_create_path = NULL;
}

static int run(int argc, char **argv)
{
    return copy_command_main(argc, argv);
}

int main(void)
{
    char *single[] = {"copy", "src1", "out"};
    char *single_into_dir[] = {"copy", "src1", "dest/"};
    char *multiple[] = {"copy", "src1", "src2", "dest/"};
    char *missing_destination[] = {"copy", "src1", "src2", "missing"};
    char *file_destination[] = {"copy", "src1", "src2", "dest-file"};
    char *middle_failure[] = {"copy", "src1", "src2", "src3", "dest"};
    char *invalid_source[] = {"copy", "bad-source", "src1", "dest"};
    char *directory_source[] = {"copy", "directory-source", "src1", "dest/"};
    char *collision[] = {"copy", "src1", "other/src1", "dest"};
    char *no_operands[] = {"copy"};
    char *help[] = {"copy", "--help"};
    char *leading_dash[] = {"copy", "--", "-source", "dest"};

    reset();
    assert(run(3, single) == 0);
    assert(created_count == 1U && !strcmp(created[0], "out"));

    reset();
    assert(run(3, single_into_dir) == 0);
    assert(created_count == 1U && !strcmp(created[0], "dest/src1"));

    reset();
    assert(run(4, multiple) == 0);
    assert(created_count == 2U && !strcmp(created[0], "dest/src1") &&
           !strcmp(created[1], "dest/src2"));

    reset();
    assert(run(4, missing_destination) == 1 && attempted_count == 0U);

    reset();
    assert(run(4, file_destination) == 1 && attempted_count == 0U);

    reset();
    failed_create_path = "dest/src2";
    assert(run(5, middle_failure) == 1);
    assert(attempted_count == 3U);
    assert(!strcmp(attempted[0], "dest/src1") &&
           !strcmp(attempted[1], "dest/src2") &&
           !strcmp(attempted[2], "dest/src3"));
    assert(created_count == 2U && !strcmp(created[1], "dest/src3"));

    reset();
    assert(run(4, invalid_source) == 1);
    assert(attempted_count == 1U && !strcmp(attempted[0], "dest/src1"));

    reset();
    assert(run(4, directory_source) == 1);
    assert(created_count == 1U && !strcmp(created[0], "dest/src1"));

    reset();
    assert(run(4, collision) == 1);
    assert(attempted_count == 1U && created_count == 1U);

    reset();
    assert(run(4, leading_dash) == 0 && created_count == 1U);
    assert(!strcmp(created[0], "dest/-source"));

    reset();
    assert(run(1, no_operands) == 1 && usage_requested);
    assert(run(2, help) == 0 && help_requested);
    return 0;
}
