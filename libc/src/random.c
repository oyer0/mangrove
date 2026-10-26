/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/random.h>

extern long mg_syscall(unsigned long number, unsigned long arg0,
                       unsigned long arg1, unsigned long arg2);

mg_result_t mg_random_status(void)
{
    return (mg_result_t)mg_syscall(100, 0, 0, 0);
}

mg_result_t mg_random_bytes(void *buffer, usize length)
{
    return (mg_result_t)mg_syscall(101, (unsigned long)buffer,
                                   (unsigned long)length, 0);
}
