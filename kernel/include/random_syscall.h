/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <mangrove_errors.h>
#include <mg/types.h>

/* Validate a userspace destination as one complete writable range before
 * generating any bytes into it. */
mg_result_t syscall_random_fill(void *buffer, u64 length);
