/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <crypto/hmac_drbg_sha256.h>

#include <crypto/sha256.h>
#include <string.h>

static void hmac_drbg_sha256_update(hmac_drbg_sha256_t *state,
                                   const u8 *provided_data,
                                   usize provided_length)
{
    u8 material[HMAC_DRBG_SHA256_VALUE_BYTES + 1U +
                HMAC_DRBG_SHA256_ENTROPY_BYTES +
                HMAC_DRBG_SHA256_NONCE_BYTES];
    crypto_hmac_sha256_key_t prepared;
    u8 next_key[HMAC_DRBG_SHA256_KEY_BYTES];

    memcpy(material, state->value, sizeof(state->value));
    material[sizeof(state->value)] = 0;
    if (provided_length)
        memcpy(material + sizeof(state->value) + 1U, provided_data,
               provided_length);

    crypto_hmac_sha256_key_init(&prepared, state->key, sizeof(state->key));
    crypto_hmac_sha256_key_run(&prepared, material,
                               sizeof(state->value) + 1U + provided_length,
                               next_key);
    crypto_hmac_sha256_key_clear(&prepared);
    memcpy(state->key, next_key, sizeof(state->key));

    crypto_hmac_sha256_key_init(&prepared, state->key, sizeof(state->key));
    crypto_hmac_sha256_key_run(&prepared, state->value, sizeof(state->value),
                               state->value);
    crypto_hmac_sha256_key_clear(&prepared);

    if (provided_length) {
        memcpy(material, state->value, sizeof(state->value));
        material[sizeof(state->value)] = 1U;
        memcpy(material + sizeof(state->value) + 1U, provided_data,
               provided_length);
        crypto_hmac_sha256_key_init(&prepared, state->key,
                                    sizeof(state->key));
        crypto_hmac_sha256_key_run(
            &prepared, material, sizeof(state->value) + 1U + provided_length,
            next_key);
        crypto_hmac_sha256_key_clear(&prepared);
        memcpy(state->key, next_key, sizeof(state->key));

        crypto_hmac_sha256_key_init(&prepared, state->key,
                                    sizeof(state->key));
        crypto_hmac_sha256_key_run(&prepared, state->value,
                                   sizeof(state->value), state->value);
        crypto_hmac_sha256_key_clear(&prepared);
    }
    crypto_secure_clear(material, sizeof(material));
    crypto_secure_clear(next_key, sizeof(next_key));
}

void hmac_drbg_sha256_clear(hmac_drbg_sha256_t *state)
{
    crypto_secure_clear(state, state ? sizeof(*state) : 0U);
}

bool hmac_drbg_sha256_instantiate(hmac_drbg_sha256_t *state,
                                  const u8 *entropy, usize entropy_length,
                                  const u8 *nonce, usize nonce_length)
{
    hmac_drbg_sha256_t next;
    u8 seed[HMAC_DRBG_SHA256_ENTROPY_BYTES +
            HMAC_DRBG_SHA256_NONCE_BYTES];

    if (!state || !entropy || !nonce ||
        entropy_length != HMAC_DRBG_SHA256_ENTROPY_BYTES ||
        nonce_length != HMAC_DRBG_SHA256_NONCE_BYTES)
        return false;

    memset(&next, 0, sizeof(next));
    memset(next.value, 1U, sizeof(next.value));
    memcpy(seed, entropy, entropy_length);
    memcpy(seed + entropy_length, nonce, nonce_length);
    hmac_drbg_sha256_update(&next, seed, sizeof(seed));
    next.reseed_counter = 1U;
    next.instantiated = true;
    memcpy(state, &next, sizeof(next));
    hmac_drbg_sha256_clear(&next);
    crypto_secure_clear(seed, sizeof(seed));
    return true;
}

bool hmac_drbg_sha256_reseed(hmac_drbg_sha256_t *state,
                             const u8 *entropy, usize entropy_length)
{
    if (!state || !state->instantiated || !entropy ||
        entropy_length != HMAC_DRBG_SHA256_ENTROPY_BYTES)
        return false;
    hmac_drbg_sha256_update(state, entropy, entropy_length);
    state->reseed_counter = 1U;
    return true;
}

bool hmac_drbg_sha256_generate(hmac_drbg_sha256_t *state,
                               u8 *output, usize output_length)
{
    crypto_hmac_sha256_key_t prepared;
    usize offset = 0;

    if (!state || !state->instantiated ||
        (output_length && !output) ||
        output_length > HMAC_DRBG_SHA256_MAX_REQUEST_BYTES ||
        state->reseed_counter >= HMAC_DRBG_SHA256_RESEED_INTERVAL)
        return false;
    if (output_length == 0U) return true;

    crypto_hmac_sha256_key_init(&prepared, state->key, sizeof(state->key));
    while (offset < output_length) {
        usize amount = output_length - offset;
        if (amount > sizeof(state->value)) amount = sizeof(state->value);
        crypto_hmac_sha256_key_run(&prepared, state->value,
                                   sizeof(state->value), state->value);
        memcpy(output + offset, state->value, amount);
        offset += amount;
    }
    crypto_hmac_sha256_key_clear(&prepared);
    hmac_drbg_sha256_update(state, NULL, 0U);
    state->reseed_counter++;
    return true;
}
