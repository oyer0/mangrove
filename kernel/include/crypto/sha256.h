/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Internal Pith cryptographic primitives. These declarations are not a
 * userspace ABI and do not imply algorithm or module validation. */
typedef struct {
    u32 state[8];
    u64 bit_count;
    u8 block[64];
    u32 block_length;
} crypto_sha256_context_t;

typedef struct {
    crypto_sha256_context_t inner;
    crypto_sha256_context_t outer;
} crypto_hmac_sha256_key_t;

void crypto_secure_clear(void *buffer, usize size);
void crypto_sha256_init(crypto_sha256_context_t *context);
void crypto_sha256_update(crypto_sha256_context_t *context,
                          const void *data, usize length);
void crypto_sha256_final(crypto_sha256_context_t *context, u8 digest[32]);
void crypto_hmac_sha256_key_init(crypto_hmac_sha256_key_t *prepared,
                                 const u8 *key, usize key_length);
void crypto_hmac_sha256_key_run(const crypto_hmac_sha256_key_t *prepared,
                                const u8 *data, usize data_length,
                                u8 output[32]);
void crypto_hmac_sha256_key_clear(crypto_hmac_sha256_key_t *prepared);
