/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

/* Query whether the kernel generator is currently seeded and can satisfy a
 * request without first exhausting its reseed interval. This may initialize
 * or reseed the generator. */
mg_result_t mg_random_status(void);

/* Fill length bytes with kernel-generated cryptographic random bytes. Any
 * length is accepted subject to mapped writable memory and available CPU
 * time. A zero-length request succeeds without requiring generator readiness;
 * NULL is permitted only when length is zero. Invalid/unwritable ranges return
 * MG_ERR_BAD_ARGUMENT. Missing or failed entropy returns
 * MG_ERR_ENTROPY_UNAVAILABLE. On error, the buffer may hold a prefix of
 * output and must be treated as entirely unusable. */
mg_result_t mg_random_bytes(void *buffer, usize length);
