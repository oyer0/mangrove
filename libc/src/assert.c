/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *expression, const char *file, int line,
                   const char *function)
{
    (void)printf("assertion failed: %s (%s:%d, %s)\n",
                 expression, file, line, function ? function : "?");
    abort();
}
