/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/random.h>

#include <assert.h>
#include <stdio.h>

static unsigned long observed_number, observed_arg0, observed_arg1, observed_arg2;
static long next_result;

long mg_syscall(unsigned long number, unsigned long arg0, unsigned long arg1,
                unsigned long arg2)
{
    observed_number = number;
    observed_arg0 = arg0;
    observed_arg1 = arg1;
    observed_arg2 = arg2;
    return next_result;
}

int main(void)
{
    u8 bytes[32];
    next_result = MG_OK;
    assert(mg_random_status() == MG_OK && observed_number == 100);
    next_result = MG_ERR_ENTROPY_UNAVAILABLE;
    assert(mg_random_status() == MG_ERR_ENTROPY_UNAVAILABLE);
    next_result = MG_OK;
    assert(mg_random_bytes(bytes, sizeof(bytes)) == MG_OK);
    assert(observed_number == 101 && observed_arg0 == (unsigned long)bytes &&
           observed_arg1 == sizeof(bytes) && observed_arg2 == 0);
    assert(mg_random_bytes(NULL, 0) == MG_OK);
    assert(observed_number == 101 && observed_arg0 == 0 && observed_arg1 == 0);
    puts("native random userspace wrapper ABI tests passed");
    return 0;
}
