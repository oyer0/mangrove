/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <utf8.h>

void utf8_decoder_init(utf8_decoder_t *decoder)
{
    mg_utf8_state_reset(decoder);
}

static void append(utf8_decode_result_t *result, u32 scalar)
{
    if (result->count < UTF8_DECODE_OUTPUT_MAX)
        result->codepoints[result->count++] = scalar;
}

utf8_decode_result_t utf8_decoder_feed(utf8_decoder_t *decoder, u8 byte)
{
    utf8_decode_result_t result = { { 0U, 0U }, 0U };
    mg_utf8_feed_result_t decoded;

    if (!decoder) {
        append(&result, UTF8_REPLACEMENT_CODEPOINT);
        return result;
    }
    decoded = mg_utf8_state_feed(decoder, byte);
    if (decoded.status == MG_UTF8_OK) {
        append(&result, decoded.scalar);
    } else if (decoded.status == MG_UTF8_INVALID) {
        append(&result, UTF8_REPLACEMENT_CODEPOINT);
        if (!decoded.consumed) {
            decoded = mg_utf8_state_feed(decoder, byte);
            if (decoded.status == MG_UTF8_OK) append(&result, decoded.scalar);
            else if (decoded.status == MG_UTF8_INVALID)
                append(&result, UTF8_REPLACEMENT_CODEPOINT);
        }
    }
    return result;
}
