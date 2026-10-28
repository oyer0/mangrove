/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <random_syscall.h>

#include <csprng.h>
#include <vmm.h>

mg_result_t syscall_random_fill(void *buffer, u64 length)
{
    vmm_user_range_lease_t lease;
    if ((usize)length != length ||
        !vmm_user_range_lease(vmm_get_current_pml4(), buffer,
                              (usize)length, true, &lease))
        return MG_ERR_BAD_ARGUMENT;
    mg_result_t result = csprng_fill(buffer, (usize)length);
    vmm_user_range_release(&lease);
    return result;
}
