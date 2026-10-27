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
static const char *read_path;
static const char *pager_path;
static char pager_text[32];
static usize pager_length;
static u8 literal_output[512];
static usize literal_output_length;

bool command_help_requested(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return help_requested;
}

int command_print_help(const char *argv0)
{
    assert(argv0 != NULL);
    return 7;
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
    static const char contents[] = "read only\n";
    read_path = path;
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
    assert(strcmp(title, path) == 0);
    pager_called = true;
    pager_path = path;
    assert(length <= sizeof(pager_text));
    memcpy(pager_text, text, length);
    pager_length = length;
    return pager_result;
}

const char *error_string(mg_result_t error)
{
    (void)error;
    return "test error";
}

mg_result_t console_write_literal(const void *buffer, usize length)
{
    if (length > sizeof(literal_output) - literal_output_length)
        return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(literal_output + literal_output_length, buffer, length);
    literal_output_length += length;
    return (mg_result_t)length;
}

#define main view_program_main
#include "../../userspace/view/main.c"
#undef main

static void reset(void)
{
    help_requested = false;
    usage_called = false;
    pager_called = false;
    file_result = MG_OK;
    pager_result = MG_OK;
    read_path = NULL;
    pager_path = NULL;
    memset(pager_text, 0, sizeof(pager_text));
    pager_length = 0U;
    memset(literal_output, 0, sizeof(literal_output));
    literal_output_length = 0U;
}

static void test_usage_and_help(void)
{
    char *no_args[] = {"view", NULL};
    char *extra[] = {"view", "file", "extra", NULL};
    char *help[] = {"view", "--help", NULL};

    reset();
    assert(view_program_main(1, no_args) != 0 && usage_called);
    reset();
    assert(view_program_main(3, extra) != 0 && usage_called);
    reset();
    help_requested = true;
    assert(view_program_main(2, help) == 7 && !usage_called);
}

static void test_read_only_pager_handoff_and_errors(void)
{
    char *args[] = {"view", "/home/user/readme.txt", NULL};

    reset();
    assert(view_program_main(2, args) == 0);
    assert(read_path == args[1] && pager_called);
    assert(pager_path == args[1] && pager_length == strlen("read only\n"));
    assert(memcmp(pager_text, "read only\n", pager_length) == 0);

    reset();
    file_result = MG_ERR_ACCESS_DENIED;
    assert(view_program_main(2, args) != 0 && !pager_called);

    {
        char *hostile[] = {"view", "\x1b{cls}", NULL};
        static const char expected[] =
            "view: \x1b{cls}: test error\n";

        reset();
        file_result = MG_ERR_ACCESS_DENIED;
        assert(view_program_main(2, hostile) != 0 && !pager_called);
        assert(literal_output_length == sizeof(expected) - 1U);
        assert(memcmp(literal_output, expected, sizeof(expected) - 1U) == 0);
    }

    reset();
    pager_result = MG_ERR_IO;
    assert(view_program_main(2, args) != 0 && pager_called);

    {
        char *hostile[] = {"view", "\x1b{at:1,1}", NULL};
        static const char expected[] =
            "view: could not display \x1b{at:1,1}: test error\n";

        reset();
        pager_result = MG_ERR_IO;
        assert(view_program_main(2, hostile) != 0 && pager_called);
        assert(literal_output_length == sizeof(expected) - 1U);
        assert(memcmp(literal_output, expected, sizeof(expected) - 1U) == 0);
    }
}

int main(void)
{
    test_usage_and_help();
    test_read_only_pager_handoff_and_errors();
    return 0;
}
