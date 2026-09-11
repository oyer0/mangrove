/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>
#include <stdio.h>

/* Format scheduler CPU time consistently in process-oriented tools. */
static bool process_format_cpu_time(u64 milliseconds, char *output,
                                    usize capacity)
{
    u64 seconds;
    int length;

    if (!output || capacity == 0U) return false;
    seconds = milliseconds / 1000ULL;
    length = snprintf(output, capacity, "%02llu:%02llu:%02llu",
                      seconds / 3600ULL, (seconds / 60ULL) % 60ULL,
                      seconds % 60ULL);
    return length >= 0 && (usize)length < capacity;
}
