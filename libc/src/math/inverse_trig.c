/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <float.h>
#include <math.h>

#if !defined(__x86_64__)
#error Mangrove inverse trigonometric functions require x86-64 x87 long double
#endif

static const long double mg_pi = 0xc.90fdaa22168c235p-2L;
static const long double mg_pi_over_2 = 0xc.90fdaa22168c235p-3L;
static const long double mg_pi_over_4 = 0xc.90fdaa22168c235p-4L;
static const long double mg_sqrt2_minus_1 =
    0x1.a827999fcef32422p-2L;

static long double mg_atan_series(long double value)
{
    long double square = value * value;
    long double term = value;
    long double sum = value;
    long double correction = 0.0L;
    for (unsigned odd = 3; odd <= 127; odd += 2) {
        term *= -square;
        long double addend = term / (long double)odd - correction;
        long double next = sum + addend;
        correction = (next - sum) - addend;
        sum = next;
        if (term == 0.0L)
            break;
    }
    return sum;
}

static long double mg_atan_positive(long double value)
{
    int reciprocal = 0;
    if (value > 1.0L) {
        value = 1.0L / value;
        reciprocal = 1;
    }

    long double offset = 0.0L;
    if (value > mg_sqrt2_minus_1) {
        value = (value - 1.0L) / (value + 1.0L);
        offset = mg_pi_over_4;
    }

    unsigned doublings = 0;
    while (value > 0.125L) {
        value /= 1.0L + sqrtl(1.0L + value * value);
        ++doublings;
    }
    long double reduced = mg_atan_series(value);
    while (doublings-- != 0)
        reduced *= 2.0L;
    long double result = offset + reduced;
    return reciprocal ? mg_pi_over_2 - result : result;
}

static long double mg_atan_core(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (category == FP_INFINITE)
        return __mg_signbitl(value) ? -mg_pi_over_2 : mg_pi_over_2;
    if (category == FP_ZERO)
        return value;

    (void)feraiseexcept(FE_INEXACT);
    long double result = mg_atan_positive(fabsl(value));
    return __mg_signbitl(value) ? -result : result;
}

static long double mg_atan2_core(long double y, long double x)
{
    int y_category = __mg_fpclassifyl(y);
    int x_category = __mg_fpclassifyl(x);
    int negative_y = __mg_signbitl(y);
    int negative_x = __mg_signbitl(x);

    if (y_category == FP_NAN || x_category == FP_NAN)
        return y + x;
    if (y_category == FP_ZERO) {
        if (negative_x)
            return negative_y ? -mg_pi : mg_pi;
        return y;
    }
    if (x_category == FP_ZERO)
        return negative_y ? -mg_pi_over_2 : mg_pi_over_2;

    int y_infinite = y_category == FP_INFINITE;
    int x_infinite = x_category == FP_INFINITE;
    (void)feraiseexcept(FE_INEXACT);
    if (y_infinite && x_infinite) {
        long double quadrant = negative_x ? 3.0L * mg_pi_over_4 : mg_pi_over_4;
        return negative_y ? -quadrant : quadrant;
    }
    if (y_infinite)
        return negative_y ? -mg_pi_over_2 : mg_pi_over_2;
    if (x_infinite) {
        if (negative_x)
            return negative_y ? -mg_pi : mg_pi;
        return negative_y ? -0.0L : 0.0L;
    }

    long double ay = fabsl(y);
    long double ax = fabsl(x);
    long double angle;
    if (ay <= ax) {
        long double reduced = mg_atan_positive(ay / ax);
        angle = negative_x ? mg_pi - reduced : reduced;
    } else {
        long double reduced = mg_atan_positive(ax / ay);
        angle = negative_x ? mg_pi_over_2 + reduced :
                             mg_pi_over_2 - reduced;
    }
    return negative_y ? -angle : angle;
}

static long double mg_asin_core(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (fabsl(value) > 1.0L || category == FP_INFINITE) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (category == FP_ZERO)
        return value;
    long double magnitude = fabsl(value);
    long double perpendicular = sqrtl((1.0L - magnitude) *
                                       (1.0L + magnitude));
    return mg_atan2_core(value, perpendicular);
}

static long double mg_acos_core(long double value)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN)
        return value + value;
    if (fabsl(value) > 1.0L || category == FP_INFINITE) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (value == 1.0L)
        return 0.0L;
    long double magnitude = fabsl(value);
    long double perpendicular = sqrtl((1.0L - magnitude) *
                                       (1.0L + magnitude));
    return mg_atan2_core(perpendicular, value);
}

long double atanl(long double value) { return mg_atan_core(value); }
float atanf(float value) { return (float)mg_atan_core((long double)value); }
double atan(double value) { return (double)mg_atan_core((long double)value); }
long double atan2l(long double y, long double x) { return mg_atan2_core(y, x); }
float atan2f(float y, float x)
{ return (float)mg_atan2_core((long double)y, (long double)x); }
double atan2(double y, double x)
{ return (double)mg_atan2_core((long double)y, (long double)x); }
long double asinl(long double value) { return mg_asin_core(value); }
float asinf(float value) { return (float)mg_asin_core((long double)value); }
double asin(double value) { return (double)mg_asin_core((long double)value); }
long double acosl(long double value) { return mg_acos_core(value); }
float acosf(float value) { return (float)mg_acos_core((long double)value); }
double acos(double value) { return (double)mg_acos_core((long double)value); }
