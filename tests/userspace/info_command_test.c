/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <mg/error.h>
#include <mg/filesystem.h>
#include "../../userspace/common/help.h"
#include "../../userspace/common/text_pager.h"

static bool help_requested;
static bool usage_called;
static bool pager_called;
static mg_result_t file_result = MG_OK;
static mg_result_t pager_result = MG_OK;
static char captured_path[MG_PATH_MAX_BYTES + 1U];
static char captured_title[64];
static char captured_text[32];
static usize captured_length;

bool command_help_requested(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return help_requested;
}

int command_print_help(const char *argv0)
{
    assert(argv0 != NULL);
    return 9;
}

void command_usage_error(const char *argv0, const char *usage,
                         const char *option)
{
    assert(argv0 != NULL && usage != NULL);
    (void)option;
    usage_called = true;
}

mg_result_t text_pager_read_file(const char *path, char **out_text,
                                 usize *out_length)
{
    static const char contents[] = "manual\n";
    assert(strlen(path) < sizeof(captured_path));
    strcpy(captured_path, path);
    if (file_result != MG_OK) return file_result;
    *out_text = malloc(sizeof(contents));
    assert(*out_text != NULL);
    memcpy(*out_text, contents, sizeof(contents));
    *out_length = sizeof(contents) - 1U;
    return MG_OK;
}

mg_result_t text_pager_run(const char *title, const char *path,
                           const void *text, usize length)
{
    assert(strlen(title) < sizeof(captured_title));
    assert(strlen(path) < sizeof(captured_path));
    strcpy(captured_title, title);
    strcpy(captured_path, path);
    assert(length <= sizeof(captured_text));
    memcpy(captured_text, text, length);
    captured_length = length;
    pager_called = true;
    return pager_result;
}

const char *error_string(mg_result_t error)
{
    (void)error;
    return "test error";
}

#define main info_program_main
#include "../../userspace/info/main.c"
#undef main

static void reset(void)
{
    help_requested = false;
    usage_called = false;
    pager_called = false;
    file_result = MG_OK;
    pager_result = MG_OK;
    memset(captured_path, 0, sizeof(captured_path));
    memset(captured_title, 0, sizeof(captured_title));
    memset(captured_text, 0, sizeof(captured_text));
    captured_length = 0U;
}

static void test_usage_and_help(void)
{
    char *no_args[] = {"info", NULL};
    char *extra[] = {"info", "reed", "config", NULL};
    char *traversal[] = {"info", "../secret", NULL};
    char *path[] = {"info", "/etc/passwd", NULL};
    char *help[] = {"info", "-h", NULL};

    reset();
    assert(info_program_main(1, no_args) != 0 && usage_called);
    reset();
    assert(info_program_main(3, extra) != 0 && usage_called);
    reset();
    assert(info_program_main(2, traversal) != 0 && usage_called);
    reset();
    assert(info_program_main(2, path) != 0 && usage_called);
    reset();
    help_requested = true;
    assert(info_program_main(2, help) == 9 && !usage_called);
}

static void test_manual_lookup_and_failure_paths(void)
{
    char *args[] = {"info", "reed", NULL};

    reset();
    assert(info_program_main(2, args) == 0);
    assert(pager_called && strcmp(captured_title, "reed") == 0);
    assert(strcmp(captured_path, "/share/info/reed") == 0);
    assert(captured_length == strlen("manual\n"));
    assert(memcmp(captured_text, "manual\n", captured_length) == 0);

    reset();
    file_result = MG_ERR_NOT_FOUND;
    assert(info_program_main(2, args) != 0 && !pager_called);

    reset();
    file_result = MG_ERR_ACCESS_DENIED;
    assert(info_program_main(2, args) != 0 && !pager_called);

    reset();
    pager_result = MG_ERR_IO;
    assert(info_program_main(2, args) != 0 && pager_called);
}

int main(void)
{
    test_usage_and_help();
    test_manual_lookup_and_failure_paths();
    return 0;
}
