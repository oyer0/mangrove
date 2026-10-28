/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>
typedef struct page_table page_table_t;
typedef struct {
    page_table_t *address_space;
    uintptr_t first_page;
    usize page_count;
    bool active;
} vmm_user_range_lease_t;
page_table_t *vmm_get_current_pml4(void);
bool vmm_user_range_lease(page_table_t *pml4, const void *address,
                          usize length, bool writable,
                          vmm_user_range_lease_t *out_lease);
void vmm_user_range_release(vmm_user_range_lease_t *lease);
