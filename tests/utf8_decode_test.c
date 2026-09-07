/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <utf8.h>

static int expect_sequence(const u8 *bytes, u64 length,
                           const u32 *expected, u64 expected_count)
{
    utf8_decoder_t decoder;
    u32 output[16];
    u64 output_count = 0;

    utf8_decoder_init(&decoder);
    for (u64 index = 0; index < length; index++) {
        utf8_decode_result_t result = utf8_decoder_feed(&decoder, bytes[index]);
        for (u8 item = 0; item < result.count; item++) {
            if (output_count >= sizeof(output) / sizeof(output[0])) return 1;
            output[output_count++] = result.codepoints[item];
        }
    }
    if (output_count != expected_count) return 1;
    for (u64 index = 0; index < expected_count; index++) {
        if (output[index] != expected[index]) return 1;
    }
    return 0;
}

static int expect_split_box(void)
{
    utf8_decoder_t decoder;
    utf8_decode_result_t result;

    utf8_decoder_init(&decoder);
    result = utf8_decoder_feed(&decoder, 0xE2U);
    if (result.count != 0) return 1;
    result = utf8_decoder_feed(&decoder, 0x94U);
    if (result.count != 0) return 1;
    result = utf8_decoder_feed(&decoder, 0x9CU);
    return result.count == 1 && result.codepoints[0] == 0x251CU ? 0 : 1;
}

int main(void)
{
    static const u8 ascii[] = { 0x24 };
    static const u32 ascii_expected[] = { 0x24 };
    static const u8 two_byte[] = { 0xC2, 0xA2 };
    static const u32 two_byte_expected[] = { 0xA2 };
    static const u8 three_byte[] = { 0xE2, 0x94, 0x9C };
    static const u32 three_byte_expected[] = { 0x251C };
    static const u8 four_byte[] = { 0xF0, 0x9F, 0x98, 0x80 };
    static const u32 four_byte_expected[] = { 0x1F600 };
    static const u8 lone_continuation[] = { 0x80 };
    static const u32 replacement[] = { UTF8_REPLACEMENT_CODEPOINT };
    static const u8 truncated_two[] = { 0xC2, 0x41 };
    static const u32 truncated_two_expected[] = {
        UTF8_REPLACEMENT_CODEPOINT, 0x41
    };
    static const u8 truncated_three[] = { 0xE2, 0x82, 0x42 };
    static const u32 truncated_three_expected[] = {
        UTF8_REPLACEMENT_CODEPOINT, 0x42
    };
    static const u8 truncated_four[] = { 0xF0, 0x9F, 0x98, 0x43 };
    static const u32 truncated_four_expected[] = {
        UTF8_REPLACEMENT_CODEPOINT, 0x43
    };
    static const u8 overlong[] = { 0xE0, 0x80, 0x80 };
    static const u8 surrogate[] = { 0xED, 0xA0, 0x80 };
    static const u8 above_maximum[] = { 0xF4, 0x90, 0x80, 0x80 };
    static const u8 invalid_then_ascii[] = { 0xFF, 0x4F, 0x4B };
    static const u32 invalid_then_ascii_expected[] = {
        UTF8_REPLACEMENT_CODEPOINT, 0x4F, 0x4B
    };

    if (expect_sequence(ascii, sizeof(ascii), ascii_expected,
                        sizeof(ascii_expected) / sizeof(ascii_expected[0])) ||
        expect_sequence(two_byte, sizeof(two_byte), two_byte_expected,
                        sizeof(two_byte_expected) / sizeof(two_byte_expected[0])) ||
        expect_sequence(three_byte, sizeof(three_byte), three_byte_expected,
                        sizeof(three_byte_expected) / sizeof(three_byte_expected[0])) ||
        expect_sequence(four_byte, sizeof(four_byte), four_byte_expected,
                        sizeof(four_byte_expected) / sizeof(four_byte_expected[0])) ||
        expect_sequence(lone_continuation, sizeof(lone_continuation), replacement, 1) ||
        expect_sequence(truncated_two, sizeof(truncated_two),
                        truncated_two_expected,
                        sizeof(truncated_two_expected) / sizeof(truncated_two_expected[0])) ||
        expect_sequence(truncated_three, sizeof(truncated_three),
                        truncated_three_expected,
                        sizeof(truncated_three_expected) / sizeof(truncated_three_expected[0])) ||
        expect_sequence(truncated_four, sizeof(truncated_four),
                        truncated_four_expected,
                        sizeof(truncated_four_expected) / sizeof(truncated_four_expected[0])) ||
        expect_sequence(overlong, sizeof(overlong), replacement, 1) ||
        expect_sequence(surrogate, sizeof(surrogate), replacement, 1) ||
        expect_sequence(above_maximum, sizeof(above_maximum), replacement, 1) ||
        expect_sequence(invalid_then_ascii, sizeof(invalid_then_ascii),
                        invalid_then_ascii_expected,
                        sizeof(invalid_then_ascii_expected) /
                        sizeof(invalid_then_ascii_expected[0])) ||
        expect_split_box())
        return 1;
    return 0;
}
