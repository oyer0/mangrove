/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "floatconv.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BIG_LIMBS 2304U
#define DECIMAL_KEEP_DIGITS 20000U
#define HEX_KEEP_DIGITS 128U
#define DECIMAL_TEXT_CAPACITY 22016U

_Static_assert(sizeof(long double) == 16 && LDBL_MANT_DIG == 64 &&
               LDBL_MAX_EXP == 16384,
               "float conversion requires x86-64 extended long double");

typedef struct {
    uint32_t words[BIG_LIMBS];
    size_t length;
} big_uint_t;

typedef struct {
    uint64_t significand;
    int64_t exponent2;
    bool negative;
    bool zero;
    bool infinity;
    bool nan;
    bool subnormal;
    int precision;
    int minimum_exponent;
} float_parts_t;

typedef struct {
    int64_t exponent2;
    int64_t decimal_exponent;
    size_t significant_digits;
    bool decimal;
    bool sticky;
    bool negative;
    bool any;
    bool overflow;
    bool underflow;
    bool infinity;
    bool nan;
    const char *end;
} parsed_float_t;

typedef struct {
    big_uint_t source;
    big_uint_t numerator;
    big_uint_t denominator;
    big_uint_t scratch_a;
    big_uint_t scratch_b;
} float_parse_workspace_t;

typedef struct {
    big_uint_t integer;
    big_uint_t remainder;
    big_uint_t denominator;
    char exact[DECIMAL_TEXT_CAPACITY];
    char digits[DECIMAL_TEXT_CAPACITY];
} float_format_workspace_t;

static int ascii_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool ascii_space(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int ascii_match(const char *text, const char *word)
{
    int count = 0;
    while (word[count] && ascii_lower((unsigned char)text[count]) ==
                           (unsigned char)word[count]) count++;
    return word[count] ? 0 : count;
}

static void big_normalize(big_uint_t *value)
{
    while (value->length && value->words[value->length - 1U] == 0U)
        value->length--;
}

static void big_zero(big_uint_t *value)
{
    value->length = 0;
}

static bool big_set_u64(big_uint_t *value, uint64_t number)
{
    value->words[0] = (uint32_t)number;
    value->words[1] = (uint32_t)(number >> 32);
    value->length = value->words[1] ? 2U : value->words[0] ? 1U : 0U;
    return true;
}

static bool big_set_bit(big_uint_t *value, size_t bit)
{
    size_t word = bit / 32U;
    if (word >= BIG_LIMBS) return false;
    memset(value->words, 0, (word + 1U) * sizeof(value->words[0]));
    value->words[word] = (uint32_t)1U << (bit % 32U);
    value->length = word + 1U;
    return true;
}

static bool big_mul_small(big_uint_t *value, uint32_t factor)
{
    uint64_t carry = 0;
    for (size_t i = 0; i < value->length; i++) {
        uint64_t product = (uint64_t)value->words[i] * factor + carry;
        value->words[i] = (uint32_t)product;
        carry = product >> 32;
    }
    if (carry) {
        if (value->length == BIG_LIMBS) return false;
        value->words[value->length++] = (uint32_t)carry;
    }
    return true;
}

static bool big_add_small(big_uint_t *value, uint32_t addend)
{
    uint64_t carry = addend;
    size_t i = 0;
    while (carry && i < value->length) {
        uint64_t sum = (uint64_t)value->words[i] + carry;
        value->words[i++] = (uint32_t)sum;
        carry = sum >> 32;
    }
    if (carry) {
        if (value->length == BIG_LIMBS) return false;
        value->words[value->length++] = (uint32_t)carry;
    } else if (!value->length && addend) {
        value->words[0] = addend;
        value->length = 1U;
    }
    return true;
}

static bool big_mul_add(big_uint_t *value, uint32_t factor, uint32_t addend)
{
    return big_mul_small(value, factor) && big_add_small(value, addend);
}

static unsigned int leading_zeroes(uint32_t value)
{
    return value ? (unsigned int)__builtin_clz(value) : 32U;
}

static size_t big_bit_length(const big_uint_t *value)
{
    return value->length
        ? (value->length - 1U) * 32U +
          32U - leading_zeroes(value->words[value->length - 1U])
        : 0U;
}

static int big_compare(const big_uint_t *left, const big_uint_t *right)
{
    if (left->length != right->length)
        return left->length < right->length ? -1 : 1;
    for (size_t i = left->length; i; i--) {
        if (left->words[i - 1U] != right->words[i - 1U])
            return left->words[i - 1U] < right->words[i - 1U] ? -1 : 1;
    }
    return 0;
}

static uint32_t big_shifted_word(const big_uint_t *value, size_t shift,
                                size_t output_word)
{
    size_t whole = shift / 32U;
    unsigned int part = (unsigned int)(shift % 32U);
    if (output_word < whole) return 0;
    size_t source = output_word - whole;
    uint64_t result = source < value->length
        ? (uint64_t)value->words[source] << part : 0U;
    if (part && source && source - 1U < value->length)
        result |= value->words[source - 1U] >> (32U - part);
    return (uint32_t)result;
}

static size_t big_shifted_length(const big_uint_t *value, size_t shift)
{
    size_t bits = big_bit_length(value);
    return bits ? (bits + shift + 31U) / 32U : 0U;
}

static int big_compare_shifted(const big_uint_t *left, const big_uint_t *right,
                               size_t right_shift)
{
    size_t left_bits = big_bit_length(left);
    size_t right_bits = big_bit_length(right);
    if (right_bits && right_shift > SIZE_MAX - right_bits) return -1;
    right_bits += right_shift;
    if (left_bits != right_bits) return left_bits < right_bits ? -1 : 1;
    size_t words = left->length;
    for (size_t i = words; i; i--) {
        uint32_t b = big_shifted_word(right, right_shift, i - 1U);
        if (left->words[i - 1U] != b)
            return left->words[i - 1U] < b ? -1 : 1;
    }
    return 0;
}

static bool big_shift_left(big_uint_t *value, size_t shift)
{
    if (!value->length || !shift) return true;
    size_t whole = shift / 32U;
    unsigned int part = (unsigned int)(shift % 32U);
    size_t needed = value->length + whole + (part ? 1U : 0U);
    if (needed > BIG_LIMBS) return false;
    if (whole) {
        for (size_t i = value->length; i; i--)
            value->words[i - 1U + whole] = value->words[i - 1U];
        memset(value->words, 0, whole * sizeof(value->words[0]));
        value->length += whole;
    }
    if (part) {
        uint32_t carry = 0;
        for (size_t i = whole; i < value->length; i++) {
            uint64_t expanded = ((uint64_t)value->words[i] << part) | carry;
            value->words[i] = (uint32_t)expanded;
            carry = (uint32_t)(expanded >> 32);
        }
        if (carry) value->words[value->length++] = carry;
    }
    return true;
}

static bool big_sub_shifted(big_uint_t *left, const big_uint_t *right,
                            size_t right_shift)
{
    if (big_compare_shifted(left, right, right_shift) < 0) return false;
    size_t shifted_length = big_shifted_length(right, right_shift);
    uint64_t borrow = 0;
    for (size_t i = 0; i < shifted_length; i++) {
        uint64_t subtrahend = (uint64_t)big_shifted_word(right, right_shift, i) + borrow;
        uint64_t minuend = i < left->length ? left->words[i] : 0U;
        left->words[i] = (uint32_t)(minuend - subtrahend);
        borrow = minuend < subtrahend;
    }
    for (size_t i = shifted_length; borrow && i < left->length; i++) {
        uint32_t old = left->words[i];
        left->words[i] = old - 1U;
        borrow = old == 0U;
    }
    if (borrow) return false;
    big_normalize(left);
    return true;
}

static int big_compare_scaled(const big_uint_t *left, int64_t left_shift,
                             const big_uint_t *right, int64_t right_shift)
{
    int64_t left_bits = (int64_t)big_bit_length(left) + left_shift;
    int64_t right_bits = (int64_t)big_bit_length(right) + right_shift;
    if (left_bits != right_bits) return left_bits < right_bits ? -1 : 1;
    int64_t lowest = left_shift < right_shift ? left_shift : right_shift;
    for (int64_t bit = left_bits - 1; bit >= lowest; bit--) {
        int64_t li = bit - left_shift;
        int64_t ri = bit - right_shift;
        bool lb = li >= 0 && (uint64_t)li < big_bit_length(left) &&
            ((left->words[(size_t)li / 32U] >> ((size_t)li % 32U)) & 1U);
        bool rb = ri >= 0 && (uint64_t)ri < big_bit_length(right) &&
            ((right->words[(size_t)ri / 32U] >> ((size_t)ri % 32U)) & 1U);
        if (lb != rb) return lb ? 1 : -1;
    }
    return 0;
}

static bool big_div_u64(big_uint_t *numerator, const big_uint_t *denominator,
                        uint64_t *quotient, bool *high_bit)
{
    size_t numerator_bits = big_bit_length(numerator);
    size_t denominator_bits = big_bit_length(denominator);
    *quotient = 0;
    *high_bit = false;
    if (!denominator_bits) return false;
    if (numerator_bits < denominator_bits) return true;
    size_t difference = numerator_bits - denominator_bits;
    if (difference > 64U) return false;
    for (size_t step = difference + 1U; step; step--) {
        size_t bit = step - 1U;
        if (big_compare_shifted(numerator, denominator, bit) >= 0) {
            if (!big_sub_shifted(numerator, denominator, bit)) return false;
            if (bit == 64U) *high_bit = true;
            else *quotient |= (uint64_t)1U << bit;
        }
    }
    return true;
}

static int64_t saturating_add(int64_t left, int64_t right)
{
    if (right > 0 && left > INT64_MAX - right) return INT64_MAX;
    if (right < 0 && left < INT64_MIN - right) return INT64_MIN;
    return left + right;
}

static int64_t saturating_sub(int64_t left, int64_t right)
{
    if (right == INT64_MIN) return left >= 0 ? INT64_MAX : left - right;
    return saturating_add(left, -right);
}

static int64_t parse_exponent_wide(const char **cursor, char marker)
{
    const char *start = *cursor;
    const char *p = start;
    bool negative = false;
    int64_t value = 0;
    if (ascii_lower((unsigned char)*p) != (unsigned char)marker) return 0;
    p++;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    const char *digits = p;
    while (*p >= '0' && *p <= '9') {
        int digit = *p++ - '0';
        if (value < INT64_C(1000000000)) value = value * 10 + digit;
        if (value > INT64_C(1000000000)) value = INT64_C(1000000000);
    }
    if (p == digits) return 0;
    *cursor = p;
    return negative ? -value : value;
}

static int target_precision(int kind)
{
    return kind == 0 ? FLT_MANT_DIG : kind == 1 ? DBL_MANT_DIG : LDBL_MANT_DIG;
}

static int target_min_exponent(int kind)
{
    return kind == 0 ? FLT_MIN_EXP - 1 : kind == 1 ? DBL_MIN_EXP - 1 :
                                                       LDBL_MIN_EXP - 1;
}

static int target_max_exponent(int kind)
{
    return kind == 0 ? FLT_MAX_EXP - 1 : kind == 1 ? DBL_MAX_EXP - 1 :
                                                       LDBL_MAX_EXP - 1;
}

static int64_t big_floor_log2_ratio(const big_uint_t *numerator,
                                   const big_uint_t *denominator,
                                   int64_t binary_shift)
{
    int64_t exponent = (int64_t)big_bit_length(numerator) -
                       (int64_t)big_bit_length(denominator) + binary_shift;
    if (big_compare_scaled(numerator, binary_shift,
                           denominator, exponent) < 0)
        exponent--;
    return exponent;
}

static bool big_scaled_quotient(const big_uint_t *numerator,
                                const big_uint_t *denominator,
                                int64_t shift, bool sticky,
                                uint64_t *quotient, bool *quotient_high,
                                bool *inexact, big_uint_t *a, big_uint_t *b)
{
    memcpy(a, numerator, sizeof(*a));
    memcpy(b, denominator, sizeof(*b));
    if (shift >= 0) {
        if ((uint64_t)shift > SIZE_MAX ||
            !big_shift_left(a, (size_t)shift)) return false;
    } else {
        uint64_t amount = (uint64_t)(-(shift + 1)) + 1U;
        if (amount > SIZE_MAX || !big_shift_left(b, (size_t)amount)) return false;
    }
    if (!big_div_u64(a, b, quotient, quotient_high)) return false;
    int half = big_compare_scaled(a, 1, b, 0);
    bool remainder = a->length != 0;
    *inexact = remainder || sticky;
    bool round_up = half > 0 || (half == 0 && (sticky || (*quotient & 1U)));
    if (round_up) {
        if (*quotient == UINT64_MAX) {
            *quotient = 0;
            *quotient_high = true;
        } else (*quotient)++;
    }
    return true;
}

static bool parse_special(const char *start, const char **after,
                          bool *infinity, bool *nan)
{
    int length = ascii_match(start, "inf");
    if (length) {
        *infinity = true;
        *nan = false;
        *after = start + length;
        if (ascii_match(*after, "inity")) *after += 5;
        return true;
    }
    length = ascii_match(start, "nan");
    if (length) {
        *infinity = false;
        *nan = true;
        *after = start + length;
        if (**after == '(') {
            const char *p = *after + 1;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') p++;
            if (*p == ')') *after = p + 1;
        }
        return true;
    }
    return false;
}

static bool parse_mantissa(const char *text, bool hexadecimal,
                           big_uint_t *number, size_t keep_digits,
                           size_t *stored_digits, size_t *total_digits,
                           size_t *fraction_digits, size_t *discarded,
                           bool *sticky, const char **after)
{
    const char *p = text;
    bool point = false, seen_digit = false, seen_nonzero = false;
    *stored_digits = *total_digits = *fraction_digits = *discarded = 0;
    *sticky = false;
    big_zero(number);
    for (;;) {
        int digit = ascii_digit((unsigned char)*p);
        if (digit >= 0 && digit < (hexadecimal ? 16 : 10)) {
            seen_digit = true;
            if (point) (*fraction_digits)++;
            if (seen_nonzero || digit != 0) {
                seen_nonzero = true;
                (*total_digits)++;
                if (*stored_digits < keep_digits) {
                    bool ok = hexadecimal
                        ? (big_shift_left(number, 4U) && big_add_small(number, (uint32_t)digit))
                        : big_mul_add(number, 10U, (uint32_t)digit);
                    if (!ok) return false;
                    (*stored_digits)++;
                } else {
                    (*discarded)++;
                    *sticky |= digit != 0;
                }
            }
            p++;
            continue;
        }
        if (*p == '.' && !point) { point = true; p++; continue; }
        break;
    }
    *after = p;
    return seen_digit;
}

static void set_end(char **end, const char *value)
{
    if (end) *end = (char *)value;
}

static int64_t saturating_size(size_t value)
{
    return value > (size_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

static int parse_float_value(const char *text,
                                      parsed_float_t *parsed,
                                      big_uint_t *numerator)
{
    const char *cursor = text;
    const char *mantissa_end;
    bool sticky = false;
    size_t stored, total, fractional, discarded;
    int64_t explicit_exponent;

    memset(parsed, 0, sizeof(*parsed));
    parsed->end = text;
    if (!text) return EINVAL;
    while (ascii_space((unsigned char)*cursor)) cursor++;
    if (*cursor == '+' || *cursor == '-') parsed->negative = *cursor++ == '-';
    const char *special_end = cursor;
    if (parse_special(cursor, &special_end, &parsed->infinity, &parsed->nan)) {
        parsed->any = true;
        parsed->end = special_end;
        return 0;
    }

    bool hexadecimal = cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X');
    if (hexadecimal) {
        const char *hex_start = cursor + 2;
        if (parse_mantissa(hex_start, true, numerator, HEX_KEEP_DIGITS,
                           &stored, &total, &fractional, &discarded,
                           &sticky, &mantissa_end)) {
            cursor = mantissa_end;
            explicit_exponent = parse_exponent_wide(&cursor, 'p');
            int64_t lost_bits = saturating_size(discarded) > INT64_MAX / 4
                ? INT64_MAX : saturating_size(discarded) * 4;
            int64_t fraction_bits = saturating_size(fractional) > INT64_MAX / 4
                ? INT64_MAX : saturating_size(fractional) * 4;
            parsed->exponent2 = saturating_sub(explicit_exponent, fraction_bits);
            parsed->exponent2 = saturating_add(parsed->exponent2, lost_bits);
            parsed->significant_digits = stored;
            parsed->sticky = sticky;
            parsed->any = true;
            parsed->end = cursor;
            return 0;
        }
    }

    if (!parse_mantissa(cursor, false, numerator, DECIMAL_KEEP_DIGITS,
                        &stored, &total, &fractional, &discarded,
                        &sticky, &mantissa_end)) return 0;
    cursor = mantissa_end;
    explicit_exponent = parse_exponent_wide(&cursor, 'e');
    int64_t dropped = saturating_size(discarded);
    int64_t frac = saturating_size(fractional);
    parsed->decimal_exponent = saturating_add(
        saturating_sub(explicit_exponent, frac), dropped);
    parsed->significant_digits = stored;
    parsed->sticky = sticky;
    parsed->decimal = true;
    parsed->any = true;
    parsed->end = cursor;
    return 0;
}

static int64_t decimal_order(const big_uint_t *number, int64_t exponent,
                             size_t significant_digits)
{
    (void)number;
    int64_t digits = saturating_size(significant_digits);
    return saturating_add(saturating_add(digits, exponent), -1);
}

static bool prepare_ratio(const parsed_float_t *parsed,
                         const big_uint_t *source,
                         big_uint_t *numerator, big_uint_t *denominator,
                         int kind)
{
    (void)kind;
    memcpy(numerator, source, sizeof(*numerator));
    if (!parsed->decimal) {
        big_set_u64(denominator, 1U);
        return true;
    }
    int64_t exponent = parsed->decimal_exponent;
    if (exponent >= 0) {
        if (exponent > 20000) return false;
        for (int64_t i = 0; i < exponent; i++)
            if (!big_mul_small(numerator, 5U)) return false;
        big_set_u64(denominator, 1U);
    } else {
        uint64_t count = (uint64_t)(-(exponent + 1)) + 1U;
        if (count > 20000U) return false;
        big_set_u64(denominator, 1U);
        for (uint64_t i = 0; i < count; i++)
            if (!big_mul_small(denominator, 5U)) return false;
    }
    return true;
}

static void store_zero(void *output, int kind, bool negative)
{
    if (kind == 0) {
        uint32_t bits = negative ? UINT32_C(0x80000000) : 0U;
        memcpy(output, &bits, sizeof(bits));
    } else if (kind == 1) {
        uint64_t bits = negative ? UINT64_C(0x8000000000000000) : 0U;
        memcpy(output, &bits, sizeof(bits));
    } else {
        unsigned char bytes[sizeof(long double)] = {0};
        uint16_t se = negative ? 0x8000U : 0U;
        memcpy(bytes + 8, &se, sizeof(se));
        memcpy(output, bytes, sizeof(bytes));
    }
}

static void store_infinity(void *output, int kind, bool negative)
{
    if (kind == 0) {
        uint32_t bits = negative ? UINT32_C(0xff800000) : UINT32_C(0x7f800000);
        memcpy(output, &bits, sizeof(bits));
    } else if (kind == 1) {
        uint64_t bits = negative ? UINT64_C(0xfff0000000000000) :
                                   UINT64_C(0x7ff0000000000000);
        memcpy(output, &bits, sizeof(bits));
    } else {
        unsigned char bytes[sizeof(long double)] = {0};
        uint64_t significand = UINT64_C(0x8000000000000000);
        uint16_t se = (uint16_t)(0x7fffU | (negative ? 0x8000U : 0U));
        memcpy(bytes, &significand, sizeof(significand));
        memcpy(bytes + 8, &se, sizeof(se));
        memcpy(output, bytes, sizeof(bytes));
    }
}

static int convert_parsed_float(const parsed_float_t *parsed,
                                const big_uint_t *source, int kind,
                                void *output,
                                float_parse_workspace_t *workspace)
{
    int precision = target_precision(kind);
    int min_exp = target_min_exponent(kind);
    int max_exp = target_max_exponent(kind);
    big_uint_t *numerator = &workspace->numerator;
    big_uint_t *denominator = &workspace->denominator;
    bool range_error = false;
    if (parsed->infinity || parsed->nan) {
        if (kind == 0) {
            uint32_t bits = parsed->negative ? UINT32_C(0x80000000) : 0U;
            bits |= parsed->infinity ? UINT32_C(0x7f800000) : UINT32_C(0x7fc00001);
            memcpy(output, &bits, sizeof(bits));
        } else if (kind == 1) {
            uint64_t bits = parsed->negative ? UINT64_C(0x8000000000000000) : 0U;
            bits |= parsed->infinity ? UINT64_C(0x7ff0000000000000) :
                                       UINT64_C(0x7ff8000000000001);
            memcpy(output, &bits, sizeof(bits));
        } else {
            unsigned char bytes[sizeof(long double)] = {0};
            uint64_t significand = parsed->infinity
                ? UINT64_C(0x8000000000000000) :
                  UINT64_C(0xc000000000000001);
            uint16_t se = (uint16_t)(0x7fffU | (parsed->negative ? 0x8000U : 0U));
            memcpy(bytes, &significand, sizeof(significand));
            memcpy(bytes + 8, &se, sizeof(se));
            memcpy(output, bytes, sizeof(bytes));
        }
        return 0;
    }
    if (!source->length) {
        if (kind == 0) {
            uint32_t bits = parsed->negative ? UINT32_C(0x80000000) : 0U;
            memcpy(output, &bits, sizeof(bits));
        } else if (kind == 1) {
            uint64_t bits = parsed->negative ? UINT64_C(0x8000000000000000) : 0U;
            memcpy(output, &bits, sizeof(bits));
        } else {
            unsigned char bytes[sizeof(long double)] = {0};
            uint16_t se = parsed->negative ? 0x8000U : 0U;
            memcpy(bytes + 8, &se, sizeof(se));
            memcpy(output, bytes, sizeof(bytes));
        }
        return 0;
    }
    if (parsed->decimal) {
        int64_t order = decimal_order(source, parsed->decimal_exponent,
                                      parsed->significant_digits);
        int maximum_order = kind == 0 ? 38 : kind == 1 ? 308 : 4932;
        int minimum_order = kind == 0 ? -46 : kind == 1 ? -324 : -4951;
        if (order > maximum_order) {
            store_infinity(output, kind, parsed->negative);
            errno = ERANGE;
            return 0;
        }
        if (order < minimum_order) {
            store_zero(output, kind, parsed->negative);
            errno = ERANGE;
            return 0;
        }
    }
    if (!prepare_ratio(parsed, source, numerator, denominator, kind)) {
        errno = ENOMEM;
        return -1;
    }

    int64_t binary_shift = parsed->decimal
        ? parsed->decimal_exponent : parsed->exponent2;
    int64_t exponent = big_floor_log2_ratio(numerator, denominator,
                                            binary_shift);
    uint64_t significand;
    bool high = false, inexact = false;
    if (exponent > max_exp) {
        range_error = true;
        significand = 0;
    } else if (exponent < (int64_t)min_exp - precision) {
        range_error = true;
        significand = 0;
        inexact = true;
    } else {
        int64_t quantum = (int64_t)min_exp - (precision - 1);
        bool subnormal = exponent < min_exp;
        int64_t shift = subnormal ? binary_shift - quantum :
            binary_shift + (precision - 1) - exponent;
        if (!big_scaled_quotient(numerator, denominator, shift,
                                 parsed->sticky, &significand, &high,
                                 &inexact, &workspace->scratch_a,
                                 &workspace->scratch_b)) {
            errno = ENOMEM;
            return -1;
        }
        if (subnormal) {
            if (high || (precision < 64 && significand >=
                         ((uint64_t)1U << (precision - 1)))) {
                subnormal = false;
                exponent = min_exp;
                significand = (uint64_t)1U << (precision - 1);
                high = false;
            }
            if (inexact) range_error = true;
        } else if (high || (precision < 64 && significand ==
                            ((uint64_t)1U << precision))) {
            significand = (uint64_t)1U << (precision - 1);
            exponent++;
        }
        if (exponent > max_exp) {
            range_error = true;
            high = true;
        }
    }

    if (kind == 0) {
        uint32_t bits;
        if (exponent > max_exp || high)
            bits = parsed->negative ? UINT32_C(0xff800000) : UINT32_C(0x7f800000);
        else if (!significand)
            bits = parsed->negative ? UINT32_C(0x80000000) : 0U;
        else if (exponent < min_exp)
            bits = (parsed->negative ? UINT32_C(0x80000000) : 0U) |
                   (uint32_t)significand;
        else
            bits = (parsed->negative ? UINT32_C(0x80000000) : 0U) |
                   (uint32_t)(exponent + 127) << 23 |
                   ((uint32_t)significand & UINT32_C(0x7fffff));
        memcpy(output, &bits, sizeof(bits));
    } else if (kind == 1) {
        uint64_t bits;
        if (exponent > max_exp || high)
            bits = parsed->negative ? UINT64_C(0xfff0000000000000) :
                                       UINT64_C(0x7ff0000000000000);
        else if (!significand)
            bits = parsed->negative ? UINT64_C(0x8000000000000000) : 0U;
        else if (exponent < min_exp)
            bits = (parsed->negative ? UINT64_C(0x8000000000000000) : 0U) |
                   significand;
        else
            bits = (parsed->negative ? UINT64_C(0x8000000000000000) : 0U) |
                   (uint64_t)(exponent + 1023) << 52 |
                   (significand & UINT64_C(0x000fffffffffffff));
        memcpy(output, &bits, sizeof(bits));
    } else {
        unsigned char bytes[sizeof(long double)] = {0};
        uint16_t exponent_field;
        if (exponent > max_exp || high) {
            significand = UINT64_C(0x8000000000000000);
            exponent_field = 0x7fffU;
        } else if (!significand) {
            exponent_field = 0U;
        } else if (exponent < min_exp) {
            exponent_field = 0U;
        } else {
            exponent_field = (uint16_t)(exponent + 16383);
        }
        if (parsed->negative) exponent_field |= 0x8000U;
        memcpy(bytes, &significand, sizeof(significand));
        memcpy(bytes + 8, &exponent_field, sizeof(exponent_field));
        memcpy(output, bytes, sizeof(bytes));
    }
    if (range_error) errno = ERANGE;
    return 0;
}

int __mg_strto_float(const char *text, char **end, int kind, void *output)
{
    parsed_float_t parsed;
    float_parse_workspace_t *workspace;
    if (!output || kind < 0 || kind > 2) { errno = EINVAL; return -1; }
    workspace = malloc(sizeof(*workspace));
    if (!workspace) { errno = ENOMEM; set_end(end, text); return -1; }
    big_zero(&workspace->source);
    int status = parse_float_value(text, &parsed, &workspace->source);
    if (status) {
        free(workspace);
        errno = status;
        set_end(end, text);
        return -1;
    }
    if (!parsed.any) {
        free(workspace);
        set_end(end, text);
        return 0;
    }
    set_end(end, parsed.end);
    int result = convert_parsed_float(&parsed, &workspace->source, kind,
                                      output, workspace);
    free(workspace);
    return result;
}

static bool emit_char(mg_float_put_fn put, void *context, char value)
{
    return put(context, value);
}

static bool emit_repeat(mg_float_put_fn put, void *context, char value,
                        size_t count)
{
    while (count--) if (!emit_char(put, context, value)) return false;
    return true;
}

static bool emit_text(mg_float_put_fn put, void *context, const char *text,
                      size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (!emit_char(put, context, text[i])) return false;
    return true;
}

static size_t u64_decimal(uint64_t value, char *output)
{
    char reverse[32];
    size_t length = 0;
    do { reverse[length++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    for (size_t i = 0; i < length; i++) output[i] = reverse[length - i - 1U];
    return length;
}

static size_t big_decimal(big_uint_t *value, char *output)
{
    char reverse[6000];
    size_t length = 0;
    while (value->length) {
        uint64_t remainder = 0;
        for (size_t i = value->length; i; i--) {
            uint64_t current = (remainder << 32) | value->words[i - 1U];
            value->words[i - 1U] = (uint32_t)(current / 10U);
            remainder = current % 10U;
        }
        reverse[length++] = (char)('0' + remainder);
        big_normalize(value);
        if (length == sizeof(reverse)) break;
    }
    if (!length) reverse[length++] = '0';
    for (size_t i = 0; i < length; i++) output[i] = reverse[length - i - 1U];
    return length;
}

static bool decimal_expansion(const float_parts_t *parts, char *digits,
                              size_t *length, size_t *point,
                              big_uint_t *integer, big_uint_t *remainder,
                              big_uint_t *denominator)
{
    if (parts->zero) { digits[0] = '0'; *length = 1; *point = 1; return true; }
    big_set_u64(integer, parts->significand);
    if (parts->exponent2 >= 0) {
        if ((uint64_t)parts->exponent2 > SIZE_MAX ||
            !big_shift_left(integer, (size_t)parts->exponent2)) return false;
        *length = big_decimal(integer, digits);
        *point = *length;
        return true;
    }
    uint64_t integer_part = 0;
    int64_t shift = -parts->exponent2;
    if (shift < 64) integer_part = parts->significand >> shift;
    big_set_u64(remainder, parts->significand);
    if (shift >= 0) {
        size_t whole = (size_t)shift / 32U;
        unsigned int part = (unsigned int)((size_t)shift % 32U);
        if (whole >= remainder->length) {
            /* The complete significand is below the binary point. */
        } else {
            size_t keep = whole + (part ? 1U : 0U);
            if (keep < remainder->length) remainder->length = keep;
            if (part && whole < remainder->length)
                remainder->words[whole] &= (UINT32_C(1) << part) - 1U;
            else if (!part && whole < remainder->length)
                remainder->length = whole;
            big_normalize(remainder);
        }
    }
    if (shift >= 64) big_set_u64(remainder, parts->significand);
    if (shift > (int64_t)(BIG_LIMBS * 32U) ||
        !big_set_bit(denominator, (size_t)shift)) return false;
    *point = u64_decimal(integer_part, digits);
    *length = *point;
    while (remainder->length && *length < DECIMAL_TEXT_CAPACITY) {
        if (!big_mul_small(remainder, 10U)) return false;
        unsigned int digit = 0;
        while (big_compare(remainder, denominator) >= 0) {
            if (!big_sub_shifted(remainder, denominator, 0)) return false;
            digit++;
        }
        if (digit > 9U) return false;
        digits[(*length)++] = (char)('0' + digit);
    }
    return remainder->length == 0;
}

static size_t first_nonzero(const char *digits, size_t length)
{
    for (size_t i = 0; i < length; i++) if (digits[i] != '0') return i;
    return SIZE_MAX;
}

static bool round_decimal(char *digits, size_t *length, size_t *point,
                          size_t *first, size_t cut)
{
    if (cut >= *length || *first == SIZE_MAX) return true;
    unsigned int guard = (unsigned int)(digits[cut] - '0');
    bool sticky = false;
    for (size_t i = cut + 1U; i < *length; i++) sticky |= digits[i] != '0';
    char previous = cut ? digits[cut - 1U] : '0';
    bool up = guard > 5U || (guard == 5U &&
        (sticky || ((previous - '0') & 1)));
    if (!up) { *length = cut; return true; }
    size_t i = cut;
    while (i > *first) {
        i--;
        if (digits[i] != '9') { digits[i]++; *length = cut; return true; }
        digits[i] = '0';
    }
    if (*first) {
        digits[*first - 1U] = '1';
        *first -= 1U;
        for (size_t j = *first + 1U; j < cut; j++) digits[j] = '0';
        *length = cut;
        return true;
    }
    if (*length + 1U >= DECIMAL_TEXT_CAPACITY) return false;
    memmove(digits + 1, digits, *length);
    digits[0] = '1';
    (*length)++;
    (*point)++;
    *first = 0;
    *length = cut + 1U;
    return true;
}

static int64_t decimal_exponent(size_t point, size_t first)
{
    return (int64_t)point - (int64_t)first - 1;
}

static size_t digit_count_u32(unsigned int value, char *output)
{
    return u64_decimal(value, output);
}

static bool format_padding(mg_float_put_fn put, void *context,
                           unsigned int flags, int width, size_t body_length,
                           const char *sign, size_t sign_length,
                           bool special, size_t *left_spaces,
                           size_t *zeroes, size_t *right_spaces)
{
    size_t padding = width > 0 && (size_t)width > body_length + sign_length
        ? (size_t)width - body_length - sign_length : 0;
    bool left = (flags & 1U) != 0;
    bool zero = (flags & 16U) != 0 && !left && !special;
    *left_spaces = !left && !zero ? padding : 0;
    *zeroes = zero ? padding : 0;
    *right_spaces = left ? padding : 0;
    (void)put; (void)context;
    (void)sign;
    return true;
}

static bool emit_padded(mg_float_put_fn put, void *context,
                        unsigned int flags, int width, size_t body_length,
                        const char *sign, size_t sign_length, bool special,
                        bool (*body)(mg_float_put_fn, void *, void *),
                        void *body_context)
{
    size_t left_spaces, zeroes, right_spaces;
    (void)format_padding(put, context, flags, width, body_length, sign,
                         sign_length, special, &left_spaces, &zeroes,
                         &right_spaces);
    if (!emit_repeat(put, context, ' ', left_spaces) ||
        !emit_text(put, context, sign, sign_length) ||
        !emit_repeat(put, context, '0', zeroes) ||
        !body(put, context, body_context) ||
        !emit_repeat(put, context, ' ', right_spaces)) return false;
    return true;
}

typedef struct {
    char *digits;
    size_t length;
    size_t point;
    size_t first;
    size_t precision;
    bool decimal_point;
} fixed_body_t;

static char digit_or_zero(const fixed_body_t *body, size_t index)
{
    return index < body->length ? body->digits[index] : '0';
}

static bool emit_fixed_body(mg_float_put_fn put, void *context, void *opaque)
{
    fixed_body_t *body = opaque;
    for (size_t i = 0; i < body->point; i++)
        if (!emit_char(put, context, digit_or_zero(body, i))) return false;
    if (body->decimal_point && !emit_char(put, context, '.')) return false;
    for (size_t i = 0; i < body->precision; i++)
        if (!emit_char(put, context,
                       digit_or_zero(body, body->point + i))) return false;
    return true;
}

typedef struct {
    char *digits;
    size_t length;
    size_t first;
    size_t precision;
    int64_t exponent;
    bool decimal_point;
    bool uppercase;
} exponential_body_t;

static bool emit_exponential_body(mg_float_put_fn put, void *context,
                                  void *opaque)
{
    exponential_body_t *body = opaque;
    char exponent_digits[32];
    char first = body->first < body->length ? body->digits[body->first] : '0';
    if (!emit_char(put, context, first)) return false;
    if (body->decimal_point && !emit_char(put, context, '.')) return false;
    for (size_t i = 0; i < body->precision; i++) {
        size_t index = body->first + 1U + i;
        char c = index < body->length ? body->digits[index] : '0';
        if (!emit_char(put, context, c)) return false;
    }
    int64_t magnitude = body->exponent < 0 ? -body->exponent : body->exponent;
    size_t count = digit_count_u32((unsigned int)magnitude, exponent_digits);
    if (count < 2U) {
        memmove(exponent_digits + 1, exponent_digits, count);
        exponent_digits[0] = '0';
        count++;
    }
    if (!emit_char(put, context, body->uppercase ? 'E' : 'e') ||
        !emit_char(put, context, body->exponent < 0 ? '-' : '+')) return false;
    return emit_text(put, context, exponent_digits, count);
}

static bool read_parts(const void *value, int source_kind,
                       float_parts_t *parts)
{
    memset(parts, 0, sizeof(*parts));
    if (source_kind == 1) {
        uint64_t bits;
        memcpy(&bits, value, sizeof(bits));
        uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);
        unsigned int exponent = (unsigned int)((bits >> 52) & 0x7ffU);
        parts->negative = (bits >> 63) != 0;
        parts->precision = DBL_MANT_DIG;
        parts->minimum_exponent = DBL_MIN_EXP - 1;
        parts->infinity = exponent == 0x7ffU && !fraction;
        parts->nan = exponent == 0x7ffU && fraction;
        parts->zero = exponent == 0 && !fraction;
        parts->subnormal = exponent == 0 && fraction;
        if (parts->infinity || parts->nan || parts->zero) return true;
        parts->significand = exponent ? fraction | UINT64_C(0x0010000000000000)
                                      : fraction;
        parts->exponent2 = exponent ? (int64_t)exponent - 1023 - 52
                                    : 1 - 1023 - 52;
        return true;
    }
    if (source_kind == 2) {
        unsigned char bytes[sizeof(long double)];
        uint64_t significand;
        uint16_t se;
        if (sizeof(long double) != 16U) return false;
        memcpy(bytes, value, sizeof(bytes));
        memcpy(&significand, bytes, sizeof(significand));
        memcpy(&se, bytes + 8, sizeof(se));
        unsigned int exponent = se & 0x7fffU;
        parts->negative = (se & 0x8000U) != 0;
        parts->precision = LDBL_MANT_DIG;
        parts->minimum_exponent = LDBL_MIN_EXP - 1;
        parts->infinity = exponent == 0x7fffU &&
                          significand == UINT64_C(0x8000000000000000);
        parts->nan = exponent == 0x7fffU && !parts->infinity;
        parts->zero = exponent == 0 && significand == 0;
        parts->subnormal = exponent == 0 && significand != 0 &&
                           !(significand & UINT64_C(0x8000000000000000));
        if (parts->infinity || parts->nan || parts->zero) return true;
        parts->significand = significand;
        if (exponent == 0 && (significand & UINT64_C(0x8000000000000000)))
            parts->exponent2 = 1 - 16383 - 63; /* pseudo-denormal */
        else parts->exponent2 = (exponent ? (int64_t)exponent : 1) - 16383 - 63;
        return true;
    }
    return false;
}

static bool format_special(char conversion, const float_parts_t *parts,
                           unsigned int flags, int width,
                           mg_float_put_fn put, void *context)
{
    bool upper = conversion == 'A' || conversion == 'E' ||
                 conversion == 'F' || conversion == 'G';
    const char *word = parts->infinity ? (upper ? "INF" : "inf") :
                                         (upper ? "NAN" : "nan");
    char sign[1]; size_t sign_length = 0;
    if (parts->negative) { sign[0] = '-'; sign_length = 1; }
    else if (flags & 2U) { sign[0] = '+'; sign_length = 1; }
    else if (flags & 4U) { sign[0] = ' '; sign_length = 1; }
    size_t pad = width > 0 && (size_t)width > 3U + sign_length
        ? (size_t)width - 3U - sign_length : 0;
    if (!(flags & 1U) && !emit_repeat(put, context, ' ', pad)) return false;
    if (!emit_text(put, context, sign, sign_length) ||
        !emit_text(put, context, word, 3U)) return false;
    return (flags & 1U) ? emit_repeat(put, context, ' ', pad) : true;
}

static bool format_hex(char conversion, const float_parts_t *parts,
                       unsigned int flags, int width, int precision,
                       bool precision_set, mg_float_put_fn put, void *context)
{
    bool upper = conversion == 'A';
    int precision_bits;
    unsigned int available_bits = 0;
    uint64_t fraction = parts->significand;
    int64_t exponent;
    bool subnormal = parts->subnormal;
    char leading_digit = '1';
    unsigned int top = 0;
    if (parts->zero) {
        exponent = 0;
        fraction = 0;
        available_bits = 0;
        leading_digit = '0';
    } else if (subnormal) {
        exponent = parts->minimum_exponent;
        available_bits = (unsigned int)parts->precision - 1U;
        leading_digit = '0';
    } else {
        uint64_t value = fraction;
        while (value >>= 1) top++;
        exponent = parts->exponent2 + top;
        available_bits = top;
        fraction &= ~(UINT64_C(1) << top);
    }
    if (!precision_set) precision = (int)((available_bits + 3U) / 4U);
    if (precision < 0) precision = 0;
    if ((uint64_t)precision > (uint64_t)INT_MAX / 4U) return false;
    precision_bits = precision * 4;
    uint64_t rounded = fraction;
    if ((unsigned int)precision_bits < available_bits) {
        unsigned int discard = available_bits - (unsigned int)precision_bits;
        uint64_t retained = fraction >> discard;
        bool guard = (fraction >> (discard - 1U)) & 1U;
        uint64_t mask = discard > 1U ? ((UINT64_C(1) << (discard - 1U)) - 1U) : 0U;
        bool sticky = (fraction & mask) != 0;
        if (guard && (sticky || (retained & 1U))) retained++;
        if (subnormal && retained == (UINT64_C(1) << precision_bits)) {
            subnormal = false;
            rounded = 0;
        } else if (retained == (UINT64_C(1) << precision_bits) && !subnormal) {
            leading_digit = '2';
            rounded = 0;
        } else {
            rounded = retained << discard;
        }
    }
    char sign[1]; size_t sign_length = 0;
    if (parts->negative) { sign[0] = '-'; sign_length = 1; }
    else if (flags & 2U) { sign[0] = '+'; sign_length = 1; }
    else if (flags & 4U) { sign[0] = ' '; sign_length = 1; }
    char exponent_digits[32];
    size_t exponent_length = digit_count_u32((unsigned int)(exponent < 0 ?
        -exponent : exponent), exponent_digits);
    if (exponent_length < 1U) exponent_digits[exponent_length++] = '0';
    size_t body_length = 3U + ((precision || (flags & 8U)) ? 1U : 0U) +
                         (size_t)precision + 2U + exponent_length;
    size_t padding = width > 0 && (size_t)width > body_length + sign_length
        ? (size_t)width - body_length - sign_length : 0;
    bool left = (flags & 1U) != 0;
    bool zero = (flags & 16U) && !left;
    if (!left && !zero && !emit_repeat(put, context, ' ', padding)) return false;
    if (!emit_text(put, context, sign, sign_length) ||
        !emit_char(put, context, '0') ||
        !emit_char(put, context, upper ? 'X' : 'x') ||
        !emit_repeat(put, context, '0', zero ? padding : 0U) ||
        !emit_char(put, context, leading_digit)) return false;
    if ((precision || (flags & 8U)) && !emit_char(put, context, '.')) return false;
    for (int digit = 0; digit < precision; digit++) {
        unsigned int nibble = 0;
        for (unsigned int bit = 0; bit < 4; bit++) {
            unsigned int position = (unsigned int)digit * 4U + bit;
            bool set = position < available_bits &&
                ((rounded >> (available_bits - position - 1U)) & 1U);
            nibble = (nibble << 1) | (set ? 1U : 0U);
        }
        char c = nibble < 10U ? (char)('0' + nibble) :
            (char)((upper ? 'A' : 'a') + nibble - 10U);
        if (!emit_char(put, context, c)) return false;
    }
    if (!emit_char(put, context, upper ? 'P' : 'p') ||
        !emit_char(put, context, exponent < 0 ? '-' : '+')) return false;
    if (!emit_text(put, context, exponent_digits, exponent_length)) return false;
    return !left || emit_repeat(put, context, ' ', padding);
}

static bool format_decimal_float(char conversion, const float_parts_t *parts,
                                 unsigned int flags, int width, int precision,
                                 bool precision_set,
                                 float_format_workspace_t *workspace,
                                 mg_float_put_fn put, void *context)
{
    char *exact = workspace->exact;
    size_t exact_length, point;
    if (!decimal_expansion(parts, exact, &exact_length, &point,
                           &workspace->integer, &workspace->remainder,
                           &workspace->denominator)) return false;
    char *digits = workspace->digits;
    memcpy(digits, exact, exact_length);
    size_t length = exact_length;
    size_t first = first_nonzero(digits, length);
    bool upper = conversion == 'E' || conversion == 'F' || conversion == 'G';
    bool alternate = (flags & 8U) != 0;
    if (!precision_set) precision = 6;
    if (precision < 0) return false;
    if (conversion == 'g' || conversion == 'G') {
        if (!precision) precision = 1;
        size_t cut = first == SIZE_MAX ? 1U : first + (size_t)precision;
        if (!round_decimal(digits, &length, &point, &first, cut)) return false;
        int64_t exponent = first == SIZE_MAX ? 0 : decimal_exponent(point, first);
        bool scientific = exponent < -4 || exponent >= precision;
        if (scientific) {
            size_t fractional = (size_t)precision - 1U;
            if (!alternate) {
                while (fractional) {
                    size_t last = first + fractional;
                    if (last < length && digits[last] != '0') break;
                    fractional--;
                }
            }
            exponential_body_t body = {digits, length, first, fractional,
                exponent, fractional || alternate, upper};
            char exponent_text[32];
            size_t exp_digits = digit_count_u32((unsigned int)(exponent < 0 ?
                -exponent : exponent), exponent_text);
            if (exp_digits < 2U) exp_digits = 2U;
            size_t body_length = 1U + (body.decimal_point ? 1U : 0U) +
                fractional + 2U + exp_digits;
            char sign[1]; size_t sign_length = 0;
            if (parts->negative) { sign[0] = '-'; sign_length = 1; }
            else if (flags & 2U) { sign[0] = '+'; sign_length = 1; }
            else if (flags & 4U) { sign[0] = ' '; sign_length = 1; }
            return emit_padded(put, context, flags, width, body_length, sign,
                               sign_length, false,
                               (bool (*)(mg_float_put_fn, void *, void *))
                               emit_exponential_body, &body);
        }
        int64_t fractional_count = (int64_t)precision - exponent - 1;
        size_t fractional = fractional_count > 0
            ? (size_t)fractional_count : 0U;
        if (!alternate) {
            while (fractional &&
                   digit_or_zero(&(fixed_body_t){digits, length, point, first,
                                                  fractional, true}, point + fractional - 1U) == '0')
                fractional--;
        }
        fixed_body_t body = {digits, length, point, first, fractional,
                             fractional || alternate};
        size_t body_length = point + (body.decimal_point ? 1U : 0U) + fractional;
        char sign[1]; size_t sign_length = 0;
        if (parts->negative) { sign[0] = '-'; sign_length = 1; }
        else if (flags & 2U) { sign[0] = '+'; sign_length = 1; }
        else if (flags & 4U) { sign[0] = ' '; sign_length = 1; }
        return emit_padded(put, context, flags, width, body_length, sign,
                           sign_length, false, emit_fixed_body, &body);
    }
    if (conversion == 'e' || conversion == 'E') {
        if (!precision_set) precision = 6;
        size_t significant = (size_t)precision + 1U;
        size_t cut = first == SIZE_MAX ? significant : first + significant;
        if (!round_decimal(digits, &length, &point, &first, cut)) return false;
        int64_t exponent = first == SIZE_MAX ? 0 : decimal_exponent(point, first);
        exponential_body_t body = {digits, length, first == SIZE_MAX ? 0U : first,
            (size_t)precision, exponent, precision > 0 || alternate, upper};
        char exponent_text[32];
        size_t exp_digits = digit_count_u32((unsigned int)(exponent < 0 ?
            -exponent : exponent), exponent_text);
        if (exp_digits < 2U) exp_digits = 2U;
        size_t body_length = 1U + (body.decimal_point ? 1U : 0U) +
                             (size_t)precision + 2U + exp_digits;
        char sign[1]; size_t sign_length = 0;
        if (parts->negative) { sign[0] = '-'; sign_length = 1; }
        else if (flags & 2U) { sign[0] = '+'; sign_length = 1; }
        else if (flags & 4U) { sign[0] = ' '; sign_length = 1; }
        return emit_padded(put, context, flags, width, body_length, sign,
                           sign_length, false, emit_exponential_body, &body);
    }
    if (conversion == 'f' || conversion == 'F') {
        size_t cut = point + (size_t)precision;
        if (!round_decimal(digits, &length, &point, &first, cut)) return false;
        fixed_body_t body = {digits, length, point, first, (size_t)precision,
                             precision > 0 || alternate};
        size_t body_length = point + (body.decimal_point ? 1U : 0U) +
                             (size_t)precision;
        char sign[1]; size_t sign_length = 0;
        if (parts->negative) { sign[0] = '-'; sign_length = 1; }
        else if (flags & 2U) { sign[0] = '+'; sign_length = 1; }
        else if (flags & 4U) { sign[0] = ' '; sign_length = 1; }
        return emit_padded(put, context, flags, width, body_length, sign,
                           sign_length, false, emit_fixed_body, &body);
    }
    return false;
}

bool __mg_format_float(char conversion, int source_kind, unsigned int flags,
                       int width, int precision, bool precision_set,
                       const void *value, mg_float_put_fn put, void *context)
{
    float_parts_t parts;
    if (!value || !put || !read_parts(value, source_kind, &parts)) return false;
    if (parts.infinity || parts.nan)
        return format_special(conversion, &parts, flags, width, put, context);
    if (conversion == 'a' || conversion == 'A')
        return format_hex(conversion, &parts, flags, width, precision,
                          precision_set, put, context);
    float_format_workspace_t *workspace = malloc(sizeof(*workspace));
    if (!workspace) { errno = ENOMEM; return false; }
    bool result = format_decimal_float(conversion, &parts, flags, width,
                                       precision, precision_set, workspace,
                                       put, context);
    free(workspace);
    return result;
}
