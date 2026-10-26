/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <crypto/hmac_drbg_sha256.h>
#include <crypto/sha256.h>
#include <csprng.h>
#include <entropy.h>
#include <entropy_platform.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool rdseed_supported;
static bool rdrand_supported;
static bool rdseed_ready;
static bool rdrand_ready;
static unsigned rdseed_reads;
static unsigned rdrand_reads;
static u64 hardware_word = 0x0123456789abcdefULL;

bool entropy_platform_source_supported(entropy_source_t source)
{
    if (source == ENTROPY_SOURCE_RDSEED) return rdseed_supported;
    if (source == ENTROPY_SOURCE_RDRAND) return rdrand_supported;
    return false;
}

entropy_word_result_t entropy_platform_read_word(entropy_source_t source,
                                                 u64 *value)
{
    bool ready;
    if (source == ENTROPY_SOURCE_RDSEED) {
        rdseed_reads++;
        ready = rdseed_ready;
    } else if (source == ENTROPY_SOURCE_RDRAND) {
        rdrand_reads++;
        ready = rdrand_ready;
    } else {
        return ENTROPY_WORD_UNSUPPORTED;
    }
    if (!entropy_platform_source_supported(source))
        return ENTROPY_WORD_UNSUPPORTED;
    if (!ready) return ENTROPY_WORD_NOT_READY;
    *value = hardware_word;
    hardware_word += 0x9e3779b97f4a7c15ULL;
    return ENTROPY_WORD_READY;
}

static u8 hex_digit(char value)
{
    if (value >= '0' && value <= '9') return (u8)(value - '0');
    assert(value >= 'a' && value <= 'f');
    return (u8)(value - 'a' + 10);
}

static void unhex(const char *text, u8 *output, usize length)
{
    for (usize index = 0; index < length; index++)
        output[index] = (u8)((hex_digit(text[index * 2U]) << 4) |
                             hex_digit(text[index * 2U + 1U]));
}

static void test_hmac_drbg_vector(void)
{
    /* NIST CAVP HMAC_DRBG.rsp, SHA-256, no prediction resistance, Count 0.
     * EntropyInputReseed is followed by two Generate calls; the first output
     * is discarded as prescribed by the vector test procedure. */
    static const char expected_hex[] =
        "76fc79fe9b50beccc991a11b5635783a83536add03c157fb30645e611c2898bb"
        "2b1bc215000209208cd506cb28da2a51bdb03826aaf2bd2335d576d519160842"
        "e7158ad0949d1a9ec3e66ea1b1a064b005de914eac2e9d4f2d72a8616a802254"
        "22918250ff66a41bd2f864a6a38cc5b6499dc43f7f2bd09e1e0f8f5885935124";
    u8 entropy[32], nonce[16], reseed[32], discarded[128], output[128];
    u8 expected[128];
    hmac_drbg_sha256_t state;

    unhex("06032cd5eed33f39265f49ecb142c511da9aff2af71203bffaf34a9ca5bd9c0d",
          entropy, sizeof(entropy));
    unhex("0e66f71edc43e42a45ad3c6fc6cdc4df", nonce, sizeof(nonce));
    unhex("01920a4e669ed3a85ae8a33b35a74ad7fb2a6bb4cf395ce00334a9c9a5a5d552",
          reseed, sizeof(reseed));
    unhex(expected_hex, expected, sizeof(expected));

    assert(hmac_drbg_sha256_instantiate(&state, entropy, sizeof(entropy),
                                        nonce, sizeof(nonce)));
    assert(hmac_drbg_sha256_reseed(&state, reseed, sizeof(reseed)));
    assert(hmac_drbg_sha256_generate(&state, discarded, sizeof(discarded)));
    assert(hmac_drbg_sha256_generate(&state, output, sizeof(output)));
    assert(memcmp(output, expected, sizeof(expected)) == 0);
    assert(!hmac_drbg_sha256_generate(&state, output,
                                      HMAC_DRBG_SHA256_MAX_REQUEST_BYTES + 1U));
    hmac_drbg_sha256_clear(&state);
    for (usize index = 0; index < sizeof(state); index++)
        assert(((const u8 *)&state)[index] == 0);
}

static void test_entropy_selection(void)
{
    u64 word = 99;
    u8 output[24];

    rdseed_supported = false;
    rdrand_supported = false;
    rdseed_reads = rdrand_reads = 0;
    assert(!entropy_available());
    assert(!entropy_random_u64(&word) && word == 0);
    memset(output, 0xa5, sizeof(output));
    assert(!entropy_random_bytes(output, sizeof(output)));
    for (usize index = 0; index < sizeof(output); index++) assert(!output[index]);

    rdseed_supported = true;
    rdrand_supported = true;
    rdseed_ready = false;
    rdrand_ready = true;
    rdseed_reads = rdrand_reads = 0;
    assert(entropy_available());
    assert(entropy_random_u64(&word));
    assert(rdseed_reads == 16U && rdrand_reads == 1U);

    rdseed_ready = true;
    rdseed_reads = rdrand_reads = 0;
    assert(entropy_random_u64(&word));
    assert(rdseed_reads == 1U && rdrand_reads == 0U);

    rdseed_supported = false;
    rdseed_reads = rdrand_reads = 0;
    assert(entropy_random_u64(&word));
    assert(rdseed_reads == 1U && rdrand_reads == 1U);
}

static void *thread_fill(void *argument)
{
    u8 *buffer = (u8 *)argument;
    assert(csprng_fill(buffer, 65536U) == MG_OK);
    return NULL;
}

static bool all_zero(const u8 *buffer, usize length)
{
    for (usize index = 0; index < length; index++)
        if (buffer[index]) return false;
    return true;
}

static void test_generator_lifecycle(void)
{
    const usize large_length = 2U * 1024U * 1024U;
    const usize failure_length = 256U * 10000U;
    u8 *large = malloc(failure_length);
    pthread_t threads[8];
    u8 *thread_buffers[8];

    assert(large);
    rdseed_supported = true;
    rdrand_supported = true;
    rdseed_ready = false;
    rdrand_ready = false;
    assert(csprng_initialize() == MG_ERR_ENTROPY_UNAVAILABLE);
    assert(csprng_status() == MG_ERR_ENTROPY_UNAVAILABLE);
    rdseed_ready = true;
    assert(csprng_status() == MG_OK);
    assert(csprng_fill(NULL, 0) == MG_OK);
    assert(csprng_fill(NULL, 1) == MG_ERR_BAD_ARGUMENT);
    assert(csprng_fill(large, large_length) == MG_OK);
    assert(!all_zero(large, large_length));

    /* The service emits 256-byte DRBG requests, so this reaches the configured
     * reseed boundary. An entropy failure must fail the next request closed. */
    rdseed_ready = false;
    rdrand_ready = false;
    memset(large, 0xa5, failure_length);
    assert(csprng_fill(large, failure_length) ==
           MG_ERR_ENTROPY_UNAVAILABLE);
    assert(memcmp(large, (u8[256]){[0 ... 255] = 0xa5}, 256U) != 0);
    for (usize index = failure_length - 64U; index < failure_length; index++)
        assert(large[index] == 0xa5);
    assert(csprng_status() == MG_ERR_ENTROPY_UNAVAILABLE);
    rdseed_ready = true;
    assert(csprng_status() == MG_OK);

    for (unsigned index = 0; index < 8U; index++) {
        thread_buffers[index] = malloc(65536U);
        assert(thread_buffers[index]);
        assert(pthread_create(&threads[index], NULL, thread_fill,
                              thread_buffers[index]) == 0);
    }
    for (unsigned index = 0; index < 8U; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
        assert(!all_zero(thread_buffers[index], 65536U));
        for (unsigned other = 0; other < index; other++)
            assert(memcmp(thread_buffers[index], thread_buffers[other],
                          65536U) != 0);
    }
    for (unsigned index = 0; index < 8U; index++)
        free(thread_buffers[index]);
    free(large);
}

int main(void)
{
    test_hmac_drbg_vector();
    test_entropy_selection();
    test_generator_lifecycle();
    puts("random DRBG, entropy failure, reseed, large request and concurrency tests passed");
    return 0;
}
