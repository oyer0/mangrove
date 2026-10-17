/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <math.h>
#include <float.h>

#if !defined(__x86_64__)
#error Mangrove root functions require the x86-64 x87 long-double ABI
#endif

/* Normalize before iteration so the Newton steps stay in a small range.
 * The exponent is split using floor division, leaving the reduced argument
 * in [1/2, 4) and the final scale in an exact multiple of three. */
static long double mg_cbrt_core(long double x)
{
    int exponent;
    long double mantissa = frexpl(fabsl(x), &exponent);
    int remainder = exponent % 3;
    int scale = exponent / 3;
    if (remainder < 0) {
        remainder += 3;
        --scale;
    }
    long double reduced = scalbnl(mantissa, remainder);
    int negative = __mg_signbitl(x);
    if (negative)
        reduced = -reduced;

    /* Newton's map contracts quadratically over the normalized interval.
     * A fixed iteration count avoids data-dependent termination near a
     * two-value rounding cycle and reaches x87 precision from either end. */
    long double root = negative ? -1.0L : 1.0L;
    for (unsigned i = 0; i < 12; ++i)
        root = (2.0L * root + reduced / (root * root)) / 3.0L;
    return scalbnl(root, scale);
}

float cbrtf(float x)
{
    int category = __mg_fpclassifyf(x);
    if (category == FP_NAN)
        return x + x;
    if (category == FP_INFINITE || category == FP_ZERO)
        return x;
    return (float)mg_cbrt_core((long double)x);
}

double cbrt(double x)
{
    int category = __mg_fpclassify(x);
    if (category == FP_NAN)
        return x + x;
    if (category == FP_INFINITE || category == FP_ZERO)
        return x;
    return (double)mg_cbrt_core((long double)x);
}

long double cbrtl(long double x)
{
    int category = __mg_fpclassifyl(x);
    if (category == FP_NAN)
        return x + x;
    if (category == FP_INFINITE || category == FP_ZERO)
        return x;
    return mg_cbrt_core(x);
}

static long double mg_hypot_core(long double x, long double y,
                                 int precision, int minimum_exponent)
{
    x = fabsl(x);
    y = fabsl(y);
    if (x < y) {
        long double temporary = x;
        x = y;
        y = temporary;
    }
    if (y == 0.0L)
        return x;

    int exponent_gap = ilogbl(x) - ilogbl(y);
    if (exponent_gap > precision / 2 + 2) {
        /* The smaller square cannot change the rounded result at this
         * precision. Avoid forming an underflowing ratio for widely separated
         * subnormal and normal values. The exact result is still inexact; a
         * subnormal rounded result also raises underflow. */
        if (fegetround() == FE_UPWARD) {
            if (precision == FLT_MANT_DIG)
                return (long double)nextafterf((float)x, HUGE_VALF);
            if (precision == DBL_MANT_DIG)
                return (long double)nextafter((double)x, HUGE_VAL);
            return nextafterl(x, HUGE_VALL);
        }
        int exceptions = FE_INEXACT;
        if (ilogbl(x) < minimum_exponent - 1)
            exceptions |= FE_UNDERFLOW;
        (void)feraiseexcept(exceptions);
        return x;
    }

    /* Normalize both operands by an exact power of two. This avoids the
     * rounded y/x division in the correction term and keeps both squares
     * normal even when the inputs are subnormal. */
    int scale = ilogbl(x);
    long double normalized_x = scalbnl(x, -scale);
    long double normalized_y = scalbnl(y, -scale);
    long double sum = normalized_x * normalized_x +
                      normalized_y * normalized_y;
    return scalbnl(sqrtl(sum), scale);
}

static long double mg_hypot_checked(long double x, long double y,
                                    int x_category, int y_category,
                                    int precision, int minimum_exponent)
{
    /* C specifies the infinity result even when the other argument is NaN. */
    if (x_category == FP_INFINITE || y_category == FP_INFINITE)
        return HUGE_VALL;
    if (x_category == FP_NAN || y_category == FP_NAN)
        return x + y;
    return mg_hypot_core(x, y, precision, minimum_exponent);
}

float hypotf(float x, float y)
{
    return (float)mg_hypot_checked((long double)x, (long double)y,
                                    __mg_fpclassifyf(x), __mg_fpclassifyf(y),
                                    FLT_MANT_DIG, FLT_MIN_EXP);
}

double hypot(double x, double y)
{
    return (double)mg_hypot_checked((long double)x, (long double)y,
                                     __mg_fpclassify(x), __mg_fpclassify(y),
                                     DBL_MANT_DIG, DBL_MIN_EXP);
}

long double hypotl(long double x, long double y)
{
    return mg_hypot_checked(x, y, __mg_fpclassifyl(x), __mg_fpclassifyl(y),
                            LDBL_MANT_DIG, LDBL_MIN_EXP);
}
