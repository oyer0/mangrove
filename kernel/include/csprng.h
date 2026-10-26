/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <mangrove_errors.h>
#include <mg/types.h>
#include <mangrove_errors.h>

/* Kernel-only, system-wide generator API. Callers serialize access through
 * this module; the output pointer must refer to a validated writable range. */
mg_result_t csprng_initialize(void);
mg_result_t csprng_status(void);
mg_result_t csprng_fill(void *output, usize length);
