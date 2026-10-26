/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/tls.h>
#include <mg/random.h>

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/curve25519.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mg_result_t random_status = MG_OK;
static mg_result_t random_result = MG_OK;
static unsigned int random_calls;
static unsigned int wolfssl_init_calls;
static unsigned int wolfssl_cleanup_calls;

/* Library lifecycle symbols are linked in the native archive. The host
 * fixture supplies them so its sanitizer build can isolate the cryptographic
 * source files under test. TLS API headers are compile-probed separately. */
int wolfSSL_Init(void)
{
    wolfssl_init_calls++;
    return 1;
}

int wolfSSL_Cleanup(void)
{
    wolfssl_cleanup_calls++;
    return 1;
}

mg_result_t mg_random_status(void)
{
    return random_status;
}

mg_result_t mg_random_bytes(void *buffer, usize length)
{
    unsigned char *bytes = (unsigned char *)buffer;
    usize index;
    random_calls++;
    for (index = 0; index < length; ++index)
        bytes[index] = (unsigned char)(index * 29U + 7U);
    if (random_result != MG_OK && length != 0U)
        bytes[length / 2U] = 0xA5U; /* emulate a partial failed write */
    return random_result;
}

static int matches(const unsigned char *left, const unsigned char *right,
                   usize length)
{
    return memcmp(left, right, (size_t)length) == 0;
}

static int all_zero(const unsigned char *bytes, usize length)
{
    usize index;
    for (index = 0; index < length; ++index)
        if (bytes[index] != 0U) return 0;
    return 1;
}

static int test_sha256(void)
{
    static const unsigned char expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    wc_Sha256 context;
    unsigned char digest[sizeof(expected)];
    if (wc_InitSha256(&context) != 0 ||
        wc_Sha256Update(&context, (const byte *)"abc", 3U) != 0 ||
        wc_Sha256Final(&context, digest) != 0)
        return 0;
    return matches(digest, expected, sizeof(expected));
}

static int test_aes_gcm(void)
{
    static const unsigned char ciphertext_expected[16] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,
        0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78
    };
    static const unsigned char tag_expected[16] = {
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
        0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };
    unsigned char key[16] = {0};
    unsigned char nonce[12] = {0};
    unsigned char plaintext[16] = {0};
    unsigned char ciphertext[16];
    unsigned char tag[16];
    Aes aes;
    int result;

    if (wc_AesInit(&aes, NULL, INVALID_DEVID) != 0) return 0;
    result = wc_AesGcmSetKey(&aes, key, sizeof(key));
    if (result == 0)
        result = wc_AesGcmEncrypt(&aes, ciphertext, plaintext,
                                  sizeof(plaintext), nonce, sizeof(nonce),
                                  tag, sizeof(tag), NULL, 0U);
    wc_AesFree(&aes);
    return result == 0 &&
        matches(ciphertext, ciphertext_expected, sizeof(ciphertext)) &&
        matches(tag, tag_expected, sizeof(tag));
}

static int test_x25519(void)
{
    static const unsigned char private_key[32] = {
        0x70,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,
        0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
        0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,
        0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x6a
    };
    static const unsigned char public_expected[32] = {
        0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,
        0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
        0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,
        0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a
    };
    unsigned char public_key[32];
    int result = wc_curve25519_make_pub(sizeof(public_key), public_key,
                                        sizeof(private_key), private_key);
    if (result != 0) {
        printf("MGTLS X25519 return=%d\n", result);
        return 0;
    }
    return matches(public_key, public_expected, sizeof(public_key));
}

static int test_entropy_contract(void)
{
    unsigned char bytes[73];
    WC_RNG rng;
    unsigned char *large;
    const usize large_length = 2U * 1024U * 1024U;

    random_status = MG_ERR_ENTROPY_UNAVAILABLE;
    if (mgtls_init() != MG_ERR_ENTROPY_UNAVAILABLE ||
        wolfssl_init_calls != 0U)
        return 0;
    random_status = MG_OK;
    random_result = MG_ERR_ENTROPY_UNAVAILABLE;
    memset(bytes, 0xCC, sizeof(bytes));
    if (mgtls_random_bytes(bytes, sizeof(bytes)) !=
            MG_ERR_ENTROPY_UNAVAILABLE || !all_zero(bytes, sizeof(bytes)))
        return 0;

    /* The selected wolfCrypt RNG source must reach the same fail-closed
     * adapter; partially written bytes may not escape as successful output. */
    if (wc_InitRng(&rng) != 0) return 0;
    memset(bytes, 0xCC, sizeof(bytes));
    if (wc_RNG_GenerateBlock(&rng, bytes, sizeof(bytes)) == 0 ||
        !all_zero(bytes, sizeof(bytes))) {
        wc_FreeRng(&rng);
        return 0;
    }
    wc_FreeRng(&rng);

    random_result = MG_OK;
    memset(bytes, 0, sizeof(bytes));
    if (mgtls_random_bytes(bytes, sizeof(bytes)) != MG_OK ||
        all_zero(bytes, sizeof(bytes)) ||
        mgtls_random_bytes(NULL, 1U) != MG_ERR_BAD_ARGUMENT ||
        mgtls_random_bytes(NULL, 0U) != MG_OK)
        return 0;
    large = (unsigned char *)malloc((size_t)large_length);
    if (!large) return 0;
    if (mgtls_random_bytes(large, large_length) != MG_OK ||
        all_zero(large, large_length)) {
        free(large);
        return 0;
    }
    free(large);
    if (mgtls_init() != MG_OK || wolfssl_init_calls != 1U ||
        mgtls_cleanup() != MG_OK || wolfssl_cleanup_calls != 1U)
        return 0;
    return random_calls >= 4U;
}

int main(void)
{
    if (!test_sha256()) { puts("MGTLS FAIL sha256"); return 1; }
    if (!test_aes_gcm()) { puts("MGTLS FAIL aes-gcm"); return 1; }
    if (!test_x25519()) { puts("MGTLS FAIL x25519"); return 1; }
    if (!test_entropy_contract()) { puts("MGTLS FAIL entropy"); return 1; }
    puts("MGTLS PASS sha256 aes-gcm x25519 entropy");
    return 0;
}
