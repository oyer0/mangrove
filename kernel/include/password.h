#pragma once

#include <types.h>

/* Password records are deliberately an internal kernel format.  The public
 * account API never exposes any of these fields. */
#define IDENTITY_PASSWORD_MAX_LENGTH 128U
#define IDENTITY_PASSWORD_SALT_BYTES 16U
#define IDENTITY_PASSWORD_HASH_BYTES 32U
#define IDENTITY_PASSWORD_ITERATIONS 120000U
#define IDENTITY_PASSWORD_MIN_ITERATIONS 10000U
#define IDENTITY_PASSWORD_MAX_ITERATIONS 1000000U

typedef enum {
    IDENTITY_AUTH_NONE = 0U,
    IDENTITY_AUTH_PBKDF2_SHA256 = 1U,
} identity_auth_algorithm_t;

typedef struct {
    u32 algorithm;
    u32 iterations;
    u8 salt[IDENTITY_PASSWORD_SALT_BYTES];
    u8 hash[IDENTITY_PASSWORD_HASH_BYTES];
} identity_authentication_t;

bool password_auth_valid(const identity_authentication_t *authentication);
bool password_auth_available(void);
bool password_auth_generate(identity_authentication_t *authentication,
                            const char *password);
bool password_auth_verify(const identity_authentication_t *authentication,
                          const char *password);
void password_secure_clear(void *buffer, usize size);
