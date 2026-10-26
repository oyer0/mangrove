/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <password.h>

#include <entropy.h>
#include <crypto/sha256.h>
#include <string.h>

static void pbkdf2_sha256(const char *password, const u8 *salt,
                          usize salt_length, u32 iterations, u8 output[32])
{
    u8 input[IDENTITY_PASSWORD_SALT_BYTES + 4U];
    u8 current[32];
    u8 accumulated[32];
    usize password_length = strlen(password);
    crypto_hmac_sha256_key_t prepared;

    crypto_hmac_sha256_key_init(&prepared, (const u8 *)password,
                                password_length);
    memcpy(input, salt, salt_length);
    input[salt_length] = 0;
    input[salt_length + 1U] = 0;
    input[salt_length + 2U] = 0;
    input[salt_length + 3U] = 1;
    crypto_hmac_sha256_key_run(&prepared, input, salt_length + 4U,
                               accumulated);
    memcpy(current, accumulated, sizeof(current));
    for (u32 round = 1U; round < iterations; round++) {
        crypto_hmac_sha256_key_run(&prepared, current, sizeof(current),
                                   current);
        for (u32 index = 0; index < sizeof(accumulated); index++)
            accumulated[index] ^= current[index];
    }
    memcpy(output, accumulated, sizeof(accumulated));
    password_secure_clear(input, sizeof(input));
    password_secure_clear(current, sizeof(current));
    password_secure_clear(accumulated, sizeof(accumulated));
    crypto_hmac_sha256_key_clear(&prepared);
}

static bool random_bytes(u8 *output, usize length)
{
    for (usize offset = 0; offset < length; offset += sizeof(u64)) {
        u64 value;
        usize amount = length - offset < sizeof(value)
            ? length - offset : sizeof(value);
        if (!entropy_random_u64(&value)) return false;
        memcpy(output + offset, &value, amount);
        password_secure_clear(&value, sizeof(value));
    }
    return true;
}

void password_secure_clear(void *buffer, usize size)
{
    crypto_secure_clear(buffer, size);
}

bool password_auth_valid(const identity_authentication_t *authentication)
{
    if (!authentication) return false;
    if (authentication->algorithm == IDENTITY_AUTH_NONE) {
        if (authentication->iterations != 0) return false;
        for (u32 index = 0; index < sizeof(authentication->salt); index++)
            if (authentication->salt[index]) return false;
        for (u32 index = 0; index < sizeof(authentication->hash); index++)
            if (authentication->hash[index]) return false;
        return true;
    }
    return authentication->algorithm == IDENTITY_AUTH_PBKDF2_SHA256 &&
           authentication->iterations >= IDENTITY_PASSWORD_MIN_ITERATIONS &&
           authentication->iterations <= IDENTITY_PASSWORD_MAX_ITERATIONS;
}

bool password_auth_available(void)
{
    return entropy_available();
}

bool password_auth_generate(identity_authentication_t *authentication,
                            const char *password)
{
    usize length;

    if (!authentication || !password) return false;
    length = strlen(password);
    if (length == 0 || length > IDENTITY_PASSWORD_MAX_LENGTH ||
        !random_bytes(authentication->salt,
                      IDENTITY_PASSWORD_SALT_BYTES)) return false;
    authentication->algorithm = IDENTITY_AUTH_PBKDF2_SHA256;
    authentication->iterations = IDENTITY_PASSWORD_ITERATIONS;
    pbkdf2_sha256(password, authentication->salt,
                  IDENTITY_PASSWORD_SALT_BYTES, authentication->iterations,
                  authentication->hash);
    return password_auth_valid(authentication);
}

bool password_auth_verify(const identity_authentication_t *authentication,
                          const char *password)
{
    u8 computed[IDENTITY_PASSWORD_HASH_BYTES];
    u8 difference = 0;
    usize length;

    if (!authentication || !password ||
        authentication->algorithm != IDENTITY_AUTH_PBKDF2_SHA256 ||
        !password_auth_valid(authentication)) return false;
    length = strlen(password);
    if (length > IDENTITY_PASSWORD_MAX_LENGTH) return false;
    pbkdf2_sha256(password, authentication->salt,
                  IDENTITY_PASSWORD_SALT_BYTES, authentication->iterations,
                  computed);
    for (u32 index = 0; index < sizeof(computed); index++)
        difference |= computed[index] ^ authentication->hash[index];
    password_secure_clear(computed, sizeof(computed));
    return difference == 0;
}
