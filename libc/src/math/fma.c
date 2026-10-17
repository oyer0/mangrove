/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <float.h>
#include <math.h>

/* The exact product and addend are aligned in an integer accumulator.  Its
 * low bit is the least possible product bit for the format, so cancellation
 * and sticky-bit decisions do not depend on intermediate floating arithmetic.
 * The largest workspace is 1027 words (8216 bytes) for x87 extended. */
#define MG_FMA_F_BASE (-298)
#define MG_FMA_F_WORDS 10
#define MG_FMA_D_BASE (-2148)
#define MG_FMA_D_WORDS 67
#define MG_FMA_L_BASE (-32890)
#define MG_FMA_L_WORDS 1027

_Static_assert(MG_FMA_F_WORDS * 64 >= 2 * 127 + 1 - MG_FMA_F_BASE + 1,
               "binary32 fused accumulator is too short");
_Static_assert(MG_FMA_D_WORDS * 64 >= 2 * 1023 + 1 - MG_FMA_D_BASE + 1,
               "binary64 fused accumulator is too short");
_Static_assert(MG_FMA_L_WORDS * 64 >= 2 * 16383 + 1 - MG_FMA_L_BASE + 1,
               "x87 fused accumulator is too short");

typedef struct {
    uint64_t significand;
    unsigned exponent;
    int negative;
} mg_fma_result;

static int mg_fma_rounding_mode(void)
{
    int mode = fegetround();
    return mode == FE_TONEAREST || mode == FE_TOWARDZERO ||
           mode == FE_UPWARD || mode == FE_DOWNWARD ? mode : FE_TONEAREST;
}

static uint64_t mg_fma_term_limb(uint64_t low, uint64_t high, int index)
{
    if (index == 0)
        return low;
    if (index == 1)
        return high;
    return 0;
}

static uint64_t mg_fma_term_word(uint64_t low, uint64_t high,
                                int shift, unsigned word)
{
    unsigned whole = (unsigned)shift / 64U;
    unsigned part = (unsigned)shift % 64U;
    int index = (int)word - (int)whole;
    uint64_t current = mg_fma_term_limb(low, high, index);
    if (part == 0)
        return current;
    return (current << part) |
           (mg_fma_term_limb(low, high, index - 1) >> (64U - part));
}

static int mg_fma_compare(uint64_t al, uint64_t ah, int ashift,
                          uint64_t bl, uint64_t bh, int bshift,
                          unsigned words)
{
    for (unsigned i = words; i-- > 0;) {
        uint64_t a = mg_fma_term_word(al, ah, ashift, i);
        uint64_t b = mg_fma_term_word(bl, bh, bshift, i);
        if (a != b)
            return a > b ? 1 : -1;
    }
    return 0;
}

static unsigned mg_fma_high_bit(uint64_t value)
{
    return 63U - (unsigned)__builtin_clzll(value);
}

static int mg_fma_any_below(const uint64_t *value, unsigned words,
                            unsigned bit)
{
    unsigned whole = bit / 64U;
    unsigned part = bit % 64U;
    for (unsigned i = 0; i < whole && i < words; ++i)
        if (value[i] != 0)
            return 1;
    if (part != 0 && whole < words &&
        (value[whole] & ((UINT64_C(1) << part) - 1U)) != 0)
        return 1;
    return 0;
}

static uint64_t mg_fma_extract(const uint64_t *value, unsigned words,
                               unsigned shift)
{
    unsigned whole = shift / 64U;
    unsigned part = shift % 64U;
    uint64_t low = whole < words ? value[whole] : 0;
    if (part == 0)
        return low;
    uint64_t high = whole + 1U < words ? value[whole + 1U] : 0;
    return (low >> part) | (high << (64U - part));
}

static mg_fma_result mg_fma_pack_exact(uint64_t *magnitude, unsigned words,
                                       int base, unsigned precision,
                                       int minimum_exponent,
                                       int maximum_exponent, int negative)
{
    mg_fma_result result = {0, 0, negative};
    int top_word = (int)words - 1;
    while (top_word >= 0 && magnitude[top_word] == 0)
        --top_word;
    if (top_word < 0) {
        /* Exact cancellation is -0 only in roundTowardNegative. */
        result.negative = mg_fma_rounding_mode() == FE_DOWNWARD;
        return result;
    }

    int top = top_word * 64 + (int)mg_fma_high_bit(magnitude[top_word]);
    int exponent = base + top;
    int quantum = exponent < minimum_exponent ?
                  minimum_exponent - ((int)precision - 1) :
                  exponent - ((int)precision - 1);
    unsigned shift = (unsigned)(quantum - base);
    uint64_t rounded = mg_fma_extract(magnitude, words, shift);
    int inexact = shift != 0 && mg_fma_any_below(magnitude, words, shift);
    int guard = shift != 0 &&
                ((magnitude[(shift - 1U) / 64U] >> ((shift - 1U) % 64U)) & 1U);
    int sticky = shift > 1U && mg_fma_any_below(magnitude, words, shift - 1U);
    int mode = mg_fma_rounding_mode();
    int increment = 0;
    if (inexact) {
        if (mode == FE_TONEAREST)
            increment = guard && (sticky || (rounded & 1U));
        else if (mode == FE_UPWARD)
            increment = !negative;
        else if (mode == FE_DOWNWARD)
            increment = negative;
    }

    if (increment) {
        ++rounded;
        if (precision == 64U && rounded == 0) {
            rounded = UINT64_C(1) << 63;
            ++quantum;
        } else if (precision < 64U &&
                   rounded == (UINT64_C(1) << precision)) {
            rounded >>= 1;
            ++quantum;
        }
    }

    int rounded_exponent = quantum + ((int)precision - 1);
    if (rounded_exponent > maximum_exponent) {
        int infinity = mode == FE_TONEAREST ||
                       (mode == FE_UPWARD && !negative) ||
                       (mode == FE_DOWNWARD && negative);
        result.exponent = (unsigned)(2 * maximum_exponent + 1);
        if (infinity) {
            result.significand = precision == 64U ?
                                 UINT64_C(1) << 63 : 0;
        } else {
            result.exponent -= 1U;
            result.significand = precision == 64U ? UINT64_MAX :
                                 (UINT64_C(1) << precision) - 1U;
        }
        (void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);
        return result;
    }

    uint64_t hidden = UINT64_C(1) << (precision - 1U);
    if (rounded == 0) {
        result.significand = 0;
        result.exponent = 0;
    } else if (quantum == minimum_exponent - ((int)precision - 1) &&
               rounded < hidden) {
        result.significand = rounded;
        result.exponent = 0;
    } else {
        result.significand = rounded;
        result.exponent = (unsigned)(rounded_exponent - minimum_exponent + 1);
    }

    if (inexact) {
        if (result.exponent == 0)
            (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
        else
            (void)feraiseexcept(FE_INEXACT);
    }
    return result;
}

static mg_fma_result mg_fma_finite(uint64_t a, int ae, int aneg,
                                   uint64_t b, int be, int bneg,
                                   uint64_t c, int ce, int cneg,
                                   unsigned precision, int minimum_exponent,
                                   int maximum_exponent, int base,
                                   unsigned words)
{
    uint64_t acc[words];
    for (unsigned i = 0; i < words; ++i)
        acc[i] = 0;

    unsigned __int128 product = (unsigned __int128)a * b;
    uint64_t pl = (uint64_t)product;
    uint64_t ph = (uint64_t)(product >> 64);
    int ps = ae + be - base;
    int cs = ce - base;
    int product_sign = aneg != bneg;
    int comparison = mg_fma_compare(pl, ph, ps, c, 0, cs, words);
    int result_sign = product_sign;
    int subtract = product_sign != cneg;

    if (subtract && comparison == 0) {
        result_sign = mg_fma_rounding_mode() == FE_DOWNWARD;
    } else if (!subtract) {
        uint64_t carry = 0;
        for (unsigned i = 0; i < words; ++i) {
            unsigned __int128 sum =
                (unsigned __int128)mg_fma_term_word(pl, ph, ps, i) +
                mg_fma_term_word(c, 0, cs, i) + carry;
            acc[i] = (uint64_t)sum;
            carry = (uint64_t)(sum >> 64);
        }
        result_sign = product_sign;
    } else {
        uint64_t borrow = 0;
        int product_larger = comparison > 0;
        uint64_t large_low = product_larger ? pl : c;
        uint64_t large_high = product_larger ? ph : 0;
        int large_shift = product_larger ? ps : cs;
        uint64_t small_low = product_larger ? c : pl;
        uint64_t small_high = product_larger ? 0 : ph;
        int small_shift = product_larger ? cs : ps;
        result_sign = product_larger ? product_sign : cneg;
        for (unsigned i = 0; i < words; ++i) {
            uint64_t left = mg_fma_term_word(large_low, large_high,
                                             large_shift, i);
            uint64_t right = mg_fma_term_word(small_low, small_high,
                                              small_shift, i);
            uint64_t subtrahend = right + borrow;
            uint64_t overflow = subtrahend < right;
            acc[i] = left - subtrahend;
            borrow = overflow || left < subtrahend;
        }
    }

    return mg_fma_pack_exact(acc, words, base, precision,
                             minimum_exponent, maximum_exponent, result_sign);
}

static mg_fma_result mg_fma_special(int invalid_product, int product_infinite,
                                    int product_negative, int addend_infinite,
                                    int addend_negative, unsigned precision,
                                    int maximum_exponent)
{
    mg_fma_result result = {0, 0, product_negative};
    if (invalid_product || (product_infinite && addend_infinite &&
                            product_negative != addend_negative)) {
        (void)feraiseexcept(FE_INVALID);
        result.exponent = (unsigned)(2 * maximum_exponent + 1);
        result.significand = precision == 64U ?
                             (UINT64_C(1) << 63) | (UINT64_C(1) << 62) :
                             UINT64_C(1) << (precision - 2U);
        return result;
    }
    if (addend_infinite) {
        result.negative = addend_negative;
        result.exponent = (unsigned)(2 * maximum_exponent + 1);
        result.significand = precision == 64U ? UINT64_C(1) << 63 : 0;
    } else if (product_infinite) {
        result.exponent = (unsigned)(2 * maximum_exponent + 1);
        result.significand = precision == 64U ? UINT64_C(1) << 63 : 0;
    }
    return result;
}

static float mg_fma_float_value(mg_fma_result value)
{
    uint32_t bits = (uint32_t)value.significand & UINT32_C(0x7fffff);
    if (value.exponent != 0)
        bits |= (uint32_t)value.exponent << 23;
    if (value.negative)
        bits |= UINT32_C(0x80000000);
    return mg_float_from_bits(bits);
}

static double mg_fma_double_value(mg_fma_result value)
{
    uint64_t bits = value.significand & UINT64_C(0x000fffffffffffff);
    if (value.exponent != 0)
        bits |= (uint64_t)value.exponent << 52;
    if (value.negative)
        bits |= UINT64_C(0x8000000000000000);
    return mg_double_from_bits(bits);
}

static long double mg_fma_long_double_value(mg_fma_result value)
{
    mg_x87_value raw = {value.significand,
                        (uint16_t)(value.exponent |
                                   (value.negative ? 0x8000U : 0U)),
                        {0}};
    return mg_x87_pack(raw);
}

float fmaf(float x, float y, float z)
{
    uint32_t xb = mg_float_bits(x), yb = mg_float_bits(y), zb = mg_float_bits(z);
    unsigned xe = (xb >> 23) & 0xffU, ye = (yb >> 23) & 0xffU;
    unsigned ze = (zb >> 23) & 0xffU;
    uint64_t xs = xb & UINT32_C(0x7fffff), ys = yb & UINT32_C(0x7fffff);
    uint64_t zs = zb & UINT32_C(0x7fffff);
    int xn = (xb >> 31) != 0, yn = (yb >> 31) != 0, zn = (zb >> 31) != 0;
    int xinf = xe == 0xffU && xs == 0, yinf = ye == 0xffU && ys == 0;
    int zinf = ze == 0xffU && zs == 0;
    int xnan = xe == 0xffU && xs != 0, ynan = ye == 0xffU && ys != 0;
    int znan = ze == 0xffU && zs != 0;
    if ((xinf && ye == 0 && ys == 0) ||
        (yinf && xe == 0 && xs == 0))
        return mg_fma_float_value(mg_fma_special(1, 0, xn != yn, zinf, zn,
                                                 24, 127));
    if (xnan || ynan || znan) {
        if ((xnan && !(xs & (UINT32_C(1) << 22))) ||
            (ynan && !(ys & (UINT32_C(1) << 22))) ||
            (znan && !(zs & (UINT32_C(1) << 22))))
            (void)feraiseexcept(FE_INVALID);
        uint32_t chosen = (uint32_t)(xnan ? xb : (ynan ? yb : zb));
        return mg_float_from_bits(chosen | (UINT32_C(1) << 22));
    }
    uint64_t xa = xs, ya = ys, za = zs;
    int xexp = -149, yexp = -149, zexp = -149;
    if (xe != 0) { xa |= UINT64_C(1) << 23; xexp = (int)xe - 127 - 23; }
    if (ye != 0) { ya |= UINT64_C(1) << 23; yexp = (int)ye - 127 - 23; }
    if (ze != 0) { za |= UINT64_C(1) << 23; zexp = (int)ze - 127 - 23; }
    int product_zero = xa == 0 || ya == 0;
    if (xinf || yinf || zinf)
        return mg_fma_float_value(mg_fma_special(0, xinf || yinf, xn != yn,
                                                 zinf, zn, 24, 127));
    if (product_zero && za == 0) {
        int negative = (xn != yn) == zn ? (xn != yn) :
                       mg_fma_rounding_mode() == FE_DOWNWARD;
        mg_fma_result zero = {0, 0, negative};
        return mg_fma_float_value(zero);
    }
    if (product_zero)
        return z;
    if (za == 0)
        za = 0;
    mg_fma_result value = mg_fma_finite(xa, xexp, xn, ya, yexp, yn,
                                        za, zexp, zn, 24, -126, 127,
                                        MG_FMA_F_BASE, MG_FMA_F_WORDS);
    return mg_fma_float_value(value);
}

double fma(double x, double y, double z)
{
    uint64_t xb = mg_double_bits(x), yb = mg_double_bits(y), zb = mg_double_bits(z);
    unsigned xe = (unsigned)((xb >> 52) & 0x7ffU);
    unsigned ye = (unsigned)((yb >> 52) & 0x7ffU);
    unsigned ze = (unsigned)((zb >> 52) & 0x7ffU);
    uint64_t xs = xb & UINT64_C(0x000fffffffffffff);
    uint64_t ys = yb & UINT64_C(0x000fffffffffffff);
    uint64_t zs = zb & UINT64_C(0x000fffffffffffff);
    int xn = (xb >> 63) != 0, yn = (yb >> 63) != 0, zn = (zb >> 63) != 0;
    int xinf = xe == 0x7ffU && xs == 0, yinf = ye == 0x7ffU && ys == 0;
    int zinf = ze == 0x7ffU && zs == 0;
    int xnan = xe == 0x7ffU && xs != 0, ynan = ye == 0x7ffU && ys != 0;
    int znan = ze == 0x7ffU && zs != 0;
    if ((xinf && ye == 0 && ys == 0) || (yinf && xe == 0 && xs == 0))
        return mg_fma_double_value(mg_fma_special(1, 0, xn != yn, zinf, zn,
                                                  53, 1023));
    if (xnan || ynan || znan) {
        if ((xnan && !(xs & (UINT64_C(1) << 51))) ||
            (ynan && !(ys & (UINT64_C(1) << 51))) ||
            (znan && !(zs & (UINT64_C(1) << 51))))
            (void)feraiseexcept(FE_INVALID);
        uint64_t chosen = xnan ? xb : (ynan ? yb : zb);
        return mg_double_from_bits(chosen | (UINT64_C(1) << 51));
    }
    uint64_t xa = xs, ya = ys, za = zs;
    int xexp = -1074, yexp = -1074, zexp = -1074;
    if (xe != 0) { xa |= UINT64_C(1) << 52; xexp = (int)xe - 1023 - 52; }
    if (ye != 0) { ya |= UINT64_C(1) << 52; yexp = (int)ye - 1023 - 52; }
    if (ze != 0) { za |= UINT64_C(1) << 52; zexp = (int)ze - 1023 - 52; }
    int product_zero = xa == 0 || ya == 0;
    if (xinf || yinf || zinf)
        return mg_fma_double_value(mg_fma_special(0, xinf || yinf, xn != yn,
                                                  zinf, zn, 53, 1023));
    if (product_zero && za == 0) {
        int negative = (xn != yn) == zn ? (xn != yn) :
                       mg_fma_rounding_mode() == FE_DOWNWARD;
        mg_fma_result zero = {0, 0, negative};
        return mg_fma_double_value(zero);
    }
    if (product_zero)
        return z;
    mg_fma_result value = mg_fma_finite(xa, xexp, xn, ya, yexp, yn,
                                        za, zexp, zn, 53, -1022, 1023,
                                        MG_FMA_D_BASE, MG_FMA_D_WORDS);
    return mg_fma_double_value(value);
}

long double fmal(long double x, long double y, long double z)
{
    mg_x87_value xr = mg_x87_unpack(x), yr = mg_x87_unpack(y);
    mg_x87_value zr = mg_x87_unpack(z);
    unsigned xe = mg_x87_exponent(xr), ye = mg_x87_exponent(yr);
    unsigned ze = mg_x87_exponent(zr);
    uint64_t xs = xr.significand, ys = yr.significand, zs = zr.significand;
    int xn = mg_x87_negative(xr), yn = mg_x87_negative(yr);
    int zn = mg_x87_negative(zr);
    int xinf = xe == 0x7fffU && xs == mg_x87_integer_bit();
    int yinf = ye == 0x7fffU && ys == mg_x87_integer_bit();
    int zinf = ze == 0x7fffU && zs == mg_x87_integer_bit();
    int xnan = __mg_fpclassifyl(x) == FP_NAN;
    int ynan = __mg_fpclassifyl(y) == FP_NAN;
    int znan = __mg_fpclassifyl(z) == FP_NAN;
    if ((xinf && __mg_fpclassifyl(y) == FP_ZERO) ||
        (yinf && __mg_fpclassifyl(x) == FP_ZERO))
        return mg_fma_long_double_value(mg_fma_special(1, 0, xn != yn,
                                                        zinf, zn, 64, 16383));
    if (xnan || ynan || znan) {
        mg_x87_value chosen = xnan ? xr : (ynan ? yr : zr);
        int signaling = (xnan && (xe != 0x7fffU ||
                           !(xs & (UINT64_C(1) << 62)) ||
                           !(xs & mg_x87_integer_bit()))) ||
                        (ynan && (ye != 0x7fffU ||
                           !(ys & (UINT64_C(1) << 62)) ||
                           !(ys & mg_x87_integer_bit()))) ||
                        (znan && (ze != 0x7fffU ||
                           !(zs & (UINT64_C(1) << 62)) ||
                           !(zs & mg_x87_integer_bit())));
        if (signaling)
            (void)feraiseexcept(FE_INVALID);
        chosen.significand |= mg_x87_integer_bit() | (UINT64_C(1) << 62);
        chosen.sign_exponent = (uint16_t)((chosen.sign_exponent & 0x8000U) |
                                          0x7fffU);
        return mg_x87_pack(chosen);
    }
    if (xe == 0x7fffU) xs = 0;
    if (ye == 0x7fffU) ys = 0;
    if (ze == 0x7fffU) zs = 0;
    int product_zero = xs == 0 || ys == 0;
    if (xinf || yinf || zinf)
        return mg_fma_long_double_value(mg_fma_special(0, xinf || yinf,
                                                        xn != yn, zinf, zn,
                                                        64, 16383));
    if (product_zero && zs == 0) {
        int negative = (xn != yn) == zn ? (xn != yn) :
                       mg_fma_rounding_mode() == FE_DOWNWARD;
        mg_fma_result zero = {0, 0, negative};
        return mg_fma_long_double_value(zero);
    }
    if (product_zero)
        return z;
    int xexp = xe == 0 ? -16445 : (int)xe - 16383 - 63;
    int yexp = ye == 0 ? -16445 : (int)ye - 16383 - 63;
    int zexp = ze == 0 ? -16445 : (int)ze - 16383 - 63;
    mg_fma_result value = mg_fma_finite(xs, xexp, xn, ys, yexp, yn,
                                        zs, zexp, zn, 64, -16382, 16383,
                                        MG_FMA_L_BASE, MG_FMA_L_WORDS);
    return mg_fma_long_double_value(value);
}
