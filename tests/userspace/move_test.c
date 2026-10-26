/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <mg/filesystem.h>
#define main move_command_main
#include "../../userspace/move/main.c"
#undef main

typedef struct {
    char source[256];
    char destination[256];
} move_record_t;

static move_record_t moves[32];
static usize move_count;
static const char *failed_source;
static const char *failed_destination;
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
    assert(!strcmp(usage, "move <source>... <destination>"));
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
    if (result == MG_ERR_NOT_FOUND) return "not found";
    if (result == MG_ERR_ALREADY_EXISTS) return "already exists";
    if (result == MG_ERR_IO) return "injected failure";
    return "unexpected error";
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!strcmp(path, "missing")) return MG_ERR_NOT_FOUND;
    if (!strcmp(path, "dest") || !strcmp(path, "dest/"))
        info->type = MG_PATH_TYPE_DIRECTORY;
    else if (!strcmp(path, "directory-source"))
        info->type = MG_PATH_TYPE_DIRECTORY;
    else if (!strcmp(path, "dest-file"))
        info->type = MG_PATH_TYPE_FILE;
    else
        info->type = MG_PATH_TYPE_FILE;
    return MG_OK;
}

mg_result_t path_move(const char *source, const char *destination)
{
    assert(move_count < sizeof(moves) / sizeof(moves[0]));
    strcpy(moves[move_count].source, source);
    strcpy(moves[move_count].destination, destination);
    move_count++;
    if (failed_source && !strcmp(source, failed_source)) return MG_ERR_IO;
    if (failed_destination && !strcmp(destination, failed_destination))
        return MG_ERR_ALREADY_EXISTS;
    return MG_OK;
}

static void reset(void)
{
    memset(moves, 0, sizeof(moves));
    move_count = 0U;
    failed_source = NULL;
    failed_destination = NULL;
    help_requested = usage_requested = false;
}

static int run(int argc, char **argv)
{
    return move_command_main(argc, argv);
}

int main(void)
{
    char *single[] = {"move", "src1", "out"};
    char *single_into_directory[] = {"move", "src1", "dest/"};
    char *multiple[] = {"move", "src1", "src2", "dest"};
    char *multiple_directory[] = {"move", "src1", "directory-source", "dest"};
    char *missing_destination[] = {"move", "src1", "src2", "missing"};
    char *file_destination[] = {"move", "src1", "src2", "dest-file"};
    char *failure_middle[] = {"move", "src1", "src2", "src3", "dest"};
    char *basename_collision[] = {"move", "src1", "other/src1", "dest"};
    char *no_operands[] = {"move"};
    char *help[] = {"move", "--help"};
    char *leading_dash[] = {"move", "--", "-source", "dest"};

    reset();
    assert(run(3, single) == 0 && move_count == 1U);
    assert(!strcmp(moves[0].source, "src1") &&
           !strcmp(moves[0].destination, "out"));

    reset();
    assert(run(3, single_into_directory) == 0 && move_count == 1U);
    assert(!strcmp(moves[0].destination, "dest/src1"));

    reset();
    assert(run(4, multiple) == 0 && move_count == 2U);
    assert(!strcmp(moves[0].destination, "dest/src1") &&
           !strcmp(moves[1].destination, "dest/src2"));

    reset();
    assert(run(4, multiple_directory) == 0 && move_count == 2U);
    assert(!strcmp(moves[1].source, "directory-source") &&
           !strcmp(moves[1].destination, "dest/directory-source"));

    reset();
    assert(run(4, missing_destination) == 1 && move_count == 0U);

    reset();
    assert(run(4, file_destination) == 1 && move_count == 0U);

    reset();
    failed_source = "src2";
    assert(run(5, failure_middle) == 1 && move_count == 3U);
    assert(!strcmp(moves[2].destination, "dest/src3"));

    reset();
    failed_destination = "dest/src1";
    assert(run(4, basename_collision) == 1 && move_count == 2U);
    assert(!strcmp(moves[1].destination, "dest/src1"));

    reset();
    assert(run(4, leading_dash) == 0 && move_count == 1U);
    assert(!strcmp(moves[0].source, "-source") &&
           !strcmp(moves[0].destination, "dest/-source"));

    reset();
    assert(run(1, no_operands) == 1 && usage_requested);
    assert(run(2, help) == 0 && help_requested);
    return 0;
}
