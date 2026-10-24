/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <mg/filesystem.h>
#define main plant_command_main
#include "../../userspace/plant/main.c"

#undef main

static char created[16][256];
static usize created_count;
static char attempted[32][256];
static usize attempted_count;
static bool help_requested;

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
    (void)usage;
    (void)option;
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
    return result == MG_ERR_IO ? "injected failure" : "unexpected error";
}

mg_result_t file_create(const char *path)
{
    assert(attempted_count < sizeof(attempted) / sizeof(attempted[0]));
    strcpy(attempted[attempted_count++], path);
    if (!strcmp(path, "fail")) return MG_ERR_IO;
    for (usize index = 0; index < created_count; index++)
        if (!strcmp(created[index], path)) return MG_ERR_ALREADY_EXISTS;
    assert(created_count < sizeof(created) / sizeof(created[0]));
    strcpy(created[created_count++], path);
    return MG_OK;
}

static void reset(void)
{
    memset(created, 0, sizeof(created));
    memset(attempted, 0, sizeof(attempted));
    created_count = attempted_count = 0;
    help_requested = false;
}

static int run(int argc, char **argv)
{
    return plant_command_main(argc, argv);
}

int main(void)
{
    char *single[] = {"plant", "single"};
    char *multiple[] = {"plant", "file1", "file2", "file3"};
    char *partial[] = {"plant", "first", "fail", "last"};
    char *duplicate[] = {"plant", "same", "same"};
    char *with_option[] = {"plant", "before", "-x", "after"};
    char *missing[] = {"plant"};
    char *help[] = {"plant", "--help"};
    char long_name[300];
    char *invalid[] = {"plant", long_name, "after-invalid"};

    reset();
    assert(run(2, single) == 0);
    assert(created_count == 1 && !strcmp(created[0], "single"));

    reset();
    assert(run(4, multiple) == 0);
    assert(created_count == 3 && attempted_count == 3);
    assert(!strcmp(created[0], "file1") &&
           !strcmp(created[1], "file2") &&
           !strcmp(created[2], "file3"));

    reset();
    assert(run(4, partial) == 1);
    assert(created_count == 2 && attempted_count == 3);
    assert(!strcmp(attempted[0], "first") &&
           !strcmp(attempted[1], "fail") &&
           !strcmp(attempted[2], "last"));

    reset();
    assert(run(3, duplicate) == 1);
    assert(created_count == 1 && attempted_count == 2);

    reset();
    assert(run(4, with_option) == 1);
    assert(created_count == 2 && attempted_count == 2);
    assert(!strcmp(created[0], "before") && !strcmp(created[1], "after"));

    reset();
    assert(run(1, missing) == 1 && attempted_count == 0);
    assert(run(2, help) == 0 && help_requested && attempted_count == 0);

    memset(long_name, 'x', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    reset();
    assert(run(3, invalid) == 1);
    assert(created_count == 1 && attempted_count == 1 &&
           !strcmp(created[0], "after-invalid"));
    return 0;
}
