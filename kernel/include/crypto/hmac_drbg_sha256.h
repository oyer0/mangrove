/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define HMAC_DRBG_SHA256_KEY_BYTES 32U
#define HMAC_DRBG_SHA256_VALUE_BYTES 32U
#define HMAC_DRBG_SHA256_ENTROPY_BYTES 32U
#define HMAC_DRBG_SHA256_NONCE_BYTES 16U
#define HMAC_DRBG_SHA256_MAX_REQUEST_BYTES 65536U
#define HMAC_DRBG_SHA256_RESEED_INTERVAL 10000ULL

typedef struct {
    u8 key[HMAC_DRBG_SHA256_KEY_BYTES];
    u8 value[HMAC_DRBG_SHA256_VALUE_BYTES];
    u64 reseed_counter;
    bool instantiated;
} hmac_drbg_sha256_t;

/* These pure state operations use HMAC_DRBG with SHA-256 as specified by
 * NIST SP 800-90A Rev. 1, Section 10.1.2. No entropy is obtained here. */
void hmac_drbg_sha256_clear(hmac_drbg_sha256_t *state);
bool hmac_drbg_sha256_instantiate(hmac_drbg_sha256_t *state,
                                  const u8 *entropy, usize entropy_length,
                                  const u8 *nonce, usize nonce_length);
bool hmac_drbg_sha256_reseed(hmac_drbg_sha256_t *state,
                             const u8 *entropy, usize entropy_length);
bool hmac_drbg_sha256_generate(hmac_drbg_sha256_t *state,
                               u8 *output, usize output_length);
