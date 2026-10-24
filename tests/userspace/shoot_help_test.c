/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../userspace/shoot/builtin.h"

static char output[2048];
static usize output_length;
static usize handler_calls;
static usize command_help_calls;
static usize shell_run_calls;
static usize script_run_calls;
static char requested_program[64];

static int capture_printf(const char *format, ...)
{
    va_list arguments;
    int written;

    if (output_length >= sizeof(output)) return -1;
    va_start(arguments, format);
    written = vsnprintf(output + output_length,
                        sizeof(output) - output_length, format, arguments);
    va_end(arguments);
    if (written < 0 || (usize)written >= sizeof(output) - output_length)
        return -1;
    output_length += (usize)written;
    return written;
}

#define printf capture_printf
#include "../../userspace/shoot/help.c"
#include "../../userspace/shoot/builtin.c"
#define main shoot_program_main
#include "../../userspace/shoot/main.c"
#undef main
#undef printf

bool execute_jump(shell_state_t *state, const shell_command_t *command)
{
    (void)state; (void)command;
    handler_calls++;
    return true;
}

bool execute_help(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    return render_help(command);
}

bool execute_reload(shell_state_t *state, const shell_command_t *command)
{
    (void)state; (void)command;
    handler_calls++;
    return true;
}

bool execute_history(shell_state_t *state, const shell_command_t *command)
{
    (void)state; (void)command;
    handler_calls++;
    return true;
}

bool execute_exit(shell_state_t *state, const shell_command_t *command)
{
    (void)state; (void)command;
    handler_calls++;
    return true;
}

bool command_help_requested(int argc, char **argv)
{
    return argc == 2 && argv && argv[1] &&
           (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));
}

int command_print_help(const char *argv0)
{
    command_help_calls++;
    const char *name = argv0;
    for (const char *cursor = argv0; *cursor; cursor++)
        if (*cursor == '/') name = cursor + 1U;
    strncpy(requested_program, name, sizeof(requested_program) - 1U);
    return 0;
}

void shell_run(void) { shell_run_calls++; }

int shell_run_script(const char *path)
{
    (void)path;
    script_run_calls++;
    return 7;
}

static void reset(void)
{
    memset(output, 0, sizeof(output));
    output_length = 0U;
    handler_calls = 0U;
    command_help_calls = shell_run_calls = script_run_calls = 0U;
    memset(requested_program, 0, sizeof(requested_program));
}

int main(void)
{
    shell_command_t command = {0};

    reset();
    assert(execute_builtin(NULL, &(shell_command_t){.name = "help"}));
    assert(strstr(output, "Shoot built-in commands:") != NULL);
    assert(strstr(output, "jump") != NULL);
    assert(strstr(output, "help") != NULL);
    assert(strstr(output, "history") != NULL);
    assert(strstr(output, "change Shoot's current directory") != NULL);
    assert(strstr(output, "/bin") == NULL);

    reset();
    command.name = "help";
    command.argument_count = 1U;
    command.arguments[0] = "external-command";
    assert(!execute_builtin(NULL, &command));
    assert(strstr(output, "Usage: help") != NULL);
    assert(strstr(output, "external-command") == NULL);

    reset();
    memset(&command, 0, sizeof(command));
    command.name = "jump";
    command.argument_count = 1U;
    command.arguments[0] = "--help";
    assert(execute_builtin(NULL, &command));
    assert(strstr(output, "jump - change Shoot's current directory") != NULL);
    assert(strstr(output, "Usage: jump [path]") != NULL);
    assert(handler_calls == 0U);

    reset();
    command.arguments[0] = "-h";
    assert(execute_builtin(NULL, &command));
    assert(strstr(output, "Usage: jump [path]") != NULL);
    assert(handler_calls == 0U);

    reset();
    char *shoot_help[] = {"/bin/shoot", "-h"};
    assert(shoot_program_main(2, shoot_help) == 0);
    assert(command_help_calls == 1U && !strcmp(requested_program, "shoot"));
    assert(shell_run_calls == 0U && script_run_calls == 0U);

    reset();
    char *shoot_long_help[] = {"shoot", "--help"};
    assert(shoot_program_main(2, shoot_long_help) == 0);
    assert(command_help_calls == 1U);

    reset();
    char *shoot_script[] = {"shoot", "script.shoot"};
    assert(shoot_program_main(2, shoot_script) == 7);
    assert(script_run_calls == 1U && command_help_calls == 0U);
    return 0;
}
