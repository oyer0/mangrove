/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>

/* Mangrove currently has one userspace thread per address space. This process
 * cell is deliberately not advertised as thread-local storage. */
static int process_errno;

int *__errno_location(void)
{
    return &process_errno;
}
