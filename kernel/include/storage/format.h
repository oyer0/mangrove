/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <block.h>

/* Protected filesystem construction/metadata operations. Callers perform
 * user authorization at the syscall boundary; formatting entries require
 * management/quiescence and exclusive recovery
 * admission, exact-device identity, read-only policy and bounded writes.
 * Failed destructive writes retain uncertainty; formatting is not rollback. */
/* Preserve the caller-selected registration through mandatory admission. */
int storage_format_filesystem_for_id(block_device_t *device, u64 expected_id,
                                     const char *filesystem);
int storage_set_filesystem_label(block_device_t *device,
                                  const char *filesystem,
                                  const char *label);

/* Preserve the caller's saved registration across admission; native label
 * requests use this form instead of recapturing a reusable device slot. */
int storage_set_filesystem_label_for_id(block_device_t *device, u64 expected_id,
                                        const char *filesystem, const char *label);

/* Nonblocking SMP admission. A successful begin holds the block-I/O gate
 * for the complete operation; no spinlock spans I/O. Also protects shared GPT
 * read/plan scratch. Non-recursive: nested GPT work uses private helpers. */
bool storage_mutation_begin(void);
void storage_mutation_end(void);
bool storage_mutation_busy(void);
/* Caller holds management admission/quiescence. Reject a planned write that
 * overlaps another registered range. A partition's containing physical parent
 * is allowed; siblings must be disjoint. No I/O, cache changes or allocation. */
int storage_management_range_preflight(block_device_t *device, u64 expected_id,
                                        u64 lba, u64 count);
