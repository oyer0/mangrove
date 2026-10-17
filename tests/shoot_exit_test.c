/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <userspace/shoot/builtin.h>
#include <userspace/shoot/commands/commands.h>
#include <userspace/shoot/shell.h>

static jmp_buf exit_point;
static shell_state_t state;
static i32 exit_status;
static unsigned exit_count;

void shell_exit(shell_state_t *shell, i32 status)
{
    assert(shell == &state);
    exit_status = status;
    exit_count++;
    longjmp(exit_point, 1);
}

static void valid_status(const char *text, i32 expected)
{
    shell_command_t command = {0};
    command.argument_count = 1;
    command.arguments[0] = text;
    exit_count = 0;
    if (!setjmp(exit_point)) {
        (void)execute_exit(&state, &command);
        assert(!"valid exit command returned");
    }
    assert(exit_count == 1 && exit_status == expected);
}

static void invalid_status(const char *text)
{
    shell_command_t command = {0};
    command.argument_count = 1;
    command.arguments[0] = text;
    exit_count = 0;
    assert(execute_exit(&state, &command));
    assert(exit_count == 0);
}

int main(void)
{
    shell_command_t command = {0};
    int prior_errno = ENOENT;

    errno = prior_errno;
    valid_status("-2147483648", INT32_MIN);
    assert(errno == prior_errno);
    valid_status("2147483647", INT32_MAX);
    valid_status("+0", 0);
    valid_status("-00012", -12);

    command.argument_count = 0;
    exit_count = 0;
    if (!setjmp(exit_point)) {
        (void)execute_exit(&state, &command);
        assert(!"exit without status returned");
    }
    assert(exit_count == 1 && exit_status == 0);

    invalid_status("");
    invalid_status("+");
    invalid_status("-");
    invalid_status(" 1");
    invalid_status("1 ");
    invalid_status("1x");
    invalid_status("2147483648");
    invalid_status("-2147483649");
    invalid_status("999999999999999999999999999999999999999");

    puts("Shoot exit status parsing: signed i32 boundaries and strict syntax passed");
    return 0;
}
