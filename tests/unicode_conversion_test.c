/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <limits.h>
#include <mg/utf8.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int check(bool condition, int code)
{
    return condition ? 0 : code;
}

static int test_vectors(void)
{
    static const u8 sequences[][4] = {
        { 0x00U, 0U, 0U, 0U },
        { 0x24U, 0U, 0U, 0U },
        { 0xc2U, 0xa2U, 0U, 0U },
        { 0xe2U, 0x82U, 0xacU, 0U },
        { 0xf0U, 0x9fU, 0x98U, 0x80U }
    };
    static const usize lengths[] = { 1U, 1U, 2U, 3U, 4U };
    static const u32 scalars[] = { 0U, 0x24U, 0xa2U, 0x20acU, 0x1f600U };
    static const u8 invalid[][4] = {
        { 0xc0U, 0x80U, 0U, 0U },
        { 0xe0U, 0x80U, 0U, 0U },
        { 0xedU, 0xa0U, 0x80U, 0U },
        { 0xf0U, 0x80U, 0x80U, 0x80U },
        { 0xf4U, 0x90U, 0x80U, 0x80U },
        { 0x80U, 0U, 0U, 0U },
        { 0xf5U, 0x80U, 0x80U, 0x80U }
    };
    static const usize invalid_lengths[] = { 2U, 2U, 3U, 4U, 4U, 1U, 4U };

    for (usize index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
        u32 scalar = 0xffffffffU;
        usize consumed = 0;
        u8 encoded[4] = { 0U, 0U, 0U, 0U };
        usize produced = 0;
        if (mg_utf8_decode_one(sequences[index], lengths[index], &scalar,
                               &consumed) != MG_UTF8_OK ||
            scalar != scalars[index] || consumed != lengths[index] ||
            mg_utf8_encode_one(scalar, encoded, sizeof(encoded), &produced) !=
                MG_UTF8_OK || produced != lengths[index] ||
            memcmp(encoded, sequences[index], produced) != 0)
            return 10 + (int)index;
        for (usize prefix = 1U; prefix < lengths[index]; prefix++) {
            usize partial_consumed = 99U;
            if (mg_utf8_decode_one(sequences[index], prefix, &scalar,
                                   &partial_consumed) != MG_UTF8_INCOMPLETE ||
                partial_consumed != 0U)
                return 15 + (int)index;
        }
    }
    if (!mg_utf8_validate(sequences[0], 1U)) return 20;
    for (usize index = 0; index < sizeof(invalid_lengths) /
                                sizeof(invalid_lengths[0]); index++) {
        u32 scalar = 0U;
        usize consumed = 0;
        if (mg_utf8_decode_one(invalid[index], invalid_lengths[index], &scalar,
                               &consumed) != MG_UTF8_INVALID || consumed != 1U ||
            mg_utf8_validate(invalid[index], invalid_lengths[index]))
            return 21 + (int)index;
    }
    {
        static const u8 bad_prefix[][3] = {
            { 0xe2U, 0U, 0U }, { 0xe2U, 0x41U, 0U },
            { 0xe0U, 0x80U, 0U }, { 0xf4U, 0x90U, 0U }
        };
        static const usize bad_prefix_lengths[] = { 1U, 2U, 2U, 2U };
        for (usize index = 0; index < 4U; index++) {
            u32 scalar;
            usize consumed;
            mg_utf8_result_t status = mg_utf8_decode_one(
                bad_prefix[index], bad_prefix_lengths[index], &scalar,
                &consumed);
            if ((index == 0U && status != MG_UTF8_INCOMPLETE) ||
                (index != 0U && status != MG_UTF8_INVALID))
                return 30 + (int)index;
        }
    }
    for (u32 scalar = 0U; scalar <= 0x10ffffU; scalar++) {
        u8 encoded[4];
        usize produced, consumed;
        u32 decoded;
        if (!mg_utf8_is_scalar(scalar)) continue;
        if (mg_utf8_encode_one(scalar, encoded, sizeof(encoded), &produced) !=
                MG_UTF8_OK ||
            mg_utf8_decode_one(encoded, produced, &decoded, &consumed) !=
                MG_UTF8_OK || decoded != scalar || consumed != produced)
            return 40;
    }
    {
        u8 output[4] = { 0x55U, 0x55U, 0x55U, 0x55U };
        usize produced = 99U;
        if (mg_utf8_encode_one(0x20acU, output, 2U, &produced) !=
                MG_UTF8_NO_SPACE || produced != 0U || output[0] != 0x55U ||
            mg_utf8_encode_one(0xd800U, output, sizeof(output), &produced) !=
                MG_UTF8_INVALID || produced != 0U)
            return 41;
    }
    {
        static const u8 malformed_prefix[] = { 0xe2U, 0x80U, 'A' };
        static const u8 incomplete_suffix[] = { 0xf0U, 0x9fU };
        u32 scalar = 0U;
        usize consumed = 0U;
        if (mg_utf8_decode_display_one(malformed_prefix,
                                       sizeof(malformed_prefix), &scalar,
                                       &consumed) != MG_UTF8_INVALID ||
            scalar != 0xfffdU || consumed != 2U ||
            mg_utf8_decode_display_one(incomplete_suffix,
                                       sizeof(incomplete_suffix), &scalar,
                                       &consumed) != MG_UTF8_INVALID ||
            scalar != 0xfffdU || consumed != sizeof(incomplete_suffix))
            return 42;
    }
    return 0;
}

static int test_restartable_conversions(void)
{
    mbstate_t state = { 0 };
    wchar_t value = 0x55;
    static const char euro[] = { (char)0xe2, (char)0x82, (char)0xac };
    static const char bad[] = { (char)0xe0, (char)0x80 };
    char encoded[5] = { 0 };
    size_t result;

    if (check(mbsinit(&state), 50)) return 50;
    result = mbrtowc(&value, euro, 1U, &state);
    if (result != (size_t)-2 || value != 0x55 || mbsinit(&state)) return 51;
    result = mbrtowc(&value, euro + 1, 2U, &state);
    if (result != 2U || value != 0x20ac || !mbsinit(&state)) return 52;
    if (mbrtowc(&value, euro, 0U, &state) != (size_t)-2) return 53;
    if (mbrtowc(&value, bad, sizeof(bad), &state) != (size_t)-1 ||
        errno != EILSEQ || !mbsinit(&state)) return 54;
    if (mbrtowc(&value, "\0x", 2U, &state) != 0U || value != L'\0' ||
        !mbsinit(&state)) return 55;
    if (wcrtomb(encoded, (wchar_t)0x20ac, &state) != 3U ||
        memcmp(encoded, euro, 3U) != 0) return 56;
    if (wcrtomb(encoded, (wchar_t)0xd800, &state) != (size_t)-1 ||
        errno != EILSEQ) return 57;
    if (mbrtowc(&value, euro, 1U, &state) != (size_t)-2 ||
        wcrtomb(encoded, L'A', &state) != (size_t)-1 || !mbsinit(&state))
        return 58;
    {
        static const struct {
            const char *bytes;
            size_t length;
            wchar_t scalar;
        } cases[] = {
            { "\xc2\xa2", 2U, (wchar_t)0xa2 },
            { "\xe2\x82\xac", 3U, (wchar_t)0x20ac },
            { "\xf0\x9f\x98\x80", 4U, (wchar_t)0x1f600 }
        };
        for (size_t item = 0; item < sizeof(cases) / sizeof(cases[0]); item++) {
            for (size_t split = 1U; split < cases[item].length; split++) {
                memset(&state, 0, sizeof(state));
                if (mbrtowc(&value, cases[item].bytes, split, &state) !=
                        (size_t)-2 || mbsinit(&state) ||
                    mbrtowc(&value, cases[item].bytes + split,
                            cases[item].length - split, &state) !=
                        cases[item].length - split ||
                    value != cases[item].scalar || !mbsinit(&state))
                    return 59;
            }
        }
    }
    memset(&state, 0, sizeof(state));
    if (mbrtowc(&value, "\xe2", 1U, &state) != (size_t)-2 ||
        mbrtowc(&value, "A", 1U, &state) != (size_t)-1 ||
        errno != EILSEQ || !mbsinit(&state) ||
        mbrtowc(&value, "A", 1U, &state) != 1U || value != L'A')
        return 60;
    {
        static const char invalid_then_ascii[] = { (char)0xe2, 'A', '\0' };
        const char *invalid_source = invalid_then_ascii;
        wchar_t invalid_output[2];
        if (mbsrtowcs(invalid_output, &invalid_source, 2U, &state) !=
                (size_t)-1 || errno != EILSEQ ||
            invalid_source != invalid_then_ascii || !mbsinit(&state))
            return 60;
    }
    if (wcrtomb(NULL, L'X', &state) != 1U || !mbsinit(&state)) return 61;
    if (mbsrtowcs(NULL, &(const char *){ "A\342\202\254" }, 0U,
                  &state) != 2U) return 62;
    if (mbstowcs(NULL, "A\342\202\254", 0U) != 2U ||
        wcstombs(NULL, L"A\u20ac", 0U) != 4U || mbsinit(NULL) != 1)
        return 63;
    return 0;
}

static int test_string_conversions(void)
{
    static const char input[] = "A\342\202\254";
    static const wchar_t wide[] = { L'A', (wchar_t)0x20ac, L'\0' };
    const char *source = input;
    const wchar_t *wide_source = wide;
    wchar_t output_wide[3] = { 0 };
    char output_bytes[5] = { 0 };
    mbstate_t state = { 0 };
    size_t result;

    result = mbsrtowcs(output_wide, &source, 1U, &state);
    if (result != 1U || source != input + 1U || output_wide[0] != L'A')
        return 60;
    result = mbsrtowcs(output_wide + 1, &source, 2U, &state);
    if (result != 1U || source != NULL || output_wide[1] != (wchar_t)0x20ac ||
        output_wide[2] != L'\0') return 61;
    source = input;
    memset(&state, 0, sizeof(state));
    if (mbsrtowcs(NULL, &source, 99U, &state) != 2U || source != input ||
        !mbsinit(&state)) return 62;

    result = wcsrtombs(output_bytes, &wide_source, 2U, &state);
    if (result != 1U || wide_source != wide + 1U || output_bytes[0] != 'A')
        return 63;
    result = wcsrtombs(output_bytes + 1, &wide_source, 4U, &state);
    if (result != 3U || wide_source != NULL ||
        memcmp(output_bytes, input, sizeof(input)) != 0) return 64;
    wide_source = wide;
    if (wcsrtombs(NULL, &wide_source, 0U, &state) != 4U ||
        wide_source != wide) return 65;
    if (mbstowcs(output_wide, input, 3U) != 2U ||
        wcscmp(output_wide, wide) != 0) return 66;
    if (wcstombs(output_bytes, wide, sizeof(output_bytes)) != 4U ||
        memcmp(output_bytes, input, sizeof(input)) != 0) return 67;
    if (mbtowc(output_wide, "\342\202\254", 3U) != 3 ||
        output_wide[0] != (wchar_t)0x20ac || mbtowc(NULL, NULL, 0U) != 0)
        return 68;
    if (mblen("\342", 1U) != -1 || mblen(NULL, 0U) != 0) return 69;
    if (btowc(0x41) != L'A' || btowc(0x80) != WEOF ||
        wctob((wint_t)0x41) != 0x41 || wctob((wint_t)0x80) != EOF)
        return 70;
    return 0;
}

static int test_wide_strings(void)
{
    wchar_t text[16] = L"A\u20ac";
    wchar_t copy[16];
    if (wcslen(text) != 2U || wcscmp(text, L"A\u20ac") != 0 ||
        wcsncmp(text, L"A\u20ac!", 2U) != 0 ||
        !wcschr(text, (wchar_t)0x20ac) || !wcsrchr(text, L'A') ||
        wcscspn(text, L"\u20ac") != 1U || wcsspn(text, L"A\u20ac") != 2U)
        return 80;
    if (!wcsstr(text, L"\u20ac") || !wcspbrk(text, L"Z\u20ac")) return 81;
    wmemcpy(copy, text, 3U);
    if (wmemcmp(copy, text, 3U) || wmemchr(copy, (wchar_t)0x20ac, 3U) !=
        copy + 1U) return 82;
    wmemmove(copy + 1, copy, 3U);
    if (wcscmp(copy + 1, text) != 0) return 83;
    wmemset(copy, L'x', 2U);
    if (copy[0] != L'x' || copy[1] != L'x') return 84;
    wcscpy(copy, L"wide");
    wcsncat(copy, L" text", 2U);
    if (wcscmp(copy, L"wide t") != 0) return 85;
    if (wcsxfrm(NULL, L"order", 0U) != 5U) return 86;
    return 0;
}

static int test_seeded_malformed_inputs(void)
{
    u32 state = 0x6d616e67U;
    for (u32 round = 0; round < 8192U; round++) {
        u8 bytes[8];
        usize length = 1U + round % sizeof(bytes);
        for (usize index = 0; index < length; index++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            bytes[index] = (u8)state;
        }
        usize offset = 0;
        while (offset < length) {
            usize consumed = 0;
            u32 scalar;
            mg_utf8_result_t result = mg_utf8_decode_one(
                bytes + offset, length - offset, &scalar, &consumed);
            if (result == MG_UTF8_OK) {
                u8 encoded[4];
                usize produced;
                if (!mg_utf8_is_scalar(scalar) || !consumed ||
                    mg_utf8_encode_one(scalar, encoded, sizeof(encoded),
                                       &produced) != MG_UTF8_OK ||
                    produced != consumed ||
                    memcmp(encoded, bytes + offset, consumed) != 0)
                    return 90;
                offset += consumed;
            } else if (result == MG_UTF8_INVALID) {
                if (consumed != 1U) return 91;
                offset++;
            } else {
                if (consumed != 0U) return 92;
                break;
            }
        }
    }
    return 0;
}

int main(void)
{
    int result;
    if ((result = test_vectors()) != 0) return result;
    if ((result = test_restartable_conversions()) != 0) return result;
    if ((result = test_string_conversions()) != 0) return result;
    if ((result = test_wide_strings()) != 0) return result;
    return test_seeded_malformed_inputs();
}
