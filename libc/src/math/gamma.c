/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <math.h>
#include "math_private.h"

/*
 * The constants below are the values zeta(2) through zeta(34), rounded to
 * the x87 significand precision.  The series
 *
 *   log Gamma(1+z) = -gamma*z + sum((-1)^n zeta(n) z^n/n, n >= 2)
 *
 * follows from the Weierstrass product and converges for |z| < 1.  Keeping
 * its argument in [-1/2,1/2] gives fast geometric convergence without a
 * fitted rational approximation.
 */
static const long double mg_zeta[] = {
    1.64493406684822643647241516664602518921894990121e0L,
    1.20205690315959428539973816151144999076498629234e0L,
    1.08232323371113819151600369654116790277475095192e0L,
    1.03692775514336992633136548645703416805708091950e0L,
    1.01734306198444913971451792979092052790181749003e0L,
    1.00834927738192282683979754984979675959986356057e0L,
    1.00407735619794433937868523850865246525896079065e0L,
    1.00200839282608221441785276923241206048560585139e0L,
    1.00099457512781808533714595890031901700601953156e0L,
    1.00049418860411946455870228252646993646860643576e0L,
    1.00024608655330804829863799804773967096041608846e0L,
    1.00012271334757848914675183652635739571427510590e0L,
    1.00006124813505870482925854510513533374748169617e0L,
    1.00003058823630702049355172851064506258762794871e0L,
    1.00001528225940865187173257148763672202323738899e0L,
    1.00000763719763789976227360029356302921308824909e0L,
    1.00000381729326499983985646164462193973045469722e0L,
    1.00000190821271655393892565695779510135325857114e0L,
    1.00000095396203387279611315203868344934594379419e0L,
    1.00000047693298678780646311671960437304596644669e0L,
    1.00000023845050272773299000364818675299493504182e0L,
    1.00000011921992596531107306778871888232638725500e0L,
    1.00000005960818905125947961244020793580122750392e0L,
    1.00000002980350351465228018606370506936601184473e0L,
    1.00000001490155482836504123465850663069862886479e0L,
    1.00000000745071178983542949198100417060411945472e0L,
    1.00000000372533402478845705481920401840242323289e0L,
    1.00000000186265972351304900640390994541694806167e0L,
    1.00000000093132743241966818287176473502121981357e0L,
    1.00000000046566290650337840729892332512200710627e0L,
    1.00000000023283118336765054920014559759404950248e0L,
    1.00000000011641550172700519775929738354563095165e0L,
    1.00000000005820772087902700889243685989106305417e0L
};

static const long double mg_euler_gamma =
    0.57721566490153286060651209008240243104215933593992L;
static const long double mg_pi =
    3.14159265358979323846264338327950288419716939937511L;
static const long double mg_log_sqrt_2pi =
    0.91893853320467274178032973640561763986139747363778L;

static long double mg_log_gamma_near_one(long double z)
{
    long double power = z * z;
    long double total = -mg_euler_gamma * z;
    long double correction = 0.0L;

    for (unsigned n = 2; n <= 80; ++n) {
        long double zeta = n <= 34 ? mg_zeta[n - 2] : 1.0L;
        long double coefficient = zeta / (long double)n;
        long double term;
        if (n & 1U)
            coefficient = -coefficient;
        term = coefficient * power;

        /* Neumaier compensation keeps the short, alternating tail from
         * erasing the much smaller leading terms near z == 0. */
        long double next = total + term;
        if (fabsl(total) >= fabsl(term))
            correction += (total - next) + term;
        else
            correction += (term - next) + total;
        total = next;
        power *= z;
    }
    return total + correction;
}

static void mg_pair_add(long double *high, long double *low,
                        long double value)
{
    long double sum = *high + value;
    long double virtual_value = sum - *high;
    long double error = (*high - (sum - virtual_value)) +
                        (value - virtual_value);
    long double tail = *low + error;
    long double normalized = sum + tail;
    *low = (sum - normalized) + tail;
    *high = normalized;
}

static void mg_log_gamma_stirling_split(long double x,
                                        long double *high, long double *low)
{
    long double inverse = 1.0L / x;
    long double square = inverse * inverse;
    long double series = 854513.0L / 63756.0L;
    series = -174611.0L / 125400.0L + square * series;
    series = 43867.0L / 244188.0L + square * series;
    series = -3617.0L / 122400.0L + square * series;
    series = 7.0L / 1092.0L + square * series;
    series = -691.0L / 360360.0L + square * series;
    series = 1.0L / 1188.0L + square * series;
    series = -1.0L / 1680.0L + square * series;
    series = 1.0L / 1260.0L + square * series;
    series = -1.0L / 360.0L + square * series;
    series = 1.0L / 12.0L + square * series;
    series *= inverse;

    long double logarithm_high, logarithm_low;
    __mg_log_positive_split(x, &logarithm_high, &logarithm_low);
    long double factor = x - 0.5L;
    long double product = factor * logarithm_high;
    *high = product;
    *low = fmal(factor, logarithm_high, -product) + factor * logarithm_low;
    mg_pair_add(high, low, -x);
    mg_pair_add(high, low, mg_log_sqrt_2pi);
    mg_pair_add(high, low, series);
}

static long double mg_log_gamma_stirling(long double x)
{
    long double high, low;
    mg_log_gamma_stirling_split(x, &high, &low);
    return high + low;
}

static long double mg_log_gamma_positive(long double x)
{
    if (x == 1.0L || x == 2.0L)
        return 0.0L;
    if (x >= 16.0L)
        return mg_log_gamma_stirling(x);
    if (x < 0.5L)
        return mg_log_gamma_near_one(x) - logl(x);
    if (x <= 1.5L)
        return mg_log_gamma_near_one(x - 1.0L);

    long double sum = 0.0L;
    while (x > 1.5L) {
        x -= 1.0L;
        sum += logl(x);
    }
    return mg_log_gamma_near_one(x - 1.0L) + sum;
}

static int mg_gamma_is_pole(long double x)
{
    return x <= 0.0L && truncl(x) == x;
}

static long double mg_gamma_small_argument(long double x);

static long double mg_log_gamma_abs(long double x, int *negative)
{
    if (x > 0.0L) {
        *negative = 0;
        return mg_log_gamma_positive(x);
    }

    if (x > -32.0L) {
        if (fabsl(x) >= 1.0L / LDBL_MAX) {
            *negative = fmodl(fabsl(truncl(x)), 2.0L) == 0.0L;
            return logl(fabsl(mg_gamma_small_argument(x)));
        }
        long double reduced = x;
        long double sum = 0.0L;
        long double correction = 0.0L;
        while (reduced < 0.5L) {
            long double term = logl(fabsl(reduced));
            long double adjusted = term - correction;
            long double next = sum + adjusted;
            correction = (next - sum) - adjusted;
            sum = next;
            reduced += 1.0L;
        }
        *negative = 0;
        if (fmodl(fabsl(truncl(x)), 2.0L) == 0.0L)
            *negative = 1;
        return mg_log_gamma_near_one(reduced - 1.0L) - sum;
    }

    long double integer;
    long double fraction = modfl(x, &integer);
    long double reduced = fabsl(fraction);
    if (reduced > 0.5L)
        reduced = 1.0L - reduced;
    long double sine = copysignl(sinl(mg_pi * reduced), fraction);
    if (fmodl(fabsl(integer), 2.0L) != 0.0L)
        sine = -sine;
    *negative = signbit(sine) != 0;
    return logl(mg_pi) - logl(fabsl(sine)) -
           mg_log_gamma_positive(1.0L - x);
}

static long double mg_gamma_near_unit(long double x)
{
    return expl(mg_log_gamma_near_one(x - 1.0L));
}

static long double mg_gamma_small_argument(long double x)
{
    long double reduced = x;
    long double product = 1.0L;
    if (x < 0.5L) {
        while (reduced < 0.5L) {
            product *= reduced;
            reduced += 1.0L;
        }
        return mg_gamma_near_unit(reduced) / product;
    }
    while (reduced > 1.5L) {
        reduced -= 1.0L;
        product *= reduced;
    }
    return mg_gamma_near_unit(reduced) * product;
}

long double lgammal(long double x)
{
    int classification = fpclassify(x);
    if (classification == FP_NAN)
        return x + x;
    if (classification == FP_INFINITE) {
        if (!signbit(x))
            return x;
        (void)feraiseexcept(FE_INVALID);
        return NAN;
    }
    if (mg_gamma_is_pole(x)) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return HUGE_VALL;
    }
    int negative;
    return mg_log_gamma_abs(x, &negative);
}

long double tgammal(long double x)
{
    int classification = fpclassify(x);
    if (classification == FP_NAN)
        return x + x;
    if (classification == FP_INFINITE) {
        if (!signbit(x))
            return x;
        (void)feraiseexcept(FE_INVALID);
        return NAN;
    }
    if (x == 0.0L) {
        (void)feraiseexcept(FE_DIVBYZERO);
        return copysignl(HUGE_VALL, x);
    }
    if (mg_gamma_is_pole(x)) {
        (void)feraiseexcept(FE_INVALID);
        return NAN;
    }

    if (x > -32.0L && x < 32.0L)
        return mg_gamma_small_argument(x);

    if (x > 0.0L && x >= 32.0L) {
        long double high, low;
        mg_log_gamma_stirling_split(x, &high, &low);
        return __mg_exp_compensated(high, low);
    }

    int negative;
    long double logarithm = mg_log_gamma_abs(x, &negative);
    long double result = expl(logarithm);
    return negative ? -result : result;
}

float lgammaf(float x) { return (float)lgammal((long double)x); }
double lgamma(double x) { return (double)lgammal((long double)x); }
float tgammaf(float x) { return (float)tgammal((long double)x); }
double tgamma(double x) { return (double)tgammal((long double)x); }
