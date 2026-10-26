/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <random_syscall.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static u8 valid_buffer[64];
static unsigned range_checks;
static unsigned generator_calls;
static mg_result_t generator_result = MG_OK;

bool vmm_user_range_writable(const void *address, usize length)
{
    range_checks++;
    return address == valid_buffer && length <= sizeof(valid_buffer);
}

mg_result_t csprng_fill(void *output, usize length)
{
    generator_calls++;
    if (generator_result == MG_OK && length)
        memset(output, 0x6d, length);
    return generator_result;
}

int main(void)
{
    assert(syscall_random_fill(NULL, 0) == MG_OK);
    assert(range_checks == 0 && generator_calls == 1);
    assert(syscall_random_fill(NULL, 1) == MG_ERR_BAD_ARGUMENT);
    assert(generator_calls == 1);
    assert(syscall_random_fill((void *)1, 8) == MG_ERR_BAD_ARGUMENT);
    assert(range_checks == 1 && generator_calls == 1);
    assert(syscall_random_fill(valid_buffer, sizeof(valid_buffer) + 1U) ==
           MG_ERR_BAD_ARGUMENT);
    assert(range_checks == 2 && generator_calls == 1);
    assert(syscall_random_fill(valid_buffer, ~(u64)0) == MG_ERR_BAD_ARGUMENT);
    assert(range_checks == 3 && generator_calls == 1);
    assert(syscall_random_fill(valid_buffer, sizeof(valid_buffer)) == MG_OK);
    assert(range_checks == 4 && generator_calls == 2);
    for (usize index = 0; index < sizeof(valid_buffer); index++)
        assert(valid_buffer[index] == 0x6d);
    generator_result = MG_ERR_ENTROPY_UNAVAILABLE;
    assert(syscall_random_fill(valid_buffer, sizeof(valid_buffer)) ==
           MG_ERR_ENTROPY_UNAVAILABLE);
    assert(generator_calls == 3);
    puts("random syscall full-range validation and error propagation tests passed");
    return 0;
}
