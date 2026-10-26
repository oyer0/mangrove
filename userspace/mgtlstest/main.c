/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/tls.h>

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/curve25519.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <stdio.h>
#include <string.h>

static int equal(const unsigned char *left, const unsigned char *right,
                 usize length)
{
    return memcmp(left, right, (size_t)length) == 0;
}

static int check_sha256(void)
{
    static const unsigned char expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    wc_Sha256 context;
    unsigned char digest[sizeof(expected)];
    return wc_InitSha256(&context) == 0 &&
        wc_Sha256Update(&context, (const byte *)"abc", 3U) == 0 &&
        wc_Sha256Final(&context, digest) == 0 &&
        equal(digest, expected, sizeof(expected));
}

static int check_aes_gcm(void)
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
    int result = wc_AesInit(&aes, NULL, INVALID_DEVID);
    if (result != 0) return 0;
    if (result == 0) result = wc_AesGcmSetKey(&aes, key, sizeof(key));
    if (result == 0)
        result = wc_AesGcmEncrypt(&aes, ciphertext, plaintext,
                                  sizeof(plaintext), nonce, sizeof(nonce),
                                  tag, sizeof(tag), NULL, 0U);
    wc_AesFree(&aes);
    return result == 0 &&
        equal(ciphertext, ciphertext_expected, sizeof(ciphertext)) &&
        equal(tag, tag_expected, sizeof(tag));
}

static int check_x25519(void)
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
    return wc_curve25519_make_pub(sizeof(public_key), public_key,
                                  sizeof(private_key), private_key) == 0 &&
        equal(public_key, public_expected, sizeof(public_key));
}

int main(void)
{
    unsigned char random[64];
    WC_RNG rng;
    mg_result_t result = mgtls_init();
    if (result != MG_OK) {
        puts("MGTLSGUEST FAIL entropy-not-ready");
        return 1;
    }
    if (mgtls_random_bytes(random, sizeof(random)) != MG_OK) {
        puts("MGTLSGUEST FAIL randomness");
        (void)mgtls_cleanup();
        return 1;
    }
    if (wc_InitRng(&rng) != 0) {
        puts("MGTLSGUEST FAIL wolfcrypt-rng-init");
        (void)mgtls_cleanup();
        return 1;
    }
    if (wc_RNG_GenerateBlock(&rng, random, sizeof(random)) != 0) {
        wc_FreeRng(&rng);
        puts("MGTLSGUEST FAIL wolfcrypt-rng-generate");
        (void)mgtls_cleanup();
        return 1;
    }
    wc_FreeRng(&rng);
    if (!check_sha256()) { puts("MGTLSGUEST FAIL sha256"); return 1; }
    if (!check_aes_gcm()) { puts("MGTLSGUEST FAIL aes-gcm"); return 1; }
    if (!check_x25519()) { puts("MGTLSGUEST FAIL x25519"); return 1; }
    if (mgtls_cleanup() != MG_OK) {
        puts("MGTLSGUEST FAIL cleanup");
        return 1;
    }
    puts("MGTLSGUEST PASS entropy sha256 aes-gcm x25519");
    (void)fflush(stdout);
    return 0;
}
