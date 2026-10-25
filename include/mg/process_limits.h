/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Per-spawn admission budget: argv pointers, the NULL sentinel, and all
 * argument strings including their NUL terminators. Actual capacity also
 * depends on kernel heap, PMM frames, page tables, and process resources. */
#define MG_PROCESS_ARGUMENT_BYTES_MAX (8U * 1024U * 1024U)
/* Every argument needs at least one 64-bit pointer and one NUL byte. This is
 * derived from the byte budget, not a separately imposed argument-count cap.
 * Allocate vectors for the actual request; this is not a stack-array size. */
#define MG_PROCESS_MAX_ARGUMENTS ((MG_PROCESS_ARGUMENT_BYTES_MAX - 8U) / 9U)
