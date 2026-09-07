/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Best-effort hardware entropy for small kernel identifiers and credentials.
 * Callers must retain their own fallback when the platform has no supported
 * instruction or the instruction reports that no value is ready. */
bool entropy_available(void);
bool entropy_random_u64(u64 *value);
