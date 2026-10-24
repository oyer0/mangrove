/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mangrove.h>
#include <mangrove_version.h>

#define main about_program_main
#include "../../userspace/about/main.c"
#undef main

static char captured[4096];
static usize captured_length;

bool command_help_requested(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return false;
}

int command_print_help(const char *argv0)
{
    (void)argv0;
    return 1;
}

void command_usage_error(const char *argv0, const char *usage,
                         const char *option)
{
    (void)argv0;
    (void)usage;
    (void)option;
}

mg_result_t terminal_write_semantic(const void *buffer, usize length,
                                    mg_terminal_style_role_t role)
{
    (void)role;
    assert(buffer != NULL);
    assert(length < sizeof(captured) - captured_length);
    memcpy(captured + captured_length, buffer, length);
    captured_length += length;
    captured[captured_length] = '\0';
    return (mg_result_t)length;
}

mg_result_t terminal_update_begin(void)
{
    return MG_OK;
}

mg_result_t terminal_update_end(void)
{
    return MG_OK;
}

mg_result_t process_get_identity(mg_identity_t *identity)
{
    (void)identity;
    return -1;
}

mg_result_t system_info(mg_system_info_t *info)
{
    (void)info;
    return -1;
}

mg_result_t memory_info(mg_system_memory_info_t *info)
{
    (void)info;
    return -1;
}

u64 uptime_ms(void)
{
    return 12000U;
}

int main(void)
{
    char *arguments[] = {"about", NULL};
    char expected_os[128];
    char expected_kernel[128];
    int length;

    assert(about_program_main(1, arguments) == 0);
    length = snprintf(expected_os, sizeof(expected_os), "OS      : %s %s\n",
                      MANGROVE_NAME, MANGROVE_VERSION);
    assert(length >= 0 && (usize)length < sizeof(expected_os));
    assert(strstr(captured, expected_os) != NULL);
    assert(strstr(captured, "Mangrove: ") == NULL);

    length = snprintf(expected_kernel, sizeof(expected_kernel),
                      "Kernel  : %s %s\n", PITH_NAME, MANGROVE_VERSION);
    assert(length >= 0 && (usize)length < sizeof(expected_kernel));
    assert(strstr(captured, expected_kernel) != NULL);
    assert(strstr(captured, "Shell   : Shoot\n") != NULL);
    assert(strstr(captured, "Uptime  : 00:00:12\n") != NULL);
    assert(strcmp(about_logo[0], "                      yyg@$,") == 0);
    return 0;
}
