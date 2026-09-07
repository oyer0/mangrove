/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/time.h>

extern long mg_syscall(unsigned long number, unsigned long arg0,
                       unsigned long arg1, unsigned long arg2);

mg_result_t mg_clock_monotonic(mg_monotonic_time_t *out)
{
    if (!out)
        return MG_ERR_BAD_ARGUMENT;
    return (mg_result_t)mg_syscall(61, (unsigned long)out, 0, 0);
}

mg_result_t mg_clock_realtime(mg_mangrove_time_t *out)
{
    if (!out)
        return MG_ERR_BAD_ARGUMENT;
    return (mg_result_t)mg_syscall(62, (unsigned long)out, 0, 0);
}

u64 mg_clock_boot_id(void)
{
    return (u64)mg_syscall(63, 0, 0, 0);
}
