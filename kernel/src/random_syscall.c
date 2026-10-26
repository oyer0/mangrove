/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <random_syscall.h>

#include <csprng.h>
#include <vmm.h>

mg_result_t syscall_random_fill(void *buffer, u64 length)
{
    if ((usize)length != length ||
        (length && (!buffer ||
                    !vmm_user_range_writable(buffer, (usize)length))))
        return MG_ERR_BAD_ARGUMENT;
    return csprng_fill(buffer, (usize)length);
}
