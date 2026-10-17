/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <float.h>
#include <math.h>

#if !defined(__x86_64__)
#error Mangrove hyperbolic functions require x86-64 x87 long double
#endif

static const long double mg_ln2 = 0xb.17217f7d1cf79acp-4L;

/* Split value-ln(2) into a rounded high part and its subtraction residual.
 * For large value, one ordinary subtraction discards many output ulps even
 * though the correction is only a constant.  Evaluate the residual as a
 * separate, near-one exponential instead. */
static long double mg_exp_minus_ln2(long double value)
{
    long double high = value - mg_ln2;
    long double low = (value - high) - mg_ln2;
    return expl(high) * expl(low);
}

static long double mg_sinh_core(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE || category == FP_ZERO)
        return value;

    long double magnitude = fabsl(value);
    long double result;
    if (magnitude < 1.0L) {
        long double change = expm1l(magnitude);
        result = 0.5L * (change + change / (1.0L + change));
    } else {
        result = mg_exp_minus_ln2(magnitude) - 0.5L * expl(-magnitude);
    }
    return __mg_signbitl(value) ? -result : result;
}

static long double mg_cosh_core(long double value, int precision)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE)
        return HUGE_VALL;

    long double magnitude = fabsl(value);
    if (category == FP_ZERO)
        return 1.0L;
    long double half_ulp_threshold = sqrtl(scalbnl(1.0L, 1 - precision));
    half_ulp_threshold = nextafterl(half_ulp_threshold, 0.0L);
    if (magnitude <= half_ulp_threshold) {
        (void)feraiseexcept(FE_INEXACT);
        return fegetround() == FE_UPWARD ? nextafterl(1.0L, HUGE_VALL) :
                                           1.0L;
    }
    if (magnitude < 1.0L) {
        long double half_sinh = mg_sinh_core(0.5L * magnitude);
        return 1.0L + 2.0L * half_sinh * half_sinh;
    }
    return mg_exp_minus_ln2(magnitude) + 0.5L * expl(-magnitude);
}

static long double mg_tanh_limit(int negative, int precision)
{
    int rounding = fegetround();
    int toward_zero_side = negative ?
        (rounding == FE_UPWARD || rounding == FE_TOWARDZERO) :
        (rounding == FE_DOWNWARD || rounding == FE_TOWARDZERO);
    if (toward_zero_side)
        return nextafterl(negative ? -1.0L : 1.0L, 0.0L);
    (void)precision;
    return negative ? -1.0L : 1.0L;
}

static long double mg_tanh_core(long double value, int precision)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE)
        return __mg_signbitl(value) ? -1.0L : 1.0L;
    if (category == FP_ZERO)
        return value;

    int negative = __mg_signbitl(value);
    long double magnitude = fabsl(value);
    /* At this point the omitted distance from one is smaller than half an
     * ulp of the requested result format.  Directed modes still choose the
     * adjacent value on the side facing the exact result. */
    long double saturation = (long double)(precision + 2) * mg_ln2 * 0.5L;
    if (magnitude > saturation)
        return mg_tanh_limit(negative, precision);

    long double change = expm1l(-2.0L * magnitude);
    long double result = -change / (2.0L + change);
    return negative ? -result : result;
}

static long double mg_asinh_core(long double value, int precision)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_ZERO)
        return value;
    if (category == FP_INFINITE)
        return value;

    long double magnitude = fabsl(value);
    long double result;
    if (magnitude < 0.5L) {
        long double square = magnitude * magnitude;
        result = log1pl(magnitude + square /
                        (1.0L + sqrtl(1.0L + square)));
    } else if (magnitude > scalbnl(1.0L, precision / 2 + 2)) {
        result = logl(magnitude) + mg_ln2;
    } else {
        long double hypotenuse = hypotl(magnitude, 1.0L);
        result = logl(magnitude + hypotenuse);
    }
    return __mg_signbitl(value) ? -result : result;
}

static long double mg_acosh_core(long double value, int precision)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE) {
        if (!__mg_signbitl(value))
            return value;
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (value < 1.0L) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (value == 1.0L)
        return 0.0L;
    if (value > scalbnl(1.0L, precision / 2 + 2))
        return logl(value) + mg_ln2;

    long double distance = value - 1.0L;
    long double root = sqrtl(distance) * sqrtl(value + 1.0L);
    return log1pl(distance + root);
}

static long double mg_atanh_core(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_ZERO)
        return value;
    if (category == FP_INFINITE || fabsl(value) > 1.0L) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (value == 1.0L || value == -1.0L) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return __mg_signbitl(value) ? -HUGE_VALL : HUGE_VALL;
    }

    long double magnitude = fabsl(value);
    if (magnitude <= 0.25L) {
        long double square = magnitude * magnitude;
        long double term = magnitude;
        long double sum = magnitude;
        long double correction = 0.0L;
        for (unsigned odd = 3; odd <= 63; odd += 2) {
            term *= square;
            long double addend = term / (long double)odd - correction;
            long double next = sum + addend;
            correction = (next - sum) - addend;
            sum = next;
        }
        return __mg_signbitl(value) ? -sum : sum;
    }
    long double ratio = (2.0L * magnitude) / (1.0L - magnitude);
    long double result = 0.5L * log1pl(ratio);
    return __mg_signbitl(value) ? -result : result;
}

long double sinhl(long double x) { return mg_sinh_core(x); }
long double coshl(long double x) { return mg_cosh_core(x, LDBL_MANT_DIG); }
long double tanhl(long double x) { return mg_tanh_core(x, LDBL_MANT_DIG); }
long double asinhl(long double x) { return mg_asinh_core(x, LDBL_MANT_DIG); }
long double acoshl(long double x) { return mg_acosh_core(x, LDBL_MANT_DIG); }
long double atanhl(long double x) { return mg_atanh_core(x); }

float sinhf(float x) { return (float)sinhl((long double)x); }
double sinh(double x) { return (double)sinhl((long double)x); }
float coshf(float x) { return (float)mg_cosh_core((long double)x, FLT_MANT_DIG); }
double cosh(double x) { return (double)mg_cosh_core((long double)x, DBL_MANT_DIG); }
float tanhf(float x) { return (float)mg_tanh_core((long double)x, FLT_MANT_DIG); }
double tanh(double x) { return (double)mg_tanh_core((long double)x, DBL_MANT_DIG); }
float asinhf(float x) { return (float)mg_asinh_core((long double)x, FLT_MANT_DIG); }
double asinh(double x) { return (double)mg_asinh_core((long double)x, DBL_MANT_DIG); }
float acoshf(float x) { return (float)mg_acosh_core((long double)x, FLT_MANT_DIG); }
double acosh(double x) { return (double)mg_acosh_core((long double)x, DBL_MANT_DIG); }
float atanhf(float x) { return (float)mg_atanh_core((long double)x); }
double atanh(double x) { return (double)mg_atanh_core((long double)x); }
