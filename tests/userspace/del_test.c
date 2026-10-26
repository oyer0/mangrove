/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <mg/filesystem.h>
#define main del_command_main
#include "../../userspace/del/main.c"
#undef main

static usize remove_calls;
static char removed_path[256];
static bool help_requested;
static bool usage_requested;
static struct {
    char path[256];
    usize cursor;
} open_directories[4];
static usize open_directory_count;
static usize active_directories;
static char removed_paths[32][256];
static usize removed_path_count;
static const char *failed_remove_path;
static usize root_remove_count;
static usize sub_remove_count;
static char successful_paths[32][256];
static usize successful_path_count;

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
    assert(!strcmp(usage, "del [-r|--recursive] <file|directory>..."));
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
    usize parent_length = strlen(parent);
    usize name_length = strlen(name);
    *out_path = malloc(parent_length + name_length + 2U);
    if (!*out_path) return false;
    memcpy(*out_path, parent, parent_length);
    (*out_path)[parent_length] = '/';
    memcpy(*out_path + parent_length + 1U, name, name_length + 1U);
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
    if (result == MG_ERR_NOT_FOUND) return "not found";
    if (result == MG_ERR_ACCESS_DENIED) return "access denied";
    return "injected error";
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    for (usize index = 0; index < successful_path_count; index++)
        if (!strcmp(path, successful_paths[index])) return MG_ERR_NOT_FOUND;
    memset(info, 0, sizeof(*info));
    if (!strcmp(path, "missing")) return MG_ERR_NOT_FOUND;
    if (!strcmp(path, "empty") || !strcmp(path, "nonempty") ||
        !strcmp(path, "tree") || !strcmp(path, "tree/sub") ||
        !strcmp(path, "tree/empty") || !strcmp(path, "tree/mount") ||
        !strcmp(path, "mountroot"))
        info->type = MG_PATH_TYPE_DIRECTORY;
    else if (!strcmp(path, "special"))
        info->type = (mg_path_type_t)99;
    else
        info->type = MG_PATH_TYPE_FILE;
    if (!strcmp(path, "tree")) info->identifier = 1U;
    else if (!strcmp(path, "tree/sub")) info->identifier = 2U;
    else if (!strcmp(path, "tree/empty")) info->identifier = 3U;
    else if (!strcmp(path, "tree/mount") || !strcmp(path, "mountroot"))
        info->identifier = 4U;
    else if (!strcmp(path, "tree/first")) info->identifier = 5U;
    else if (!strcmp(path, "tree/sub/second")) info->identifier = 6U;
    else info->identifier = 7U;
    return MG_OK;
}

mg_result_t path_remove(const char *path)
{
    assert(removed_path_count < sizeof(removed_paths) / sizeof(removed_paths[0]));
    if (!strncmp(path, "tree", 4U)) assert(active_directories == 0U);
    strcpy(removed_paths[removed_path_count++], path);
    remove_calls++;
    strcpy(removed_path, path);
    if (failed_remove_path && !strcmp(path, failed_remove_path))
        return MG_ERR_IO;
    mg_result_t result = MG_OK;
    if (!strcmp(path, "nonempty")) result = MG_ERR_NOT_EMPTY;
    else if (!strcmp(path, "tree"))
        result = ++root_remove_count == 1U ? MG_ERR_NOT_EMPTY : MG_OK;
    else if (!strcmp(path, "tree/sub"))
        result = ++sub_remove_count == 1U ? MG_ERR_NOT_EMPTY : MG_OK;
    else if (!strcmp(path, "mountroot") || !strcmp(path, "tree/mount"))
        result = MG_ERR_BUSY;
    else if (!strcmp(path, "denied")) result = MG_ERR_ACCESS_DENIED;
    if (result == MG_OK) {
        assert(successful_path_count <
               sizeof(successful_paths) / sizeof(successful_paths[0]));
        strcpy(successful_paths[successful_path_count++], path);
    }
    return result;
}

mg_result_t directory_open(const char *path)
{
    assert(open_directory_count < 4U);
    usize index = open_directory_count++;
    strcpy(open_directories[index].path, path);
    open_directories[index].cursor = 0U;
    active_directories++;
    if (!strcmp(path, "tree")) return (mg_result_t)(index + 1U);
    if (!strcmp(path, "tree/sub")) return (mg_result_t)(index + 1U);
    return MG_ERR_NOT_FOUND;
}

mg_result_t directory_read_batch(mg_handle_t handle,
                                 mg_directory_entry_t *entries,
                                 usize capacity, usize *out_count)
{
    static const char *const root_names[] = {"first", "sub", "empty"};
    static const u32 root_types[] = {MG_PATH_TYPE_FILE,
                                     MG_PATH_TYPE_DIRECTORY,
                                     MG_PATH_TYPE_DIRECTORY};
    static const char *const sub_names[] = {"second"};
    static const u32 sub_types[] = {MG_PATH_TYPE_FILE};
    usize index = (usize)handle - 1U;
    assert(index < open_directory_count && capacity);
    const char *const *names;
    const u32 *types;
    usize count;
    if (!strcmp(open_directories[index].path, "tree")) {
        names = root_names; types = root_types; count = 3U;
    } else {
        names = sub_names; types = sub_types; count = 1U;
    }
    if (open_directories[index].cursor >= count) {
        *out_count = 0U;
        return MG_ERR_END_OF_FILE;
    }
    memset(entries, 0, sizeof(*entries));
    strcpy(entries[0].name, names[open_directories[index].cursor]);
    entries[0].type = types[open_directories[index].cursor];
    open_directories[index].cursor++;
    *out_count = 1U;
    return MG_OK;
}

mg_result_t directory_read_batch_ex(mg_handle_t handle,
                                    mg_directory_entry_ex_t *entries,
                                    usize capacity, usize *out_count)
{
    mg_directory_entry_t legacy[MG_DIRECTORY_BATCH_MAX];
    usize count = 0U;
    mg_result_t result;

    if (capacity > MG_DIRECTORY_BATCH_MAX) capacity = MG_DIRECTORY_BATCH_MAX;
    result = directory_read_batch(handle, legacy, capacity, &count);
    if (result != MG_OK) return result;
    for (usize index = 0; index < count; index++) {
        memset(&entries[index], 0, sizeof(entries[index]));
        memcpy(entries[index].name, legacy[index].name,
               sizeof(legacy[index].name));
        entries[index].type = legacy[index].type;
        entries[index].identifier = legacy[index].identifier;
    }
    *out_count = count;
    return MG_OK;
}

mg_result_t handle_close(mg_handle_t handle)
{
    assert(handle > 0 && (usize)handle <= open_directory_count);
    assert(active_directories > 0U);
    active_directories--;
    return MG_OK;
}

static int run(int argc, char **argv)
{
    return del_command_main(argc, argv);
}

static void reset(void)
{
    remove_calls = 0;
    removed_path_count = 0;
    open_directory_count = 0;
    active_directories = 0;
    failed_remove_path = NULL;
    root_remove_count = 0U;
    sub_remove_count = 0U;
    successful_path_count = 0U;
    memset(successful_paths, 0, sizeof(successful_paths));
    memset(removed_paths, 0, sizeof(removed_paths));
    memset(open_directories, 0, sizeof(open_directories));
    removed_path[0] = '\0';
    help_requested = false;
    usage_requested = false;
}

int main(void)
{
    char *file[] = {"del", "file"};
    char *empty[] = {"del", "empty"};
    char *nonempty[] = {"del", "nonempty"};
    char *missing[] = {"del", "missing"};
    char *special[] = {"del", "special"};
    char *denied[] = {"del", "denied"};
    char *no_operand[] = {"del"};
    char *help[] = {"del", "--help"};
    char *recursive[] = {"del", "-r", "tree"};
    char *mounted[] = {"del", "-r", "mountroot"};
    char *recursive_failure[] = {"del", "--recursive", "tree"};
    char *multiple[] = {"del", "file", "empty", "missing", "denied"};
    char *middle_failure[] = {"del", "file", "denied", "empty"};
    char *mixed_recursive[] = {"del", "-r", "file", "tree"};
    char *duplicate[] = {"del", "file", "file"};
    char *overlap[] = {"del", "-r", "tree", "tree/first"};
    char *leading_dash[] = {"del", "--", "-file"};

    reset();
    assert(run(2, file) == 0 && remove_calls == 1);
    assert(!strcmp(removed_path, "file"));

    reset();
    assert(run(2, empty) == 0 && remove_calls == 1);
    assert(!strcmp(removed_path, "empty"));

    reset();
    assert(run(2, nonempty) == 1 && remove_calls == 1);
    assert(!strcmp(removed_path, "nonempty"));

    reset();
    assert(run(2, missing) == 1 && remove_calls == 0);

    reset();
    assert(run(2, special) == 1 && remove_calls == 0);

    reset();
    assert(run(2, denied) == 1 && remove_calls == 1);

    reset();
    assert(run(3, recursive) == 0);
    assert(open_directory_count == 2U);
    assert(!strcmp(removed_paths[removed_path_count - 4U], "tree/sub/second"));
    assert(!strcmp(removed_paths[removed_path_count - 3U], "tree/sub"));
    assert(!strcmp(removed_paths[removed_path_count - 2U], "tree/first"));
    assert(!strcmp(removed_paths[removed_path_count - 1U], "tree"));

    reset();
    assert(run(3, mounted) == 1);
    assert(open_directory_count == 0U);
    assert(removed_path_count == 1U &&
           !strcmp(removed_paths[0], "mountroot"));

    reset();
    failed_remove_path = "tree/sub/second";
    assert(run(3, recursive_failure) == 1);
    assert(!strcmp(removed_paths[removed_path_count - 1U],
                   "tree/sub/second"));
    assert(removed_path_count == 4U);

    reset();
    assert(run(5, multiple) == 1);
    assert(successful_path_count == 2U);
    assert(!strcmp(successful_paths[0], "file") &&
           !strcmp(successful_paths[1], "empty"));

    reset();
    assert(run(4, middle_failure) == 1);
    assert(successful_path_count == 2U);
    assert(!strcmp(successful_paths[0], "file") &&
           !strcmp(successful_paths[1], "empty"));

    reset();
    assert(run(4, mixed_recursive) == 0);
    assert(successful_path_count >= 2U);
    assert(!strcmp(successful_paths[0], "file"));
    assert(!strcmp(successful_paths[successful_path_count - 1U], "tree"));

    reset();
    assert(run(3, duplicate) == 1);
    assert(successful_path_count == 1U);

    reset();
    assert(run(4, overlap) == 1);
    assert(!strcmp(successful_paths[successful_path_count - 1U], "tree"));

    reset();
    assert(run(1, no_operand) == 1 && usage_requested);
    assert(run(2, help) == 0 && help_requested);

    reset();
    assert(run(3, leading_dash) == 0 && remove_calls == 1U);
    assert(!strcmp(removed_path, "-file"));
    return 0;
}
