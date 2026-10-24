/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <mg/filesystem.h>
#define main del_command_main
#include "../../userspace/del/main.c"
#undef main

static usize remove_calls;
static char removed_path[256];
static bool help_requested;
static bool usage_requested;

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
    assert(!strcmp(usage, "del <file|empty-directory>"));
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

bool result_is_error(mg_result_t result) { return result < 0; }
const char *error_string(mg_result_t result)
{
    if (result == MG_ERR_NOT_FOUND) return "not found";
    if (result == MG_ERR_ACCESS_DENIED) return "access denied";
    return "injected error";
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!strcmp(path, "missing")) return MG_ERR_NOT_FOUND;
    if (!strcmp(path, "empty") || !strcmp(path, "nonempty"))
        info->type = MG_PATH_TYPE_DIRECTORY;
    else if (!strcmp(path, "special"))
        info->type = (mg_path_type_t)99;
    else
        info->type = MG_PATH_TYPE_FILE;
    return MG_OK;
}

mg_result_t path_remove(const char *path)
{
    remove_calls++;
    strcpy(removed_path, path);
    if (!strcmp(path, "nonempty")) return MG_ERR_NOT_EMPTY;
    if (!strcmp(path, "denied")) return MG_ERR_ACCESS_DENIED;
    return MG_OK;
}

static int run(int argc, char **argv)
{
    return del_command_main(argc, argv);
}

static void reset(void)
{
    remove_calls = 0;
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
    assert(run(1, no_operand) == 1 && usage_requested);
    assert(run(2, help) == 0 && help_requested);
    return 0;
}
