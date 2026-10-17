/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <complex.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include "math_private.h"

typedef struct {
    long double real;
    long double imag;
} mg_complex_pair;

static const long double mg_complex_pi =
    3.14159265358979323846264338327950288419716939937511L;
static const long double mg_complex_ln2 = 0xb.17217f7d1cf79acp-4L;

static void mg_two_sum(long double a, long double b,
                       long double *sum, long double *error)
{
    *sum = a + b;
    long double virtual_b = *sum - a;
    *error = (a - (*sum - virtual_b)) + (b - virtual_b);
}

/* Combine the logarithm of a component with its magnitude before exp can
 * overflow. Keep the addition residual so large terms do not discard it. */
static long double mg_exp_pair_times(long double log_high,
                                     long double log_low,
                                     long double factor)
{
    if (factor == 0.0L)
        return copysignl(0.0L, factor);
    if (isnan(factor))
        return factor + factor;

    long double first, first_error, total, total_error;
    long double tail, tail_error;
    mg_two_sum(log_high, log_low, &first, &first_error);
    mg_two_sum(first, logl(fabsl(factor)), &total, &total_error);
    mg_two_sum(first_error, total_error, &tail, &tail_error);
    long double magnitude = __mg_exp_compensated(total, tail + tail_error);
    return copysignl(magnitude, factor);
}

static long double mg_exp_times(long double exponent, long double factor)
{
    /* Below this conservative threshold exp(exponent) is finite; multiplying
     * by a sine or cosine cannot overflow it. */
    if (exponent < (long double)LDBL_MAX_EXP * mg_complex_ln2 - 1.0L)
        return expl(exponent) * factor;
    return mg_exp_pair_times(exponent, 0.0L, factor);
}

static mg_complex_pair mg_pair(long double real, long double imag)
{
    return (mg_complex_pair){real, imag};
}

static mg_complex_pair mg_pair_add(mg_complex_pair a, mg_complex_pair b)
{
    return mg_pair(a.real + b.real, a.imag + b.imag);
}

static mg_complex_pair mg_pair_sub(mg_complex_pair a, mg_complex_pair b)
{
    return mg_pair(a.real - b.real, a.imag - b.imag);
}

static long double mg_sum_products(long double a, long double b,
                                   long double c, long double d, int subtract)
{
    long double first = a * b;
    long double first_error = fmal(a, b, -first);
    long double second = c * d;
    long double second_error = fmal(c, d, -second);
    if (subtract) {
        long double sum = first - second;
        long double virtual_second = first - sum;
        long double sum_error = (first - (sum + virtual_second)) +
                                (virtual_second - second);
        return sum + ((first_error - second_error) + sum_error);
    }
    long double sum = first + second;
    long double virtual_second = sum - first;
    long double sum_error = (first - (sum - virtual_second)) +
                            (second - virtual_second);
    return sum + ((first_error + second_error) + sum_error);
}

static mg_complex_pair mg_complex_multiply(mg_complex_pair a,
                                            mg_complex_pair b)
{
    if (!isfinite(a.real) || !isfinite(a.imag) ||
        !isfinite(b.real) || !isfinite(b.imag)) {
        return mg_pair(a.real * b.real - a.imag * b.imag,
                       a.real * b.imag + a.imag * b.real);
    }
    long double scale_a = fmaxl(fabsl(a.real), fabsl(a.imag));
    long double scale_b = fmaxl(fabsl(b.real), fabsl(b.imag));
    if (scale_a == 0.0L || scale_b == 0.0L)
        return mg_pair(a.real * b.real - a.imag * b.imag,
                       a.real * b.imag + a.imag * b.real);

    int exponent_a, exponent_b;
    (void)frexpl(scale_a, &exponent_a);
    (void)frexpl(scale_b, &exponent_b);
    long double ar = scalbnl(a.real, -exponent_a);
    long double ai = scalbnl(a.imag, -exponent_a);
    long double br = scalbnl(b.real, -exponent_b);
    long double bi = scalbnl(b.imag, -exponent_b);
    int exponent = exponent_a + exponent_b;
    long double real = mg_sum_products(ar, br, ai, bi, 1);
    long double imag = mg_sum_products(ar, bi, ai, br, 0);
    return mg_pair(scalbnl(real, exponent), scalbnl(imag, exponent));
}

static long double mg_log_hypot(long double x, long double y)
{
    long double a = fabsl(x), b = fabsl(y);
    if (isinf(a) || isinf(b))
        return HUGE_VALL;
    if (isnan(a) || isnan(b))
        return NAN;
    long double high = fmaxl(a, b);
    long double low = fminl(a, b);
    if (high == 0.0L)
        return -HUGE_VALL;
    long double ratio = low / high;
    return logl(high) + 0.5L * log1pl(ratio * ratio);
}

static mg_complex_pair mg_complex_log(mg_complex_pair z)
{
    if (z.real == 0.0L && z.imag == 0.0L)
        (void)feraiseexcept(FE_DIVBYZERO);
    return mg_pair(mg_log_hypot(z.real, z.imag), atan2l(z.imag, z.real));
}

static long double mg_infinite_product(long double infinite_factor,
                                      long double finite_factor)
{
    if (!isinf(infinite_factor))
        return infinite_factor * finite_factor;
    if (isnan(finite_factor))
        return NAN;
    int negative = signbit(infinite_factor) != signbit(finite_factor);
    return copysignl(finite_factor == 0.0L ? 0.0L : HUGE_VALL,
                     negative ? -1.0L : 1.0L);
}

static long double mg_hyperbolic_times(long double value, long double factor,
                                       int use_sinh)
{
    if (!isfinite(value) || !isfinite(factor))
        return mg_infinite_product(use_sinh ? sinhl(value) : coshl(value),
                                   factor);

    long double magnitude = fabsl(value);
    if (magnitude < (long double)LDBL_MAX_EXP * mg_complex_ln2 - 1.0L)
        return (use_sinh ? sinhl(value) : coshl(value)) * factor;

    long double signed_factor = factor;
    if (use_sinh) {
        int negative = __mg_signbitl(factor) != __mg_signbitl(value);
        signed_factor = copysignl(fabsl(factor), negative ? -1.0L : 1.0L);
    }
    if (signed_factor == 0.0L)
        return copysignl(0.0L, signed_factor);

    /* Here log1p(+-exp(-2|x|)) is far below an x87 ulp. Retain the exact
     * subtraction residual in the low component. */
    long double high, low;
    mg_two_sum(magnitude, -mg_complex_ln2, &high, &low);
    return mg_exp_pair_times(high, low, signed_factor);
}

static mg_complex_pair mg_complex_exp(mg_complex_pair z)
{
    if (isinf(z.imag)) {
        if (isinf(z.real)) {
            if (signbit(z.real))
                return mg_pair(0.0L, 0.0L);
            (void)feraiseexcept(FE_INVALID);
            return mg_pair(HUGE_VALL, NAN);
        }
        (void)feraiseexcept(FE_INVALID);
        return mg_pair(NAN, NAN);
    }
    if (isnan(z.imag)) {
        if (isinf(z.real))
            return signbit(z.real) ? mg_pair(0.0L, 0.0L) :
                                     mg_pair(HUGE_VALL, NAN);
        return mg_pair(NAN, NAN);
    }
    if (isnan(z.real))
        return z.imag == 0.0L ? mg_pair(NAN, z.imag) : mg_pair(NAN, NAN);
    long double cosine = cosl(z.imag);
    long double sine = sinl(z.imag);
    if (isinf(z.real)) {
        if (signbit(z.real))
            return mg_pair(copysignl(0.0L, cosine),
                           copysignl(0.0L, sine));
        return mg_pair(mg_infinite_product(z.real, cosine),
                       mg_infinite_product(z.real, sine));
    }
    return mg_pair(mg_exp_times(z.real, cosine),
                   mg_exp_times(z.real, sine));
}

static mg_complex_pair mg_complex_sqrt(mg_complex_pair z)
{
    if (isinf(z.imag))
        return mg_pair(HUGE_VALL, copysignl(HUGE_VALL, z.imag));
    if (isnan(z.imag) && isinf(z.real)) {
        if (signbit(z.real))
            return mg_pair(NAN, HUGE_VALL);
        return mg_pair(HUGE_VALL, NAN);
    }
    if (isinf(z.real)) {
        if (signbit(z.real))
            return mg_pair(0.0L, copysignl(HUGE_VALL, z.imag));
        return mg_pair(HUGE_VALL, copysignl(0.0L, z.imag));
    }
    if (isnan(z.real) || isnan(z.imag))
        return mg_pair(NAN, NAN);
    if (z.real == 0.0L && z.imag == 0.0L)
        return mg_pair(0.0L, z.imag);

    long double scale = fmaxl(fabsl(z.real), fabsl(z.imag));
    long double x = z.real / scale;
    long double y = z.imag / scale;
    long double magnitude = sqrtl(x * x + y * y);
    long double root_scale = sqrtl(scale);
    if (z.real >= 0.0L) {
        long double real = root_scale * sqrtl((magnitude + x) * 0.5L);
        return mg_pair(real, z.imag / (2.0L * real));
    }
    long double imag = copysignl(root_scale * sqrtl((magnitude - x) * 0.5L),
                                 z.imag);
    long double real = fabsl(z.imag) / (2.0L * fabsl(imag));
    return mg_pair(real, imag);
}

static mg_complex_pair mg_complex_power(mg_complex_pair base,
                                        mg_complex_pair exponent)
{
    if (exponent.real == 0.0L && exponent.imag == 0.0L)
        return mg_pair(1.0L, 0.0L);
    if (base.real == 1.0L && base.imag == 0.0L)
        return mg_pair(1.0L, 0.0L);
    if (base.real == 0.0L && base.imag == 0.0L && exponent.imag == 0.0L) {
        if (exponent.real > 0.0L)
            return mg_pair(0.0L, copysignl(0.0L, base.imag));
        if (exponent.real < 0.0L) {
            (void)feraiseexcept(FE_DIVBYZERO);
            return mg_pair(HUGE_VALL, copysignl(0.0L, base.imag));
        }
    }
    mg_complex_pair logarithm = mg_complex_log(base);
    return mg_complex_exp(mg_complex_multiply(exponent, logarithm));
}

static mg_complex_pair mg_complex_sin(mg_complex_pair z)
{
    long double sx = sinl(z.real), cx = cosl(z.real);
    if (isfinite(z.real) && isfinite(z.imag))
        return mg_pair(mg_hyperbolic_times(z.imag, sx, 0),
                       mg_hyperbolic_times(z.imag, cx, 1));
    long double shy = sinhl(z.imag), chy = coshl(z.imag);
    return mg_pair(mg_infinite_product(chy, sx),
                   mg_infinite_product(shy, cx));
}

static mg_complex_pair mg_complex_cos(mg_complex_pair z)
{
    long double sx = sinl(z.real), cx = cosl(z.real);
    if (isfinite(z.real) && isfinite(z.imag))
        return mg_pair(mg_hyperbolic_times(z.imag, cx, 0),
                       -mg_hyperbolic_times(z.imag, sx, 1));
    long double shy = sinhl(z.imag), chy = coshl(z.imag);
    return mg_pair(mg_infinite_product(chy, cx),
                   -mg_infinite_product(shy, sx));
}

static mg_complex_pair mg_complex_tan(mg_complex_pair z)
{
    long double ay = fabsl(z.imag);
    if (ay > 1.0L) {
        long double attenuation = ay > LDBL_MAX * 0.5L ? 0.0L :
                                  expl(-2.0L * ay);
        long double cosine = cosl(2.0L * z.real);
        long double sine = sinl(2.0L * z.real);
        long double denominator = 1.0L + 2.0L * attenuation * cosine +
                                  attenuation * attenuation;
        long double real = 2.0L * attenuation * sine / denominator;
        long double imag = copysignl((1.0L - attenuation * attenuation) /
                                     denominator, z.imag);
        return mg_pair(real, imag);
    }
    long double sine = sinl(z.real), cosine = cosl(z.real);
    long double sinh = sinhl(z.imag), cosh = coshl(z.imag);
    long double denominator = cosine * cosine + sinh * sinh;
    return mg_pair((sine * cosine) / denominator,
                   (sinh * cosh) / denominator);
}

static mg_complex_pair mg_complex_sinh(mg_complex_pair z)
{
    long double sy = sinl(z.imag), cy = cosl(z.imag);
    if (isfinite(z.real) && isfinite(z.imag))
        return mg_pair(mg_hyperbolic_times(z.real, cy, 1),
                       mg_hyperbolic_times(z.real, sy, 0));
    long double shx = sinhl(z.real), chx = coshl(z.real);
    return mg_pair(mg_infinite_product(shx, cy),
                   mg_infinite_product(chx, sy));
}

static mg_complex_pair mg_complex_cosh(mg_complex_pair z)
{
    long double sy = sinl(z.imag), cy = cosl(z.imag);
    if (isfinite(z.real) && isfinite(z.imag))
        return mg_pair(mg_hyperbolic_times(z.real, cy, 0),
                       mg_hyperbolic_times(z.real, sy, 1));
    long double shx = sinhl(z.real), chx = coshl(z.real);
    return mg_pair(mg_infinite_product(chx, cy),
                   mg_infinite_product(shx, sy));
}

static mg_complex_pair mg_complex_tanh(mg_complex_pair z)
{
    long double ax = fabsl(z.real);
    if (ax > 1.0L) {
        long double attenuation = ax > LDBL_MAX * 0.5L ? 0.0L :
                                  expl(-2.0L * ax);
        long double cosine = cosl(2.0L * z.imag);
        long double sine = sinl(2.0L * z.imag);
        long double denominator = 1.0L + 2.0L * attenuation * cosine +
                                  attenuation * attenuation;
        long double real = copysignl((1.0L - attenuation * attenuation) /
                                     denominator, z.real);
        long double imag = 2.0L * attenuation * sine / denominator;
        return mg_pair(real, imag);
    }
    long double sinh = sinhl(z.real), cosh = coshl(z.real);
    long double sine = sinl(z.imag), cosine = cosl(z.imag);
    long double denominator = sinh * sinh + cosine * cosine;
    return mg_pair((sinh * cosh) / denominator,
                   (sine * cosine) / denominator);
}

static mg_complex_pair mg_complex_asin_ellipse(mg_complex_pair z,
                                               long double *cosine_out,
                                               long double *semimajor_out)
{
    long double ax = fabsl(z.real), ay = fabsl(z.imag);
    /* Ellipse coordinates avoid cancellation in iz + sqrt(1-z*z).
     * The rationalized distances retain the small separation from the
     * branch points when the imaginary part is tiny. */
    long double first = hypotl(ax + 1.0L, ay);
    long double second = hypotl(ax - 1.0L, ay);
    /* Form the small distance to the branch point before adding the
     * second radius.  `second + 1 - ax` can round to `1 - ax` because
     * the addition rounds at one before the subtraction. */
    long double branch_gap = ax > 1.0L ? ax - 1.0L : 1.0L - ax;
    long double u, v;
    if (ax > 1.0L) {
        u = ay / sqrtl(first + ax + 1.0L);
        v = ay / sqrtl(second + branch_gap);
    } else {
        u = ay / sqrtl(first + 1.0L + ax);
        v = ay / sqrtl(second + branch_gap);
    }
    long double root_delta = hypotl(u, v) * 0.707106781186547524400844362104849039L;
    long double delta = root_delta * root_delta;
    long double semimajor = ax > 1.0L ? ax + delta : 1.0L + delta;
    long double cosine;
    if (ax >= 1.0L) {
        cosine = root_delta * sqrtl((1.0L + ax / semimajor) /
                                    semimajor);
    } else {
        long double gap = (1.0L - ax) + delta;
        cosine = sqrtl((gap / semimajor) *
                       (1.0L + ax / semimajor));
    }
    long double imaginary = cosine == 0.0L ?
                            copysignl(acoshl(semimajor), z.imag) :
                            asinhl(z.imag / cosine);
    *cosine_out = cosine;
    *semimajor_out = semimajor;
    return mg_pair(atan2l(z.real / semimajor, cosine), imaginary);
}

static mg_complex_pair mg_complex_asin(mg_complex_pair z)
{
    if (z.imag == 0.0L && fabsl(z.real) <= 1.0L)
        return mg_pair(asinl(z.real), z.imag);
    if (z.imag == 0.0L && isfinite(z.real))
        return mg_pair(copysignl(mg_complex_pi * 0.5L, z.real),
                       copysignl(acoshl(fabsl(z.real)), z.imag));
    if (isfinite(z.real) && isfinite(z.imag)) {
        long double ax = fabsl(z.real), ay = fabsl(z.imag);
        if (fmaxl(ax, ay) > 0x1p64L)
            return mg_pair(atan2l(z.real, ay),
                           copysignl(mg_log_hypot(ax, ay) +
                                     0.693147180559945309417232121458176568L,
                                     z.imag));
        long double cosine, semimajor;
        return mg_complex_asin_ellipse(z, &cosine, &semimajor);
    }
    mg_complex_pair iz = mg_pair(-z.imag, z.real);
    mg_complex_pair square = mg_complex_multiply(z, z);
    mg_complex_pair root = mg_complex_sqrt(
        mg_pair(1.0L - square.real, -square.imag));
    mg_complex_pair logarithm = mg_complex_log(mg_pair_add(iz, root));
    return mg_pair(logarithm.imag, -logarithm.real);
}

static mg_complex_pair mg_complex_acos(mg_complex_pair z)
{
    if (z.imag == 0.0L && fabsl(z.real) <= 1.0L)
        return mg_pair(acosl(z.real), -z.imag);
    if (isfinite(z.real) && isfinite(z.imag)) {
        long double ax = fabsl(z.real), ay = fabsl(z.imag);
        if (fmaxl(ax, ay) > 0x1p64L) {
            long double imaginary = copysignl(
                mg_log_hypot(ax, ay) +
                    0.693147180559945309417232121458176568L,
                z.imag);
            return mg_pair(atan2l(ay, z.real), -imaginary);
        }
        long double cosine, semimajor;
        mg_complex_pair asin_result =
            mg_complex_asin_ellipse(z, &cosine, &semimajor);
        /* Computing pi/2 - asin(real) loses the small result near z = 1.
         * The ellipse coordinates provide sine and cosine directly. */
        return mg_pair(atan2l(cosine, z.real / semimajor),
                       -asin_result.imag);
    }
    mg_complex_pair result = mg_complex_asin(z);
    return mg_pair(mg_complex_pi * 0.5L - result.real, -result.imag);
}

static mg_complex_pair mg_complex_atan(mg_complex_pair z)
{
    if (z.imag == 0.0L)
        return mg_pair(atanl(z.real), z.imag);
    if (isfinite(z.real) && isfinite(z.imag)) {
        if (z.real == 0.0L && fabsl(z.imag) == 1.0L) {
            (void)feraiseexcept(FE_DIVBYZERO);
            return mg_pair(z.real, copysignl(HUGE_VALL, z.imag));
        }
        long double scale = fmaxl(1.0L, fmaxl(fabsl(z.real), fabsl(z.imag)));
        long double x = z.real / scale;
        long double y = z.imag / scale;
        long double inverse = 1.0L / scale;
        long double real = 0.5L * atan2l(2.0L * x * inverse,
                                         inverse * inverse - x * x - y * y);
        long double denominator = x * x + (y - inverse) * (y - inverse);
        long double ratio = (4.0L * y * inverse) / denominator;
        long double logarithm;
        if (ratio > -0.5L) {
            logarithm = log1pl(ratio);
        } else {
            long double numerator = x * x + (y + inverse) * (y + inverse);
            logarithm = logl(numerator) - logl(denominator);
        }
        return mg_pair(real, 0.25L * logarithm);
    }
    mg_complex_pair iz = mg_pair(-z.imag, z.real);
    mg_complex_pair one = mg_pair(1.0L, 0.0L);
    mg_complex_pair left = mg_complex_log(mg_pair_sub(one, iz));
    mg_complex_pair right = mg_complex_log(mg_pair_add(one, iz));
    mg_complex_pair difference = mg_pair_sub(left, right);
    return mg_pair(-0.5L * difference.imag, 0.5L * difference.real);
}

static mg_complex_pair mg_complex_asinh(mg_complex_pair z)
{
    if (z.imag == 0.0L)
        return mg_pair(asinhl(z.real), z.imag);
    mg_complex_pair rotated = mg_complex_asin(mg_pair(-z.imag, z.real));
    return mg_pair(rotated.imag, -rotated.real);
}

static mg_complex_pair mg_complex_acosh(mg_complex_pair z)
{
    if (z.imag == 0.0L) {
        if (z.real >= 1.0L)
            return mg_pair(acoshl(z.real), z.imag);
        if (z.real >= -1.0L)
            return mg_pair(0.0L, copysignl(acosl(z.real), z.imag));
        return mg_pair(acoshl(-z.real), copysignl(mg_complex_pi, z.imag));
    }
    if (isfinite(z.real) && isfinite(z.imag)) {
        long double ax = fabsl(z.real), ay = fabsl(z.imag);
        long double first = hypotl(ax + 1.0L, ay);
        long double second = hypotl(ax - 1.0L, ay);
        long double branch_gap = ax > 1.0L ? ax - 1.0L : 1.0L - ax;
        long double u, v;
        if (ax > 1.0L) {
            u = ay / sqrtl(first + ax + 1.0L);
            v = ay / sqrtl(second + branch_gap);
        } else {
            u = ay / sqrtl(first + 1.0L + ax);
            v = ay / sqrtl(second + branch_gap);
        }
        long double root_delta = hypotl(u, v) *
            0.707106781186547524400844362104849039L;
        long double delta = root_delta * root_delta;
        long double semimajor = ax > 1.0L ? ax + delta : 1.0L + delta;
        long double real = ax > 1.0L ? acoshl(semimajor) :
                           asinhl(hypotl(u, v) *
                                  sqrtl(1.0L + delta * 0.5L));
        long double cosine = z.real / semimajor;
        if (ax <= 1.0L) {
            /* The real part can be subnormal when y is tiny.  Recovering the
             * angle as y/sinh(real) then inherits that coarse subnormal
             * quantization.  Here cos(angle)=x/semimajor is well conditioned,
             * so use acos directly to retain the independent branch angle. */
            cosine = fminl(1.0L, fmaxl(-1.0L, cosine));
            return mg_pair(real, copysignl(acosl(cosine), z.imag));
        }
        long double sine = z.imag / sinhl(real);
        return mg_pair(real, atan2l(sine, cosine));
    }
    mg_complex_pair plus = mg_complex_sqrt(mg_pair(z.real + 1.0L, z.imag));
    mg_complex_pair minus = mg_complex_sqrt(mg_pair(z.real - 1.0L, z.imag));
    return mg_complex_log(mg_pair_add(z, mg_complex_multiply(plus, minus)));
}

static mg_complex_pair mg_complex_atanh(mg_complex_pair z)
{
    if (z.imag == 0.0L) {
        long double x = z.real;
        if (fabsl(x) < 1.0L)
            return mg_pair(atanhl(x), z.imag);
        if (fabsl(x) == 1.0L) {
            (void)feraiseexcept(FE_DIVBYZERO);
            return mg_pair(copysignl(HUGE_VALL, x), z.imag);
        }
        long double magnitude = 0.5L * log1pl(2.0L / (fabsl(x) - 1.0L));
        return mg_pair(copysignl(magnitude, x),
                       copysignl(mg_complex_pi * 0.5L, z.imag));
    }
    if (isfinite(z.real) && isfinite(z.imag)) {
        long double scale = fmaxl(1.0L, fmaxl(fabsl(z.real), fabsl(z.imag)));
        long double x = z.real / scale, y = z.imag / scale;
        long double inverse = 1.0L / scale;
        long double numerator = (inverse + x) * (inverse + x) + y * y;
        long double denominator = (inverse - x) * (inverse - x) + y * y;
        long double ratio = 4.0L * x * inverse / denominator;
        long double logarithm = ratio > -0.5L ? log1pl(ratio) :
                                logl(numerator) - logl(denominator);
        long double angle = atan2l(2.0L * y * inverse,
                                   inverse * inverse - x * x - y * y);
        return mg_pair(0.25L * logarithm, 0.5L * angle);
    }
    mg_complex_pair one = mg_pair(1.0L, 0.0L);
    mg_complex_pair difference = mg_pair_sub(
        mg_complex_log(mg_pair_add(one, z)),
        mg_complex_log(mg_pair_sub(one, z)));
    return mg_pair(0.5L * difference.real, 0.5L * difference.imag);
}

static mg_complex_pair mg_complex_getf(float complex z)
{
    return mg_pair((long double)__real__ z, (long double)__imag__ z);
}

static mg_complex_pair mg_complex_get(double complex z)
{
    return mg_pair((long double)__real__ z, (long double)__imag__ z);
}

static mg_complex_pair mg_complex_getl(long double complex z)
{
    return mg_pair(__real__ z, __imag__ z);
}

#define MG_COMPLEX_UNARY(name, core) \
    float complex name##f(float complex z) \
    { mg_complex_pair r = core(mg_complex_getf(z)); \
      return CMPLXF((float)r.real, (float)r.imag); } \
    double complex name(double complex z) \
    { mg_complex_pair r = core(mg_complex_get(z)); \
      return CMPLX((double)r.real, (double)r.imag); } \
    long double complex name##l(long double complex z) \
    { mg_complex_pair r = core(mg_complex_getl(z)); \
      return CMPLXL(r.real, r.imag); }

MG_COMPLEX_UNARY(cexp, mg_complex_exp)
MG_COMPLEX_UNARY(clog, mg_complex_log)
MG_COMPLEX_UNARY(csqrt, mg_complex_sqrt)
MG_COMPLEX_UNARY(csin, mg_complex_sin)
MG_COMPLEX_UNARY(ccos, mg_complex_cos)
MG_COMPLEX_UNARY(ctan, mg_complex_tan)
MG_COMPLEX_UNARY(csinh, mg_complex_sinh)
MG_COMPLEX_UNARY(ccosh, mg_complex_cosh)
MG_COMPLEX_UNARY(ctanh, mg_complex_tanh)
MG_COMPLEX_UNARY(casin, mg_complex_asin)
MG_COMPLEX_UNARY(cacos, mg_complex_acos)
MG_COMPLEX_UNARY(catan, mg_complex_atan)
MG_COMPLEX_UNARY(casinh, mg_complex_asinh)
MG_COMPLEX_UNARY(cacosh, mg_complex_acosh)
MG_COMPLEX_UNARY(catanh, mg_complex_atanh)

float cabsf(float complex z) { mg_complex_pair p = mg_complex_getf(z); return hypotf((float)p.real, (float)p.imag); }
double cabs(double complex z) { mg_complex_pair p = mg_complex_get(z); return hypot((double)p.real, (double)p.imag); }
long double cabsl(long double complex z) { mg_complex_pair p = mg_complex_getl(z); return hypotl(p.real, p.imag); }
float cargf(float complex z) { mg_complex_pair p = mg_complex_getf(z); return atan2f((float)p.imag, (float)p.real); }
double carg(double complex z) { mg_complex_pair p = mg_complex_get(z); return atan2((double)p.imag, (double)p.real); }
long double cargl(long double complex z) { mg_complex_pair p = mg_complex_getl(z); return atan2l(p.imag, p.real); }
float crealf(float complex z) { return __real__ z; }
double creal(double complex z) { return __real__ z; }
long double creall(long double complex z) { return __real__ z; }
float cimagf(float complex z) { return __imag__ z; }
double cimag(double complex z) { return __imag__ z; }
long double cimagl(long double complex z) { return __imag__ z; }

float complex conjf(float complex z) { return CMPLXF(__real__ z, -__imag__ z); }
double complex conj(double complex z) { return CMPLX(__real__ z, -__imag__ z); }
long double complex conjl(long double complex z) { return CMPLXL(__real__ z, -__imag__ z); }
float complex cprojf(float complex z) { return isinf(__real__ z) || isinf(__imag__ z) ? CMPLXF(INFINITY, copysignf(0.0f, __imag__ z)) : z; }
double complex cproj(double complex z) { return isinf(__real__ z) || isinf(__imag__ z) ? CMPLX(INFINITY, copysign(0.0, __imag__ z)) : z; }
long double complex cprojl(long double complex z) { return isinf(__real__ z) || isinf(__imag__ z) ? CMPLXL(INFINITY, copysignl(0.0L, __imag__ z)) : z; }

float complex cpowf(float complex x, float complex y) { mg_complex_pair r = mg_complex_power(mg_complex_getf(x), mg_complex_getf(y)); return CMPLXF((float)r.real, (float)r.imag); }
double complex cpow(double complex x, double complex y) { mg_complex_pair r = mg_complex_power(mg_complex_get(x), mg_complex_get(y)); return CMPLX((double)r.real, (double)r.imag); }
long double complex cpowl(long double complex x, long double complex y) { mg_complex_pair r = mg_complex_power(mg_complex_getl(x), mg_complex_getl(y)); return CMPLXL(r.real, r.imag); }
