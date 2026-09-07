/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <block.h>

/* Protected filesystem construction/metadata operations.  Callers must
 * perform policy and authorization at the syscall boundary; these helpers
 * additionally enforce exact-device liveness, format bounds, and filesystem
 * validity before issuing any write. */
int storage_format_filesystem(block_device_t *device,
                              const char *filesystem);
int storage_set_filesystem_label(block_device_t *device,
                                  const char *filesystem,
                                  const char *label);

/* All protected storage mutations share one bounded operation gate. */
bool storage_mutation_begin(void);
void storage_mutation_end(void);
bool storage_mutation_busy(void);
