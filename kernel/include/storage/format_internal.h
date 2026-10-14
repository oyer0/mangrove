/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <storage/format.h>

/* Internal raw-format callbacks are private to their implementation. Only
 * this executor constructs a context, after exclusive private-owner admission.
 * All callback I/O must use these identity-bound, evidence-retaining helpers.
 * Geometry is the immutable admitted snapshot, never a reusable registry slot. */
typedef struct storage_format_context storage_format_context_t;
typedef bool (*storage_format_writer_t)(storage_format_context_t *context);
int storage_format_execute_for_id(block_device_t *device, u64 expected_id,
                                  const char *filesystem,
                                  storage_format_writer_t writer);
const block_device_t *storage_format_target(const storage_format_context_t *context);
bool storage_format_read(storage_format_context_t *context, u64 lba,
                          u32 count, void *buffer);
bool storage_format_write(storage_format_context_t *context, u64 lba,
                           u32 count, const void *buffer);
