/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"

#include <fenv.h>
#include <float.h>
#include <math.h>

static const long double mg_sqrt_pi =
    1.7724538509055160272981674833411451827975494561223871282138L;
static const long double mg_two_over_sqrt_pi =
    1.1283791670955125738961589031215451716881012586579977136882L;

/* For |x|<1, use the alternating Maclaurin series directly.  Its terms
 * decrease from the first ratio onward, and compensated summation controls
 * the only cancellation in the branch. */
static long double mg_erf_series(long double x)
{
    long double square = x * x;
    long double term = x;
    long double sum = x;
    long double compensation = 0.0L;

    for (unsigned index = 0; index < 64; ++index) {
        long double n = (long double)index;
        term *= -square * ((2.0L * n + 1.0L) /
                           ((n + 1.0L) * (2.0L * n + 3.0L)));
        long double adjusted = term - compensation;
        long double next = sum + adjusted;
        compensation = (next - sum) - adjusted;
        if (next == sum)
            break;
        sum = next;
    }
    return mg_two_over_sqrt_pi * sum;
}

/* Evaluate the direct positive-real erfc continued fraction by backward
 * recurrence. This keeps the small upper tail separate from 1-erf. The
 * recurrence converges faster as x grows, so its fixed depth is selected by
 * input range. The square residual is passed separately to compensated exp. */
static long double mg_erfc_fraction(long double x)
{
    long double square = x * x;
    long double square_error = fmal(x, x, -square);
    unsigned levels = x < 2.0L ? 512U : (x < 4.0L ? 256U : 128U);
    long double tail = 0.0L;
    for (unsigned index = levels; index != 0; --index) {
        long double step = (long double)index;
        tail = (step - 0.5L) /
               (1.0L + step / (square + tail));
    }
    return __mg_exp_compensated(-square, -square_error) *
           (x / (mg_sqrt_pi * (square + tail)));
}

static long double mg_erfc_positive(long double x)
{
    if (x == 0.0L)
        return 1.0L;
    if (x < 1.0L)
        return 1.0L - mg_erf_series(x);
    if (x >= 128.0L) {
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
        return 0.0L;
    }
    return mg_erfc_fraction(x);
}

long double erfl(long double x)
{
    int classification = __mg_fpclassifyl(x);
    if (classification == FP_NAN)
        return x + x;
    if (classification == FP_ZERO)
        return x;
    if (classification == FP_INFINITE)
        return __mg_signbitl(x) ? -1.0L : 1.0L;

    long double magnitude = fabsl(x);
    long double result;
    if (magnitude > 16.0L) {
        (void)feraiseexcept(FE_INEXACT);
        result = 1.0L;
    } else if (magnitude < 1.0L)
        result = mg_erf_series(magnitude);
    else
        result = 1.0L - mg_erfc_fraction(magnitude);
    return __mg_signbitl(x) ? -result : result;
}

long double erfcl(long double x)
{
    int classification = __mg_fpclassifyl(x);
    if (classification == FP_NAN)
        return x + x;
    if (classification == FP_INFINITE)
        return __mg_signbitl(x) ? 2.0L : 0.0L;
    if (x >= 0.0L)
        return mg_erfc_positive(x);

    long double magnitude = -x;
    if (magnitude >= 128.0L) {
        (void)feraiseexcept(FE_INEXACT);
        return 2.0L;
    }
    return 2.0L - mg_erfc_positive(magnitude);
}

float erff(float x)
{
    return (float)erfl((long double)x);
}

double erf(double x)
{
    return (double)erfl((long double)x);
}

float erfcf(float x)
{
    return (float)erfcl((long double)x);
}

double erfc(double x)
{
    return (double)erfcl((long double)x);
}
