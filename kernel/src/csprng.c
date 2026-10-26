/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <csprng.h>

#include <crypto/hmac_drbg_sha256.h>
#include <crypto/sha256.h>
#include <entropy.h>
#include <spinlock.h>
#include <string.h>

#define CSPRNG_OUTPUT_CHUNK_BYTES 256U

/* A zero-valued static spinlock is unlocked. Static initialization avoids a
 * one-time initialization race if readiness is queried concurrently. */
static spinlock_t csprng_lock = {0};
static hmac_drbg_sha256_t csprng_state;

static bool csprng_seed_locked(void)
{
    u8 entropy[HMAC_DRBG_SHA256_ENTROPY_BYTES];
    u8 nonce[HMAC_DRBG_SHA256_NONCE_BYTES];
    bool success = false;

    if (!entropy_random_bytes(entropy, sizeof(entropy)) ||
        !entropy_random_bytes(nonce, sizeof(nonce)))
        goto done;
    success = hmac_drbg_sha256_instantiate(
        &csprng_state, entropy, sizeof(entropy), nonce, sizeof(nonce));

done:
    if (!success) hmac_drbg_sha256_clear(&csprng_state);
    crypto_secure_clear(entropy, sizeof(entropy));
    crypto_secure_clear(nonce, sizeof(nonce));
    return success;
}

static bool csprng_reseed_locked(void)
{
    u8 entropy[HMAC_DRBG_SHA256_ENTROPY_BYTES];
    bool success = entropy_random_bytes(entropy, sizeof(entropy)) &&
                   hmac_drbg_sha256_reseed(&csprng_state, entropy,
                                           sizeof(entropy));

    if (!success) hmac_drbg_sha256_clear(&csprng_state);
    crypto_secure_clear(entropy, sizeof(entropy));
    return success;
}

static bool csprng_ready_locked(void)
{
    if (!csprng_state.instantiated && !csprng_seed_locked()) return false;
    if (csprng_state.reseed_counter >= HMAC_DRBG_SHA256_RESEED_INTERVAL &&
        !csprng_reseed_locked())
        return false;
    return csprng_state.instantiated;
}

mg_result_t csprng_initialize(void)
{
    u64 flags;

    flags = spin_lock_irqsave(&csprng_lock);
    if (!csprng_ready_locked()) {
        spin_unlock_irqrestore(&csprng_lock, flags);
        return MG_ERR_ENTROPY_UNAVAILABLE;
    }
    spin_unlock_irqrestore(&csprng_lock, flags);
    return MG_OK;
}

mg_result_t csprng_status(void)
{
    u64 flags;
    bool ready;

    flags = spin_lock_irqsave(&csprng_lock);
    ready = csprng_ready_locked();
    spin_unlock_irqrestore(&csprng_lock, flags);
    return ready ? MG_OK : MG_ERR_ENTROPY_UNAVAILABLE;
}

mg_result_t csprng_fill(void *output, usize length)
{
    u8 *bytes = (u8 *)output;
    usize offset = 0;

    if (length == 0U) return MG_OK;
    if (!output) return MG_ERR_BAD_ARGUMENT;
    while (offset < length) {
        u8 chunk[CSPRNG_OUTPUT_CHUNK_BYTES];
        usize amount = length - offset;
        u64 flags;
        bool generated;

        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        flags = spin_lock_irqsave(&csprng_lock);
        generated = csprng_ready_locked() &&
            hmac_drbg_sha256_generate(&csprng_state, chunk, amount);
        spin_unlock_irqrestore(&csprng_lock, flags);
        if (!generated) {
            crypto_secure_clear(chunk, sizeof(chunk));
            return MG_ERR_ENTROPY_UNAVAILABLE;
        }
        memcpy(bytes + offset, chunk, amount);
        crypto_secure_clear(chunk, sizeof(chunk));
        offset += amount;
    }
    return MG_OK;
}
