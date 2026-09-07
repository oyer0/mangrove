#include <utf8.h>

static void utf8_result_append(utf8_decode_result_t *result, u32 codepoint)
{
    if (result->count >= UTF8_DECODE_OUTPUT_MAX) return;
    result->codepoints[result->count++] = codepoint;
}
static void utf8_decoder_reset(utf8_decoder_t *decoder)
{
    decoder->value = 0;
    decoder->minimum = 0;
    decoder->remaining = 0;
}

static void utf8_start(utf8_decoder_t *decoder, u8 byte)
{
    if (byte >= 0xC2U && byte <= 0xDFU) {
        decoder->value = byte & 0x1FU;
        decoder->minimum = 0x80U;
        decoder->remaining = 1;
    } else if (byte >= 0xE0U && byte <= 0xEFU) {
        decoder->value = byte & 0x0FU;
        decoder->minimum = 0x800U;
        decoder->remaining = 2;
    } else if (byte >= 0xF0U && byte <= 0xF4U) {
        decoder->value = byte & 0x07U;
        decoder->minimum = 0x10000U;
        decoder->remaining = 3;
    }
}

void utf8_decoder_init(utf8_decoder_t *decoder)
{
    if (!decoder) return;
    utf8_decoder_reset(decoder);
}

utf8_decode_result_t utf8_decoder_feed(utf8_decoder_t *decoder, u8 byte)
{
    utf8_decode_result_t result = { { 0, 0 }, 0 };

    if (!decoder) {
        utf8_result_append(&result, UTF8_REPLACEMENT_CODEPOINT);
        return result;
    }

    if (decoder->remaining != 0) {
        if ((byte & 0xC0U) != 0x80U) {
            /* The sequence is broken.  Emit one replacement, then process an
             * ASCII byte or a new valid lead byte without losing it. */
            utf8_decoder_reset(decoder);
            utf8_result_append(&result, UTF8_REPLACEMENT_CODEPOINT);
            if (byte < 0x80U) {
                utf8_result_append(&result, byte);
            } else if (byte >= 0xC2U && byte <= 0xF4U) {
                utf8_start(decoder, byte);
            } else {
                utf8_result_append(&result, UTF8_REPLACEMENT_CODEPOINT);
            }
            return result;
        }

        decoder->value = (decoder->value << 6) | (byte & 0x3FU);
        decoder->remaining--;
        if (decoder->remaining != 0) return result;

        if (decoder->value < decoder->minimum ||
            decoder->value > 0x10FFFFU ||
            (decoder->value >= 0xD800U && decoder->value <= 0xDFFFU)) {
            utf8_result_append(&result, UTF8_REPLACEMENT_CODEPOINT);
        } else {
            utf8_result_append(&result, decoder->value);
        }
        utf8_decoder_reset(decoder);
        return result;
    }

    if (byte < 0x80U) {
        utf8_result_append(&result, byte);
        return result;
    }

    if (byte >= 0xC2U && byte <= 0xF4U) {
        utf8_start(decoder, byte);
        return result;
    }

    /* Continuation bytes, C0/C1, and F5/FF are never valid lead bytes. */
    utf8_result_append(&result, UTF8_REPLACEMENT_CODEPOINT);
    return result;
}
