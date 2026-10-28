/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <random_syscall.h>
#include <vmm.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static u8 valid_buffer[64];
static unsigned range_checks;
static unsigned lease_releases;
static unsigned generator_calls;
static mg_result_t generator_result = MG_OK;
struct page_table { unsigned unused; };
static struct page_table current_address_space;

page_table_t *vmm_get_current_pml4(void)
{
    return &current_address_space;
}

bool vmm_user_range_lease(page_table_t *pml4, const void *address,
                          usize length, bool writable,
                          vmm_user_range_lease_t *out_lease)
{
    assert(writable && out_lease);
    memset(out_lease, 0, sizeof(*out_lease));
    if (!length) return pml4 == &current_address_space;
    if (!address) return false;
    range_checks++;
    if (pml4 != &current_address_space || address != valid_buffer ||
        length > sizeof(valid_buffer)) return false;
    out_lease->address_space = pml4;
    out_lease->first_page = 0x1000;
    out_lease->page_count = 1;
    out_lease->active = true;
    return true;
}

void vmm_user_range_release(vmm_user_range_lease_t *lease)
{
    assert(lease);
    if (!lease->active) return;
    lease->active = false;
    lease_releases++;
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
    assert(range_checks == 0 && lease_releases == 0 && generator_calls == 1);
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
    assert(range_checks == 4 && lease_releases == 1 && generator_calls == 2);
    for (usize index = 0; index < sizeof(valid_buffer); index++)
        assert(valid_buffer[index] == 0x6d);
    generator_result = MG_ERR_ENTROPY_UNAVAILABLE;
    assert(syscall_random_fill(valid_buffer, sizeof(valid_buffer)) ==
           MG_ERR_ENTROPY_UNAVAILABLE);
    assert(generator_calls == 3 && lease_releases == 2);
    puts("random syscall full-range validation and error propagation tests passed");
    return 0;
}
