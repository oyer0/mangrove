/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>

typedef struct {
    long double high;
    long double low;
} mg_double_part;

static mg_double_part mg_sum_parts(long double left, long double right)
{
    long double sum = left + right;
    long double right_virtual = sum - left;
    long double error = (left - (sum - right_virtual)) +
                        (right - right_virtual);
    return (mg_double_part){sum, error};
}

static mg_double_part mg_parts_add(mg_double_part left, mg_double_part right)
{
    mg_double_part leading = mg_sum_parts(left.high, right.high);
    long double trailing = leading.low + left.low + right.low;
    return mg_sum_parts(leading.high, trailing);
}

static mg_double_part mg_parts_multiply(mg_double_part left,
                                         mg_double_part right)
{
    long double product = left.high * right.high;
    long double error = fmal(left.high, right.high, -product);
    error += left.high * right.low + left.low * right.high;
    return mg_sum_parts(product, error);
}

static mg_double_part mg_parts_divide(mg_double_part numerator,
                                       mg_double_part denominator)
{
    long double first = numerator.high / denominator.high;
    mg_double_part product = mg_parts_multiply(
        denominator, (mg_double_part){first, 0.0L});
    mg_double_part remainder = mg_parts_add(
        numerator, (mg_double_part){-product.high, -product.low});
    long double second = (remainder.high + remainder.low) / denominator.high;
    mg_double_part quotient = mg_parts_add((mg_double_part){first, 0.0L},
                                           (mg_double_part){second, 0.0L});
    product = mg_parts_multiply(denominator, quotient);
    remainder = mg_parts_add(numerator,
                             (mg_double_part){-product.high, -product.low});
    long double third = (remainder.high + remainder.low) / denominator.high;
    return mg_parts_add(quotient, (mg_double_part){third, 0.0L});
}

static mg_double_part mg_log_series(long double mantissa)
{
    mg_double_part numerator = mg_parts_add((mg_double_part){mantissa, 0.0L},
                                             (mg_double_part){-1.0L, 0.0L});
    mg_double_part denominator = mg_parts_add((mg_double_part){mantissa, 0.0L},
                                               (mg_double_part){1.0L, 0.0L});
    mg_double_part z = mg_parts_divide(numerator, denominator);
    mg_double_part square = mg_parts_multiply(z, z);
    mg_double_part term = z;
    mg_double_part sum = z;

    /* With frexp's [1/2,1) mantissa, |z| <= 1/3.  Eighty odd terms put the
     * truncation tail below 2^-250; double-part rounding is the limiting
     * error, not the series remainder. */
    for (unsigned index = 1; index <= 80; ++index) {
        term = mg_parts_multiply(term, square);
        mg_double_part divisor = {(long double)(2U * index + 1U), 0.0L};
        sum = mg_parts_add(sum, mg_parts_divide(term, divisor));
    }
    return (mg_double_part){sum.high * 2.0L, sum.low * 2.0L};
}

static mg_double_part mg_log_positive(long double value)
{
    int exponent;
    long double mantissa = frexpl(value, &exponent);
    mg_double_part mantissa_log = mg_log_series(mantissa);
    const mg_double_part log_two = {
        0xb.17217f7d1cf79acp-4L,
        -0x6.c3898cff81a12a1p-69L
    };
    mg_double_part scale = mg_parts_multiply(
        log_two, (mg_double_part){(long double)exponent, 0.0L});
    return mg_parts_add(mantissa_log, scale);
}

void __mg_log_positive_split(long double value, long double *high,
                             long double *low)
{
    mg_double_part result = mg_log_positive(value);
    *high = result.high;
    *low = result.low;
}

static mg_double_part mg_pow_integral(mg_double_part base, unsigned exponent,
                                      int inverse)
{
    mg_double_part result = {1.0L, 0.0L};
    if (inverse)
        base = mg_parts_divide((mg_double_part){1.0L, 0.0L}, base);
    while (exponent) {
        if (exponent & 1U)
            result = mg_parts_multiply(result, base);
        exponent >>= 1;
        if (exponent)
            base = mg_parts_multiply(base, base);
    }
    return result;
}

static int mg_pow_integer(long double value, int *odd)
{
    if (!isfinite(value) || truncl(value) != value)
        return 0;

    long double magnitude = fabsl(value);
    /* At and above 2^64 every representable x87 value is an even integer. */
    *odd = magnitude < 0x1p64L && fmodl(magnitude, 2.0L) == 1.0L;
    return 1;
}

static long double mg_pow_overflow(int negative)
{
    (void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);
    int rounding = fegetround();
    if (negative) {
        if (rounding == FE_UPWARD || rounding == FE_TOWARDZERO)
            return -LDBL_MAX;
        return -HUGE_VALL;
    }
    if (rounding == FE_DOWNWARD || rounding == FE_TOWARDZERO)
        return LDBL_MAX;
    return HUGE_VALL;
}

static long double mg_pow_underflow(int negative)
{
    (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    int rounding = fegetround();
    if (negative) {
        if (rounding == FE_DOWNWARD)
            return -LDBL_TRUE_MIN;
        return -0.0L;
    }
    if (rounding == FE_UPWARD)
        return LDBL_TRUE_MIN;
    return 0.0L;
}

long double powl(long double x, long double y)
{
    int x_class = __mg_fpclassifyl(x);
    int y_class = __mg_fpclassifyl(y);

    /* C's identity cases take precedence over NaN propagation. */
    if (y_class == FP_ZERO)
        return 1.0L;
    if (x == 1.0L)
        return 1.0L;
    if (x_class == FP_NAN || y_class == FP_NAN)
        return x + y;

    int y_odd = 0;
    int y_integer = mg_pow_integer(y, &y_odd);

    if (y_class == FP_INFINITE) {
        if (fabsl(x) == 1.0L)
            return 1.0L;
        int grows = (fabsl(x) > 1.0L) != __mg_signbitl(y);
        return grows ? HUGE_VALL : 0.0L;
    }

    int negative_result = __mg_signbitl(x) && y_integer && y_odd;
    if (x_class == FP_INFINITE)
        return __mg_signbitl(y) ?
            (negative_result ? -0.0L : 0.0L) :
            (negative_result ? -HUGE_VALL : HUGE_VALL);

    if (x_class == FP_ZERO) {
        if (__mg_signbitl(y)) {
            (void)feraiseexcept(FE_DIVBYZERO);
            return negative_result ? -HUGE_VALL : HUGE_VALL;
        }
        return negative_result ? -0.0L : 0.0L;
    }

    if (__mg_signbitl(x) && !y_integer) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }

    long double magnitude = fabsl(x);
    if (y == 1.0L)
        return x;
    if (y == -1.0L)
        return 1.0L / x;

    /* Preserve exact powers of two without running the transcendental path.
     * Besides avoiding needless work, this keeps internal log approximation
     * from raising FE_INEXACT for an exactly represented result. */
    int base_exponent;
    if (y_integer && fabsl(y) <= (long double)INT_MAX &&
        frexpl(magnitude, &base_exponent) == 0.5L) {
        long long result_exponent =
            (long long)(base_exponent - 1) * (long long)y;
        if (result_exponent > INT_MAX)
            return mg_pow_overflow(negative_result);
        if (result_exponent < INT_MIN)
            return mg_pow_underflow(negative_result);
        return scalbnl(negative_result ? -1.0L : 1.0L,
                       (int)result_exponent);
    }

    int rounding = fegetround();
    int changed = rounding >= 0 && rounding != FE_TONEAREST;
    if (changed)
        (void)fesetround(FE_TONEAREST);

    mg_double_part logarithm = mg_log_positive(magnitude);
    long double logarithm_lead = logarithm.high != 0.0L ?
                                 logarithm.high : logarithm.low;
    if (logarithm_lead == 0.0L) {
        if (changed)
            (void)fesetround(rounding);
        return 1.0L;
    }

    int product_exponent = ilogbl(fabsl(y)) + ilogbl(fabsl(logarithm_lead));
    int exponent_negative = __mg_signbitl(y) != __mg_signbitl(logarithm_lead);
    if (product_exponent >= 14) {
        if (changed)
            (void)fesetround(rounding);
        if (exponent_negative)
            return mg_pow_underflow(negative_result);
        return mg_pow_overflow(negative_result);
    }
    long double natural_exponent = y * (logarithm.high + logarithm.low);
    if (natural_exponent > 12000.0L) {
        if (changed)
            (void)fesetround(rounding);
        return mg_pow_overflow(negative_result);
    }
    if (natural_exponent < -12000.0L) {
        if (changed)
            (void)fesetround(rounding);
        return mg_pow_underflow(negative_result);
    }

    /* For integral exponents the double-part product retains substantially
     * more precision than repeated target-format multiplication.  Bound the
     * exponent to a fixed 32-step loop; larger exponents use logarithmic
     * scaling below. */
    if (y_integer && fabsl(y) <= 0xffffffffp+0L &&
        natural_exponent >= -11000.0L && natural_exponent <= 11000.0L) {
        mg_double_part result = mg_pow_integral(
            (mg_double_part){magnitude, 0.0L}, (unsigned)fabsl(y),
            __mg_signbitl(y));
        if (changed)
            (void)fesetround(rounding);
        long double high = negative_result ? -result.high : result.high;
        long double low = negative_result ? -result.low : result.low;
        return high + low;
    }

    mg_double_part exponent = mg_parts_multiply(
        logarithm, (mg_double_part){y, 0.0L});
    if (changed)
        (void)fesetround(rounding);
    int magnitude_rounding = rounding;
    if (negative_result && rounding == FE_UPWARD)
        magnitude_rounding = FE_DOWNWARD;
    else if (negative_result && rounding == FE_DOWNWARD)
        magnitude_rounding = FE_UPWARD;
    int changed_result_rounding = magnitude_rounding >= 0 &&
                                  magnitude_rounding != rounding;
    if (changed_result_rounding)
        (void)fesetround(magnitude_rounding);
    long double result = __mg_exp_compensated(exponent.high, exponent.low);
    if (changed_result_rounding)
        (void)fesetround(rounding);

    /* For a negative result, use the opposite directed magnitude mode before
     * applying the sign.  Negation after a same-mode rounded positive result
     * would reverse upward/downward rounding at range boundaries. */
    return negative_result ? -result : result;
}

float powf(float x, float y)
{
    return (float)powl((long double)x, (long double)y);
}

double pow(double x, double y)
{
    return (double)powl((long double)x, (long double)y);
}
