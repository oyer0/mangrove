/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <math.h>

static void mg_next_raise(int x_class, int result_class)
{
    if (result_class == FP_INFINITE && x_class != FP_INFINITE)
        (void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);
    else if (result_class == FP_SUBNORMAL || result_class == FP_ZERO)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
}

float nextafterf(float x, float y)
{
    int cx = __mg_fpclassifyf(x), cy = __mg_fpclassifyf(y);
    if (cx == FP_NAN || cy == FP_NAN)
        return x + y;
    if (x == y)
        return y;
    uint32_t raw = mg_float_bits(x);
    if (cx == FP_ZERO) {
        raw = (mg_float_bits(y) & UINT32_C(0x80000000)) | 1U;
        float result = mg_float_from_bits(raw);
        mg_next_raise(cx, __mg_fpclassifyf(result));
        return result;
    } else if ((x > 0.0f) == (y > x)) {
        ++raw;
    } else {
        --raw;
    }
    float result = mg_float_from_bits(raw);
    mg_next_raise(cx, __mg_fpclassifyf(result));
    return result;
}

double nextafter(double x, double y)
{
    int cx = __mg_fpclassify(x), cy = __mg_fpclassify(y);
    if (cx == FP_NAN || cy == FP_NAN)
        return x + y;
    if (x == y)
        return y;
    uint64_t raw = mg_double_bits(x);
    if (cx == FP_ZERO) {
        raw = (mg_double_bits(y) & UINT64_C(0x8000000000000000)) | 1U;
        double result = mg_double_from_bits(raw);
        mg_next_raise(cx, __mg_fpclassify(result));
        return result;
    } else if ((x > 0.0) == (y > x)) {
        ++raw;
    } else {
        --raw;
    }
    double result = mg_double_from_bits(raw);
    mg_next_raise(cx, __mg_fpclassify(result));
    return result;
}

static mg_x87_value mg_x87_step(mg_x87_value value, int increase_magnitude)
{
    unsigned exponent = mg_x87_exponent(value);
    uint64_t integer_bit = mg_x87_integer_bit();
    if (exponent == 0x7fffU) {
        if (value.significand != integer_bit || increase_magnitude)
            return value;
        value.sign_exponent = (value.sign_exponent & 0x8000U) | 0x7ffeU;
        value.significand = UINT64_MAX;
        return value;
    }

    if (exponent == 0 && (value.significand & integer_bit)) {
        /* Canonicalize x87's equivalent pseudo-denormal representation. */
        value.sign_exponent = (value.sign_exponent & 0x8000U) | 1U;
        exponent = 1;
    }

    if (increase_magnitude) {
        if (exponent == 0) {
            ++value.significand;
            if (value.significand == integer_bit)
                value.sign_exponent = (value.sign_exponent & 0x8000U) | 1U;
            return value;
        }
        if (value.significand == UINT64_MAX) {
            if (exponent == 0x7ffeU) {
                value.sign_exponent = (value.sign_exponent & 0x8000U) | 0x7fffU;
                value.significand = integer_bit;
            } else {
                value.sign_exponent = (value.sign_exponent & 0x8000U) |
                                      (uint16_t)(exponent + 1U);
                value.significand = integer_bit;
            }
        } else {
            ++value.significand;
        }
        return value;
    }

    if (exponent == 0) {
        --value.significand;
        return value;
    }
    if (value.significand == integer_bit) {
        if (exponent == 1) {
            value.sign_exponent &= 0x8000U;
            value.significand = integer_bit - 1U;
        } else {
            value.sign_exponent = (value.sign_exponent & 0x8000U) |
                                  (uint16_t)(exponent - 1U);
            value.significand = UINT64_MAX;
        }
    } else {
        --value.significand;
    }
    return value;
}

long double nextafterl(long double x, long double y)
{
    int cx = __mg_fpclassifyl(x), cy = __mg_fpclassifyl(y);
    if (cx == FP_NAN || cy == FP_NAN)
        return x + y;
    if (x == y)
        return y;
    if (cx == FP_ZERO) {
        mg_x87_value raw = mg_x87_unpack(y);
        raw.significand = 1;
        raw.sign_exponent &= 0x8000U;
        long double result = mg_x87_pack(raw);
        mg_next_raise(cx, __mg_fpclassifyl(result));
        return result;
    }
    mg_x87_value raw = mg_x87_unpack(x);
    int negative = mg_x87_negative(raw);
    int increase = negative ? y < x : y > x;
    raw = mg_x87_step(raw, increase);
    long double result = mg_x87_pack(raw);
    mg_next_raise(cx, __mg_fpclassifyl(result));
    return result;
}

float nexttowardf(float x, long double y)
{
    if (__mg_fpclassifyl(y) == FP_NAN || __mg_fpclassifyf(x) == FP_NAN)
        return (float)((long double)x + y);
    long double extended_x = (long double)x;
    if (extended_x == y)
        return copysignf(x, (float)y);
    return nextafterf(x, y > extended_x ? HUGE_VALF : -HUGE_VALF);
}

double nexttoward(double x, long double y)
{
    if (__mg_fpclassifyl(y) == FP_NAN || __mg_fpclassify(x) == FP_NAN)
        return (double)((long double)x + y);
    long double extended_x = (long double)x;
    if (extended_x == y)
        return copysign(x, (double)y);
    return nextafter(x, y > extended_x ? HUGE_VAL : -HUGE_VAL);
}

long double nexttowardl(long double x, long double y)
{
    if (x == y)
        return copysignl(x, y);
    return nextafterl(x, y);
}
