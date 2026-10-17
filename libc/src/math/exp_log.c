/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <float.h>
#include <math.h>

#if !defined(__x86_64__)
#error Mangrove exponential and logarithm functions require x86-64 x87
#endif

static long double mg_x87_times_log2e(long double value)
{
    long double result;
    __asm__ volatile("fldl2e\n\t"
                     "fldt %[value]\n\t"
                     "fmulp %%st, %%st(1)\n\t"
                     "fstpt %[result]"
                     : [result] "=m" (result)
                     : [value] "m" (value)
                     : "st", "st(1)", "memory");
    return result;
}

static long double mg_x87_exp2m1(long double value);

static long double mg_exp2_parts(long double high, long double low)
{
    int rounding = fegetround();
    int changed = rounding >= 0 && rounding != FE_TONEAREST;
    if (changed)
        (void)fesetround(FE_TONEAREST);

    long double upper = (long double)LDBL_MAX_EXP;
    long double lower = (long double)(LDBL_MIN_EXP - LDBL_MANT_DIG - 1);
    int overflow = high > upper || (high == upper && low >= 0.0L);
    int underflow = high < lower || (high == lower && low <= 0.0L);
    if (overflow || underflow) {
        if (changed)
            (void)fesetround(rounding);
        if (overflow)
            return scalbnl(1.0L, LDBL_MAX_EXP);
        return scalbnl(1.0L, LDBL_MIN_EXP - LDBL_MANT_DIG - 1);
    }

    long integral = (long)floorl(high);
    long double fraction = (high - (long double)integral) + low;
    if (fraction < 0.0L) {
        --integral;
        fraction += 1.0L;
    } else if (fraction >= 1.0L) {
        ++integral;
        fraction -= 1.0L;
    }
    if (changed)
        (void)fesetround(rounding);

    long double significand = 1.0L + mg_x87_exp2m1(fraction);
    return scalblnl(significand, integral);
}

/* Split log2(e) into an x87 high part and a residual. Dekker's product
 * transform recovers the rounding error in value*high; the residual product
 * retains the constant bits needed when value is large enough that a single
 * x87 multiplication would move the reduced fraction by hundreds of ulps. */
static void mg_exp_product(long double value, long double *high,
                           long double *low)
{
    const long double splitter = 0x1.00000001p+32L;
    const long double log2e_high = 0xb.8aa3b295c17f0bcp-3L;
    const long double log2e_low = -0x1.05e004be5b8b05dp-65L;
    int rounding = fegetround();
    int changed = rounding >= 0 && rounding != FE_TONEAREST;
    if (changed)
        (void)fesetround(FE_TONEAREST);

    long double value_split = splitter * value;
    long double value_high = value_split - (value_split - value);
    long double value_low = value - value_high;
    long double constant_split = splitter * log2e_high;
    long double constant_high = constant_split -
                                (constant_split - log2e_high);
    long double constant_low = log2e_high - constant_high;
    long double product = value * log2e_high;
    long double product_error =
        ((value_high * constant_high - product) +
         value_high * constant_low + value_low * constant_high) +
        value_low * constant_low;
    *high = product;
    *low = product_error + value * log2e_low;
    if (changed)
        (void)fesetround(rounding);
}

static long double mg_x87_exp2m1(long double value)
{
    long double result;
    __asm__ volatile("fldt %[value]\n\t"
                     "f2xm1\n\t"
                     "fstpt %[result]"
                     : [result] "=m" (result)
                     : [value] "m" (value)
                     : "st", "memory");
    return result;
}

static long double mg_x87_log2(long double value)
{
    long double result;
    __asm__ volatile("fld1\n\t"
                     "fldt %[value]\n\t"
                     "fyl2x\n\t"
                     "fstpt %[result]"
                     : [result] "=m" (result)
                     : [value] "m" (value)
                     : "st", "memory");
    return result;
}

static long double mg_x87_log(long double value)
{
    long double result;
    __asm__ volatile("fldln2\n\t"
                     "fldt %[value]\n\t"
                     "fyl2x\n\t"
                     "fstpt %[result]"
                     : [result] "=m" (result)
                     : [value] "m" (value)
                     : "st", "memory");
    return result;
}

static long double mg_x87_log10(long double value)
{
    long double result;
    __asm__ volatile("fldlg2\n\t"
                     "fldt %[value]\n\t"
                     "fyl2x\n\t"
                     "fstpt %[result]"
                     : [result] "=m" (result)
                     : [value] "m" (value)
                     : "st", "memory");
    return result;
}

static long double mg_x87_log1p_small(long double value)
{
    long double result;
    __asm__ volatile("fldln2\n\t"
                     "fldt %[value]\n\t"
                     "fyl2xp1\n\t"
                     "fstpt %[result]"
                     : [result] "=m" (result)
                     : [value] "m" (value)
                     : "st", "memory");
    return result;
}

static long double mg_exp2_finite(long double value)
{
    if (value > -1.0L && value < 0.0L)
        return 1.0L + mg_x87_exp2m1(value);
    return mg_exp2_parts(value, 0.0L);
}

static long double mg_exp_small_subnormal(long double value)
{
    (void)feraiseexcept(FE_INEXACT);
    int rounding = fegetround();
    if ((value > 0.0L && rounding == FE_UPWARD) ||
        (value < 0.0L && (rounding == FE_DOWNWARD ||
                          rounding == FE_TOWARDZERO)))
        return nextafterl(1.0L, value > 0.0L ? HUGE_VALL : 0.0L);
    return 1.0L;
}

long double exp2l(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE)
        return __mg_signbitl(value) ? 0.0L : value;
    if (category == FP_SUBNORMAL)
        return mg_exp_small_subnormal(value);
    return mg_exp2_finite(value);
}

long double expl(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE)
        return __mg_signbitl(value) ? 0.0L : value;
    if (category == FP_SUBNORMAL)
        return mg_exp_small_subnormal(value);
    if (value > 12000.0L)
        return scalbnl(1.0L, LDBL_MAX_EXP);
    if (value < -12000.0L)
        return scalbnl(1.0L, LDBL_MIN_EXP - LDBL_MANT_DIG - 1);
    if (value >= -0.5L && value <= 0.5L)
        return 1.0L + mg_x87_exp2m1(mg_x87_times_log2e(value));
    long double high, low;
    mg_exp_product(value, &high, &low);
    return mg_exp2_parts(high, low);
}

long double __mg_exp_compensated(long double high, long double low)
{
    /* These cutoffs are outside the x87 exp result range.  Checking them
     * before Dekker splitting also keeps large finite products away from the
     * splitter multiplication. */
    if (high > 12000.0L)
        return scalbnl(1.0L, LDBL_MAX_EXP);
    if (high < -12000.0L)
        return scalbnl(1.0L, LDBL_MIN_EXP - LDBL_MANT_DIG - 1);

    long double exponent_high, exponent_low;
    mg_exp_product(high, &exponent_high, &exponent_low);

    const long double log2e_high = 0xb.8aa3b295c17f0bcp-3L;
    const long double log2e_low = -0x1.05e004be5b8b05dp-65L;
    long double tail_high = low * log2e_high;
    long double tail_low = fmal(low, log2e_high, -tail_high) + low * log2e_low;

    long double sum = exponent_low + tail_high;
    long double sum_error = (exponent_low - sum) + tail_high;
    long double residual = sum_error + tail_low;
    long double combined = sum + residual;
    residual = (sum - combined) + residual;
    return mg_exp2_parts(exponent_high, combined + residual);
}

long double expm1l(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE)
        return __mg_signbitl(value) ? -1.0L : value;
    if (value > 12000.0L)
        return scalbnl(1.0L, LDBL_MAX_EXP);
    if (value <= -12000.0L) {
        (void)feraiseexcept(FE_INEXACT);
        int rounding = fegetround();
        if (rounding == FE_UPWARD || rounding == FE_TOWARDZERO)
            return nextafterl(-1.0L, 0.0L);
        return -1.0L;
    }
    if (value >= -0.5L && value <= 0.5L)
        return mg_x87_exp2m1(mg_x87_times_log2e(value));
    return expl(value) - 1.0L;
}

static long double mg_log_checked(long double value, int operation)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_ZERO) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return -HUGE_VALL;
    }
    if (__mg_signbitl(value)) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (category == FP_INFINITE)
        return HUGE_VALL;
    if (operation == 0)
        return mg_x87_log(value);
    if (operation == 1)
        return mg_x87_log2(value);
    return mg_x87_log10(value);
}

long double logl(long double value) { return mg_log_checked(value, 0); }
long double log2l(long double value) { return mg_log_checked(value, 1); }
long double log10l(long double value) { return mg_log_checked(value, 2); }

long double log1pl(long double value)
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
    if (value == -1.0L) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return -HUGE_VALL;
    }
    if (value < -1.0L) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (value == 0.0L)
        return value;
    if (value >= -0.25L && value <= 0.25L)
        return mg_x87_log1p_small(value);
    return logl(1.0L + value);
}

float exp2f(float x) { return (float)exp2l((long double)x); }
double exp2(double x) { return (double)exp2l((long double)x); }
float expf(float x) { return (float)expl((long double)x); }
double exp(double x) { return (double)expl((long double)x); }
float expm1f(float x) { return (float)expm1l((long double)x); }
double expm1(double x) { return (double)expm1l((long double)x); }
float logf(float x) { return (float)logl((long double)x); }
double log(double x) { return (double)logl((long double)x); }
float log2f(float x) { return (float)log2l((long double)x); }
double log2(double x) { return (double)log2l((long double)x); }
float log10f(float x) { return (float)log10l((long double)x); }
double log10(double x) { return (double)log10l((long double)x); }
float log1pf(float x) { return (float)log1pl((long double)x); }
double log1p(double x) { return (double)log1pl((long double)x); }
