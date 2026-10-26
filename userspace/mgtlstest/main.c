/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/tls.h>
#include <mg/net.h>

#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/curve25519.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "resource_trace.h"

#include <stdio.h>
#include <string.h>

#define MGTLS_GUEST_TLS_TIMEOUT 15000U

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

static int tls_smoke(void)
{
    static const unsigned char request[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    unsigned char response[32];
    usize sent_total = 0;
    usize received_total = 0;
    usize peak_heap_bytes = 0;
    usize live_heap_bytes = 0;
    usize peak_stack_bytes = 0;
    uintptr_t stack_mark;
    mgtls_context_t *context = NULL;
    mgtls_session_t *session = NULL;
    mg_result_t result;
    int success = 0;

    /* Initialize redirected stdout before enabling the allocation trace so
     * the TLS heap watermark excludes its persistent stdio buffer. */
    puts("MGTLSGUEST START tls13");
    (void)fflush(stdout);
    stack_mark = mgtlstest_stack_mark();
    mgtlstest_heap_trace_start();
    result = mgtls_init();

    if (result != MG_OK) {
        printf("MGTLSGUEST FAIL tls-init %lld\n", (long long)result);
        goto done;
    }
    result = mgtls_context_create(&context);
    if (result != MG_OK) {
        printf("MGTLSGUEST FAIL tls-context %lld\n", (long long)result);
        goto done;
    }
    result = mgtls_session_connect(context, "example.com", 443U,
                                   MGTLS_GUEST_TLS_TIMEOUT, &session);
    if (result != MG_OK) {
        printf("MGTLSGUEST FAIL tls-connect %lld\n", (long long)result);
        goto done;
    }
    while (sent_total < sizeof(request) - 1U) {
        usize sent = 0;
        result = mgtls_session_send(session, request + sent_total,
            sizeof(request) - 1U - sent_total, MGTLS_GUEST_TLS_TIMEOUT,
            &sent);
        if (result != MG_OK || sent == 0U) {
            printf("MGTLSGUEST FAIL tls-send %lld\n", (long long)result);
            goto done;
        }
        sent_total += sent;
    }
    while (received_total < 9U) {
        usize received = 0;
        result = mgtls_session_receive(session, response + received_total,
            sizeof(response) - received_total, MGTLS_GUEST_TLS_TIMEOUT,
            &received);
        if (result != MG_OK || received == 0U) {
            printf("MGTLSGUEST FAIL tls-receive %lld\n", (long long)result);
            goto done;
        }
        received_total += received;
    }
    if (memcmp(response, "HTTP/1.1 ", 9U) != 0 &&
        memcmp(response, "HTTP/1.0 ", 9U) != 0) {
        puts("MGTLSGUEST FAIL tls-response");
        goto done;
    }
    result = mgtls_session_shutdown(session, 1000U);
    if (result != MG_OK) {
        printf("MGTLSGUEST FAIL tls-shutdown %lld\n", (long long)result);
        goto done;
    }
    puts("MGTLSGUEST PASS tls13 authenticated example.com encrypted-http");
    success = 1;

done:
    if (session) {
        mg_result_t close_result = mgtls_session_close(session);
        if (success && close_result != MG_OK) {
            printf("MGTLSGUEST FAIL tls-close %lld\n",
                   (long long)close_result);
            success = 0;
        }
    }
    if (context) {
        mg_result_t destroy_result = mgtls_context_destroy(context);
        if (success && destroy_result != MG_OK) {
            printf("MGTLSGUEST FAIL tls-context-close %lld\n",
                   (long long)destroy_result);
            success = 0;
        }
    }
    result = mgtls_cleanup();
    if (success && result != MG_OK) {
        printf("MGTLSGUEST FAIL tls-cleanup %lld\n", (long long)result);
        success = 0;
    }
    peak_stack_bytes = mgtlstest_stack_peak_bytes(stack_mark);
    mgtlstest_heap_trace_stop(&peak_heap_bytes, &live_heap_bytes);
    printf("MGTLSGUEST RESOURCE stack_high_water_bytes=%llu "
           "peak_heap_bytes=%llu live_heap_bytes=%llu\n",
           (unsigned long long)peak_stack_bytes,
           (unsigned long long)peak_heap_bytes,
           (unsigned long long)live_heap_bytes);
    (void)fflush(stdout);
    /* The guest runner validates this exact transcript after the shell has
     * reached its shutdown command. Returning normally lets failed TLS runs
     * preserve that failure record and stop the disposable guest cleanly. */
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char random[64];
    WC_RNG rng;
    if (argc == 2 && strcmp(argv[1], "--tls") == 0)
        return tls_smoke();
    if (argc != 1) return 2;
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
