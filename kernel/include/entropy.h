/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Raw CPU-provided random words for kernel internals. This is not a
 * userspace CSPRNG interface. Source readiness and output are best effort;
 * callers needing cryptographic bytes must fail closed if this source fails. */
bool entropy_available(void);
bool entropy_random_u64(u64 *value);
/* Fill is all-or-error: on failure, the complete output range is cleared. */
bool entropy_random_bytes(u8 *output, usize length);
