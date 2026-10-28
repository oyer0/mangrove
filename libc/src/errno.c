/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>

/* Mangrove's local-exec static TLS model gives each Ring 3 context an
 * independent errno cell. Other loam state is not thereby thread-safe. */
static _Thread_local int process_errno;

int *__errno_location(void)
{
    return &process_errno;
}
