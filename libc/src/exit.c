/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdlib.h>
#include <stddef.h>

#define ATEXIT_MAX 32U

static void (*normal_functions[ATEXIT_MAX])(void);
static void (*quick_functions[ATEXIT_MAX])(void);
static size_t normal_count;
static size_t quick_count;
static bool terminating;

int atexit(void (*function)(void))
{
    if (!function || normal_count == ATEXIT_MAX) return -1;
    normal_functions[normal_count++] = function;
    return 0;
}

int at_quick_exit(void (*function)(void))
{
    if (!function || quick_count == ATEXIT_MAX) return -1;
    quick_functions[quick_count++] = function;
    return 0;
}

void _Exit(int status)
{
    process_exit(status);
}

static void run_callbacks(void (**functions)(void), size_t *count)
{
    while (*count) {
        void (*function)(void) = functions[--*count];
        functions[*count] = (void (*)(void))0;
        function();
    }
}

void exit(int status)
{
    if (terminating) _Exit(status);
    terminating = true;
    run_callbacks(normal_functions, &normal_count);
    _Exit(status);
}

void quick_exit(int status)
{
    if (terminating) _Exit(status);
    terminating = true;
    run_callbacks(quick_functions, &quick_count);
    _Exit(status);
}

void abort(void)
{
    __builtin_trap();
    for (;;) __asm__ volatile("pause");
}
