/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <entropy.h>

#include <entropy_platform.h>

static void entropy_clear(void *buffer, usize length)
{
    volatile u8 *bytes = (volatile u8 *)buffer;
    while (bytes && length--) *bytes++ = 0;
}

bool entropy_available(void)
{
    return entropy_platform_source_supported(ENTROPY_SOURCE_RDSEED) ||
           entropy_platform_source_supported(ENTROPY_SOURCE_RDRAND);
}

bool entropy_random_u64(u64 *value)
{
    u64 candidate = 0;
    entropy_word_result_t result;

    if (!value) return false;
    *value = 0;

    /* RDSEED is preferred because it obtains seed material from the hardware
     * entropy source. RDRAND is the defined fallback when RDSEED is absent or
     * persistently reports that it has no fresh word ready. */
    for (u32 attempt = 0; attempt < 16U; attempt++) {
        result = entropy_platform_read_word(ENTROPY_SOURCE_RDSEED,
                                            &candidate);
        if (result == ENTROPY_WORD_READY) {
            *value = candidate;
            entropy_clear(&candidate, sizeof(candidate));
            return true;
        }
        if (result == ENTROPY_WORD_UNSUPPORTED) break;
    }
    for (u32 attempt = 0; attempt < 16U; attempt++) {
        result = entropy_platform_read_word(ENTROPY_SOURCE_RDRAND,
                                            &candidate);
        if (result == ENTROPY_WORD_READY) {
            *value = candidate;
            entropy_clear(&candidate, sizeof(candidate));
            return true;
        }
        if (result == ENTROPY_WORD_UNSUPPORTED) break;
    }
    entropy_clear(&candidate, sizeof(candidate));
    return false;
}

bool entropy_random_bytes(u8 *output, usize length)
{
    if (length == 0U) return true;
    if (!output) return false;

    for (usize offset = 0; offset < length;) {
        u64 word = 0;
        usize amount = length - offset;
        if (amount > sizeof(word)) amount = sizeof(word);
        if (!entropy_random_u64(&word)) {
            entropy_clear(output, length);
            entropy_clear(&word, sizeof(word));
            return false;
        }
        for (usize index = 0; index < amount; index++)
            output[offset + index] = (u8)(word >> (index * 8U));
        entropy_clear(&word, sizeof(word));
        offset += amount;
    }
    return true;
}
