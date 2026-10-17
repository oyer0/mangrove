/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <math.h>

#define MG_BINARY_BASIC(suffix, type, bits_type, bits, from_bits, sign_mask) \
type fabs##suffix(type x) \
{ return from_bits(bits(x) & ~(bits_type)(sign_mask)); } \
type copysign##suffix(type x, type y) \
{ return from_bits((bits(x) & ~(bits_type)(sign_mask)) | \
                   (bits(y) & (bits_type)(sign_mask))); }

MG_BINARY_BASIC(f, float, uint32_t, mg_float_bits, mg_float_from_bits, UINT32_C(0x80000000))
MG_BINARY_BASIC(, double, uint64_t, mg_double_bits, mg_double_from_bits, UINT64_C(0x8000000000000000))

long double fabsl(long double x)
{
    mg_x87_value raw = mg_x87_unpack(x);
    raw.sign_exponent &= 0x7fffU;
    return mg_x87_pack(raw);
}

long double copysignl(long double x, long double y)
{
    mg_x87_value a = mg_x87_unpack(x);
    mg_x87_value b = mg_x87_unpack(y);
    a.sign_exponent = (a.sign_exponent & 0x7fffU) |
                      (b.sign_exponent & 0x8000U);
    return mg_x87_pack(a);
}

#define MG_MINMAX(suffix, type, classify, signbit_fn, copysign_fn) \
type fmin##suffix(type x, type y) \
{ \
    int cx = classify(x), cy = classify(y); \
    if (cx == FP_NAN) return cy == FP_NAN ? x : y; \
    if (cy == FP_NAN) return x; \
    if (x < y) return x; \
    if (y < x) return y; \
    if (x == (type)0 && y == (type)0) \
        return copysign_fn((type)0, (signbit_fn(x) || signbit_fn(y)) ? (type)-1 : (type)1); \
    return x; \
} \
type fmax##suffix(type x, type y) \
{ \
    int cx = classify(x), cy = classify(y); \
    if (cx == FP_NAN) return cy == FP_NAN ? x : y; \
    if (cy == FP_NAN) return x; \
    if (x > y) return x; \
    if (y > x) return y; \
    if (x == (type)0 && y == (type)0) \
        return copysign_fn((type)0, (!signbit_fn(x) || !signbit_fn(y)) ? (type)1 : (type)-1); \
    return x; \
} \
type fdim##suffix(type x, type y) \
{ \
    if (classify(x) == FP_NAN || classify(y) == FP_NAN) return x + y; \
    return x > y ? x - y : (type)0; \
}

MG_MINMAX(f, float, __mg_fpclassifyf, __mg_signbitf, copysignf)
MG_MINMAX(, double, __mg_fpclassify, __mg_signbit, copysign)
MG_MINMAX(l, long double, __mg_fpclassifyl, __mg_signbitl, copysignl)

/* These operations edit only discarded significand bits.  They therefore
 * preserve subnormals and signed zero without depending on the active rounding
 * mode or converting through an integer type. */
#define MG_ROUND_BITS(suffix, type, uint_type, bits, from_bits, sign_mask, \
                      exponent_mask, fraction_mask, bias, fraction_bits, one_bits) \
static type mg_round_##suffix(type x, int operation) \
{ \
    uint_type raw = bits(x); \
    uint_type sign = raw & (uint_type)(sign_mask); \
    unsigned field = (unsigned)((raw & (uint_type)(exponent_mask)) >> (fraction_bits)); \
    if (field == (unsigned)(((uint_type)(exponent_mask)) >> (fraction_bits))) return x; \
    if ((raw & ~(uint_type)(sign_mask)) == 0) return x; \
    int exponent = (int)field - (bias); \
    int negative = sign != 0; \
    int mode = operation; \
    if (operation == 5 || operation == 6) { \
        int rounding = fegetround(); \
        mode = rounding == FE_DOWNWARD ? 1 : \
               rounding == FE_UPWARD ? 2 : \
               rounding == FE_TOWARDZERO ? 0 : 4; \
    } \
    uint_type integer; \
    int discarded; \
    if (exponent < 0) { \
        discarded = 1; \
        int increment = mode == 1 ? negative : mode == 2 ? !negative : 0; \
        if (mode == 3) increment = exponent == -1; \
        if (mode == 4) increment = exponent == -1 && \
            (raw & (uint_type)(fraction_mask)) != 0; \
        if (increment) \
            integer = sign | (uint_type)(one_bits); \
        else \
            integer = sign; \
    } else { \
        if (exponent >= (fraction_bits)) return x; \
        unsigned count = (unsigned)((fraction_bits) - exponent); \
        if (count == 0) return x; \
        uint_type mask = (((uint_type)1 << count) - 1); \
        uint_type fraction = raw & mask; \
        if (fraction == 0) return x; \
        discarded = 1; \
        uint_type unit = (uint_type)1 << count; \
        int increment = mode == 1 ? negative : mode == 2 ? !negative : 0; \
        if (mode == 3) \
            increment = fraction >= (unit >> 1); \
        if (mode == 4) { \
            uint_type half = unit >> 1; \
            increment = fraction > half || (fraction == half && (raw & unit) != 0); \
        } \
        integer = raw & ~mask; \
        if (increment) integer += unit; \
    } \
    if (operation == 6) (void)feraiseexcept(FE_INEXACT); \
    (void)discarded; \
    return from_bits(integer); \
} \
type trunc##suffix(type x) { return mg_round_##suffix(x, 0); } \
type floor##suffix(type x) { return mg_round_##suffix(x, 1); } \
type ceil##suffix(type x) { return mg_round_##suffix(x, 2); } \
type round##suffix(type x) { return mg_round_##suffix(x, 3); } \
type nearbyint##suffix(type x) { return mg_round_##suffix(x, 5); } \
type rint##suffix(type x) { return mg_round_##suffix(x, 6); }

MG_ROUND_BITS(f, float, uint32_t, mg_float_bits, mg_float_from_bits,
              UINT32_C(0x80000000), UINT32_C(0x7f800000),
              UINT32_C(0x007fffff), 127, 23, UINT32_C(0x3f800000))
MG_ROUND_BITS(, double, uint64_t, mg_double_bits, mg_double_from_bits,
              UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
              UINT64_C(0x000fffffffffffff), 1023, 52,
              UINT64_C(0x3ff0000000000000))

static long double mg_round_x87(long double x, int operation)
{
    mg_x87_value raw = mg_x87_unpack(x);
    unsigned field = mg_x87_exponent(raw);
    int negative = mg_x87_negative(raw);
    if (field == 0x7fffU || raw.significand == 0)
        return x;

    int exponent = field ? (int)field - 16383 : -16382;
    if (field == 0 && (raw.significand & mg_x87_integer_bit()))
        exponent = -16382; /* x87 pseudo-denormal has the value of exponent one. */
    int mode = operation;
    if (operation == 5 || operation == 6) {
        int rounding = fegetround();
        mode = rounding == FE_DOWNWARD ? 1 :
               rounding == FE_UPWARD ? 2 :
               rounding == FE_TOWARDZERO ? 0 : 4;
    }

    int discarded = 0;
    int increment = 0;
    if (exponent < 0) {
        discarded = 1;
        increment = mode == 1 ? negative : mode == 2 ? !negative : 0;
        if (mode == 3) increment = exponent == -1;
        /* At a half tie, zero is even. */
        if (mode == 4)
            increment = exponent == -1 && raw.significand > mg_x87_integer_bit();
        raw.significand = increment ? mg_x87_integer_bit() : 0;
        raw.sign_exponent = (raw.sign_exponent & 0x8000U) |
                            (increment ? 0x3fffU : 0U);
    } else if (exponent < 63) {
        unsigned count = (unsigned)(63 - exponent);
        uint64_t unit = UINT64_C(1) << count;
        uint64_t mask = unit - 1;
        uint64_t fraction = raw.significand & mask;
        if (!fraction)
            return x;
        discarded = 1;
        increment = mode == 1 ? negative : mode == 2 ? !negative : 0;
        if (mode == 3)
            increment = fraction >= (unit >> 1);
        if (mode == 4) {
            uint64_t half = unit >> 1;
            increment = fraction > half ||
                        (fraction == half && (raw.significand & unit));
        }
        raw.significand &= ~mask;
        if (increment) {
            uint64_t rounded = raw.significand + unit;
            if (rounded == 0) {
                /* The rounded magnitude crossed a power-of-two boundary.
                 * Unlike binary32/64, x87 stores its significand separately
                 * from the exponent, so the carry must be published here. */
                raw.significand = mg_x87_integer_bit();
                raw.sign_exponent = (uint16_t)((raw.sign_exponent & 0x8000U) |
                                               ((field + 1U) & 0x7fffU));
            } else {
                raw.significand = rounded;
            }
        }
    } else {
        return x;
    }
    if (operation == 6 && discarded)
        (void)feraiseexcept(FE_INEXACT);
    return mg_x87_pack(raw);
}

long double truncl(long double x) { return mg_round_x87(x, 0); }
long double floorl(long double x) { return mg_round_x87(x, 1); }
long double ceill(long double x) { return mg_round_x87(x, 2); }
long double roundl(long double x) { return mg_round_x87(x, 3); }
long double nearbyintl(long double x) { return mg_round_x87(x, 5); }
long double rintl(long double x) { return mg_round_x87(x, 6); }

#define MG_FREXP(suffix, type, uint_type, bits, from_bits, sign_mask, \
                 exponent_mask, fraction_mask, bias, fraction_bits) \
type frexp##suffix(type x, int *result_exponent) \
{ \
    uint_type raw = bits(x); \
    unsigned field = (unsigned)((raw & (uint_type)(exponent_mask)) >> (fraction_bits)); \
    uint_type fraction = raw & (uint_type)(fraction_mask); \
    if (field == (unsigned)(((uint_type)(exponent_mask)) >> (fraction_bits))) { \
        *result_exponent = 0; return x; \
    } \
    if (field == 0) { \
        if (fraction == 0) { *result_exponent = 0; return x; } \
        unsigned top = 0; \
        for (uint_type scan = fraction; scan >>= 1; ) ++top; \
        unsigned shift = (unsigned)(fraction_bits) - top; \
        uint_type normalized = fraction << shift; \
        *result_exponent = 2 - (bias) - (int)shift; \
        return from_bits((raw & (uint_type)(sign_mask)) | \
            ((uint_type)((bias) - 1) << (fraction_bits)) | \
            (normalized & (uint_type)(fraction_mask))); \
    } \
    *result_exponent = (int)field - (bias) + 1; \
    return from_bits((raw & ~((uint_type)(exponent_mask))) | \
        ((uint_type)((bias) - 1) << (fraction_bits))); \
}

MG_FREXP(f, float, uint32_t, mg_float_bits, mg_float_from_bits,
         UINT32_C(0x80000000), UINT32_C(0x7f800000), UINT32_C(0x007fffff), 127, 23)
MG_FREXP(, double, uint64_t, mg_double_bits, mg_double_from_bits,
         UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
         UINT64_C(0x000fffffffffffff), 1023, 52)

long double frexpl(long double x, int *result_exponent)
{
    mg_x87_value raw = mg_x87_unpack(x);
    unsigned field = mg_x87_exponent(raw);
    uint64_t integer_bit = mg_x87_integer_bit();
    if (field == 0x7fffU) {
        *result_exponent = 0;
        return x;
    }
    if (field != 0) {
        *result_exponent = (int)field - 16383 + 1;
        raw.sign_exponent = (raw.sign_exponent & 0x8000U) | 0x3ffeU;
        return mg_x87_pack(raw);
    }
    if (raw.significand == 0) {
        *result_exponent = 0;
        return x;
    }
    unsigned top = 0;
    for (uint64_t scan = raw.significand; scan >>= 1; ) ++top;
    unsigned shift = 63U - top;
    raw.significand <<= shift;
    raw.sign_exponent = (raw.sign_exponent & 0x8000U) | 0x3ffeU;
    *result_exponent = -16381 - (int)shift;
    (void)integer_bit;
    return mg_x87_pack(raw);
}

#define MG_ILOGB(suffix, type, uint_type, bits, exponent_mask, fraction_mask, \
                 bias, fraction_bits, special_exp) \
int ilogb##suffix(type x) \
{ \
    uint_type raw = bits(x); \
    unsigned field = (unsigned)((raw & (uint_type)(exponent_mask)) >> (fraction_bits)); \
    uint_type fraction = raw & (uint_type)(fraction_mask); \
    if (field == (special_exp)) { \
        (void)feraiseexcept(FE_INVALID); \
        return fraction ? FP_ILOGBNAN : INT_MAX; \
    } \
    if (field) return (int)field - (bias); \
    if (!fraction) { (void)feraiseexcept(FE_INVALID); return FP_ILOGB0; } \
    int top = -1; \
    for (uint_type scan = fraction; scan; scan >>= 1) ++top; \
    return 1 - (bias) - (fraction_bits) + top; \
}

MG_ILOGB(f, float, uint32_t, mg_float_bits, UINT32_C(0x7f800000),
         UINT32_C(0x007fffff), 127, 23, 0xffU)
MG_ILOGB(, double, uint64_t, mg_double_bits, UINT64_C(0x7ff0000000000000),
         UINT64_C(0x000fffffffffffff), 1023, 52, 0x7ffU)

int ilogbl(long double x)
{
    mg_x87_value raw = mg_x87_unpack(x);
    unsigned field = mg_x87_exponent(raw);
    uint64_t integer_bit = mg_x87_integer_bit();
    if (field == 0x7fffU) {
        (void)feraiseexcept(FE_INVALID);
        return raw.significand == integer_bit ? INT_MAX : FP_ILOGBNAN;
    }
    if (field)
        return (int)field - 16383;
    if (!raw.significand) {
        (void)feraiseexcept(FE_INVALID);
        return FP_ILOGB0;
    }
    int top = -1;
    for (uint64_t scan = raw.significand; scan; scan >>= 1) ++top;
    return -16445 + top;
}

float logbf(float x)
{
    int category = __mg_fpclassifyf(x);
    if (category == FP_NAN) return x + x;
    if (category == FP_INFINITE) return HUGE_VALF;
    if (category == FP_ZERO) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return -HUGE_VALF;
    }
    return (float)ilogbf(x);
}

double logb(double x)
{
    int category = __mg_fpclassify(x);
    if (category == FP_NAN) return x + x;
    if (category == FP_INFINITE) return HUGE_VAL;
    if (category == FP_ZERO) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return -HUGE_VAL;
    }
    return (double)ilogb(x);
}

long double logbl(long double x)
{
    int category = __mg_fpclassifyl(x);
    if (category == FP_NAN) return x + x;
    if (category == FP_INFINITE) return HUGE_VALL;
    if (category == FP_ZERO) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return -HUGE_VALL;
    }
    return (long double)ilogbl(x);
}

#define MG_MODF(suffix, type, uint_type, bits, from_bits, sign_mask, \
                exponent_mask, fraction_bits, bias, one_bits) \
type modf##suffix(type x, type *integer_part) \
{ \
    uint_type raw = bits(x); \
    unsigned field = (unsigned)((raw & (uint_type)(exponent_mask)) >> (fraction_bits)); \
    if (field == (unsigned)(((uint_type)(exponent_mask)) >> (fraction_bits))) { \
        *integer_part = x; \
        return (raw & ~((uint_type)(sign_mask) | (uint_type)(exponent_mask))) ? x + x : \
               from_bits(raw & (uint_type)(sign_mask)); \
    } \
    int exponent = field ? (int)field - (bias) : 1 - (bias); \
    if (exponent < 0) { \
        *integer_part = from_bits(raw & (uint_type)(sign_mask)); return x; \
    } \
    if (exponent >= (fraction_bits)) { \
        *integer_part = x; return from_bits(raw & (uint_type)(sign_mask)); \
    } \
    unsigned count = (unsigned)((fraction_bits) - exponent); \
    uint_type mask = ((uint_type)1 << count) - 1; \
    uint_type whole = raw & ~mask; \
    *integer_part = from_bits(whole); \
    return x - *integer_part; \
}

MG_MODF(f, float, uint32_t, mg_float_bits, mg_float_from_bits,
        UINT32_C(0x80000000), UINT32_C(0x7f800000), 23, 127,
        UINT32_C(0x3f800000))
MG_MODF(, double, uint64_t, mg_double_bits, mg_double_from_bits,
        UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
        52, 1023, UINT64_C(0x3ff0000000000000))

long double modfl(long double x, long double *integer_part)
{
    mg_x87_value raw = mg_x87_unpack(x);
    unsigned field = mg_x87_exponent(raw);
    if (field == 0x7fffU) {
        *integer_part = x;
        if (raw.significand != mg_x87_integer_bit())
            return x + x;
        raw.significand = 0;
        raw.sign_exponent &= 0x8000U;
        return mg_x87_pack(raw);
    }
    int exponent = field ? (int)field - 16383 : -16382;
    if (exponent < 0) {
        raw.significand = 0;
        raw.sign_exponent &= 0x8000U;
        *integer_part = mg_x87_pack(raw);
        return x;
    }
    if (exponent >= 63) {
        *integer_part = x;
        raw.significand = 0;
        raw.sign_exponent &= 0x8000U;
        return mg_x87_pack(raw);
    }
    unsigned count = (unsigned)(63 - exponent);
    uint64_t mask = (UINT64_C(1) << count) - 1;
    raw.significand &= ~mask;
    *integer_part = mg_x87_pack(raw);
    return x - *integer_part;
}
