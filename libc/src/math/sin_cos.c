/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include "invpio2_table.h"
#include <fenv.h>
#include <math.h>

#if !defined(__x86_64__)
#error Mangrove trigonometric functions require x86-64 x87 long double
#endif

#define MG_INVPIO2_BITS 16704
#define MG_INVPIO2_LIMBS (MG_INVPIO2_BITS / 64)

static unsigned mg_product_bit(const uint64_t *product, unsigned bit)
{
    if (bit >= (MG_INVPIO2_LIMBS + 1) * 64U)
        return 0;
    return (unsigned)((product[bit / 64] >> (bit % 64)) & 1U);
}

static uint64_t mg_product_window(const uint64_t *product, unsigned first)
{
    if (first >= (MG_INVPIO2_LIMBS + 1) * 64U)
        return 0;
    unsigned word = first / 64;
    unsigned shift = first % 64;
    uint64_t value = product[word] >> shift;
    if (shift && word + 1 < MG_INVPIO2_LIMBS + 1)
        value |= product[word + 1] << (64 - shift);
    return value;
}

/* Reduce a finite magnitude to r = (x*2/pi - nearest_integer)*pi/2.
 * Multiplying the 80-bit significand by a fixed-point 2/pi expansion avoids
 * losing the low quotient bits for inputs throughout the x87 exponent range. */
static long double mg_reduce_quadrant(long double value, unsigned *quadrant)
{
    mg_x87_value raw = mg_x87_unpack(value);
    unsigned field = mg_x87_exponent(raw);
    int exponent = field ? (int)field - 16383 : -16382;
    uint64_t product[MG_INVPIO2_LIMBS + 1];
    __uint128_t carry = 0;

    for (unsigned i = 0; i < MG_INVPIO2_LIMBS; ++i) {
        __uint128_t partial = (__uint128_t)mg_invpio2_fixed[i] *
                              raw.significand + carry;
        product[i] = (uint64_t)partial;
        carry = partial >> 64;
    }
    product[MG_INVPIO2_LIMBS] = (uint64_t)carry;

    unsigned shift = (unsigned)(MG_INVPIO2_BITS + 63 - exponent);
    unsigned integer = mg_product_bit(product, shift) |
                       (mg_product_bit(product, shift + 1) << 1);
    unsigned round_up = mg_product_bit(product, shift - 1);
    uint64_t high = mg_product_window(product, shift - 64);
    uint64_t low = mg_product_window(product, shift - 128);
    long double fraction = scalbnl((long double)high, -64) -
                           (round_up ? 1.0L : 0.0L);
    fraction += scalbnl((long double)low, -128);
    if (round_up)
        integer = (integer + 1U) & 3U;
    if (mg_x87_negative(raw)) {
        fraction = -fraction;
        integer = (0U - integer) & 3U;
    }
    *quadrant = integer;
    return fraction * 0xc.90fdaa22168c235p-3L;
}

static const long double mg_sin_polynomial[] = {
    -0.16666666666666666666666666666666666666666666666667L,
     0.0083333333333333333333333333333333333333333333333333L,
    -0.00019841269841269841269841269841269841269841269841270L,
     0.0000027557319223985890652557319223985890652557319223986L,
    -2.5052108385441718775052108385441718775052108385442e-8L,
     1.6059043836821614599392377170154947932725710503488e-10L,
    -7.6471637318198164759011319857880704441551002397563e-13L,
     2.8114572543455207631989455830103200162334927352045e-15L,
    -8.2206352466243297169559812368722807492207389918261e-18L,
     1.9572941063391261230847574373505430355287473790062e-20L,
    -3.8681701706306840377169119315228123231793426462573e-23L,
     6.4469502843844733961948532192046872052989044104289e-26L,
    -9.1836898637955461484257168364739133978616871943432e-29L,
     1.1309962886447716931558764576938316992440501470866e-31L,
    -1.2161250415535179496299746856922921497247851043942e-34L,
     1.1516335620771950280586881493298221114818040761309e-37L,
};

static const long double mg_cos_polynomial[] = {
    -0.5L,
     0.041666666666666666666666666666666666666666666666667L,
    -0.0013888888888888888888888888888888888888888888888889L,
     0.000024801587301587301587301587301587301587301587301587L,
    -2.7557319223985890652557319223985890652557319223986e-7L,
     2.0876756987868098979210090321201432312543423654535e-9L,
    -1.1470745597729724713851697978682105666232650359634e-11L,
     4.7794773323873852974382074911175440275969376498477e-14L,
    -1.5619206968586226462216364350057333423519404084470e-16L,
     4.1103176233121648584779906184361403746103694959131e-19L,
    -8.8967913924505732867488974425024683433124880863919e-22L,
     1.6117375710961183490487133048011718013247261026072e-24L,
    -2.4795962632247974600749435458479566174226555424727e-27L,
     3.2798892370698379101520417273121119278077454265511e-30L,
    -3.7699876288159056438529215256461056641468338236220e-33L,
     3.8003907548547435925936708927884129678899534512318e-36L,
};

static long double mg_polynomial(const long double *coefficients,
                                 unsigned count, long double value)
{
    long double result = coefficients[count - 1];
    for (unsigned index = count - 1; index != 0; --index)
        result = result * value + coefficients[index - 1];
    return result;
}

/* The reduced interval is [-pi/4, pi/4].  These Taylor polynomials are
 * evaluated in Horner form; their omitted tails are far below x87 precision
 * throughout that interval.  Keeping the evaluation in Mangrove arithmetic
 * also avoids the variable accuracy of the hardware FSINCOS instruction. */
static void mg_sincos_reduced(long double value, long double *sine,
                              long double *cosine)
{
    long double square = value * value;
    long double sin_poly = mg_polynomial(mg_sin_polynomial,
        sizeof mg_sin_polynomial / sizeof mg_sin_polynomial[0], square);
    long double cos_poly = mg_polynomial(mg_cos_polynomial,
        sizeof mg_cos_polynomial / sizeof mg_cos_polynomial[0], square);
    *sine = value + (value * square) * sin_poly;
    *cosine = 1.0L + square * cos_poly;
}

static void mg_sincos_core(long double value, long double *sine,
                           long double *cosine)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN) {
        *sine = value + value;
        *cosine = value + value;
        return;
    }
    if (category == FP_INFINITE) {
        (void)feraiseexcept(FE_INVALID);
        *sine = (long double)NAN;
        *cosine = (long double)NAN;
        return;
    }
    if (category == FP_ZERO) {
        *sine = value;
        *cosine = 1.0L;
        return;
    }

    if (fabsl(value) < 0x1p-64L) {
        *sine = value;
        *cosine = 1.0L;
        (void)feraiseexcept(FE_INEXACT);
        return;
    }

    unsigned quadrant;
    long double magnitude = fabsl(value);
    long double reduced;
    if (magnitude <= 0.5L) {
        quadrant = 0;
        reduced = magnitude;
    } else {
        reduced = mg_reduce_quadrant(magnitude, &quadrant);
    }
    if (__mg_signbitl(value)) {
        reduced = -reduced;
        quadrant = (0U - quadrant) & 3U;
    }

    long double reduced_sine;
    long double reduced_cosine;
    mg_sincos_reduced(reduced, &reduced_sine, &reduced_cosine);
    switch (quadrant) {
    case 0:
        *sine = reduced_sine;
        *cosine = reduced_cosine;
        break;
    case 1:
        *sine = reduced_cosine;
        *cosine = -reduced_sine;
        break;
    case 2:
        *sine = -reduced_sine;
        *cosine = -reduced_cosine;
        break;
    default:
        *sine = -reduced_cosine;
        *cosine = reduced_sine;
        break;
    }
    (void)feraiseexcept(FE_INEXACT);
}

long double sinl(long double value)
{
    long double sine, cosine;
    mg_sincos_core(value, &sine, &cosine);
    if (__mg_fpclassifyl(value) == FP_SUBNORMAL)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    return sine;
}

float sinf(float value)
{
    float result = (float)sinl((long double)value);
    if (__mg_fpclassifyf(value) == FP_SUBNORMAL)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    return result;
}

double sin(double value)
{
    double result = (double)sinl((long double)value);
    if (__mg_fpclassify(value) == FP_SUBNORMAL)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    return result;
}

long double cosl(long double value)
{
    long double sine, cosine;
    mg_sincos_core(value, &sine, &cosine);
    return cosine;
}

float cosf(float value) { return (float)cosl((long double)value); }
double cos(double value) { return (double)cosl((long double)value); }

long double tanl(long double value)
{
    long double sine, cosine;
    mg_sincos_core(value, &sine, &cosine);
    long double result = sine / cosine;
    if (__mg_fpclassifyl(value) == FP_SUBNORMAL)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    return result;
}

float tanf(float value)
{
    float result = (float)tanl((long double)value);
    if (__mg_fpclassifyf(value) == FP_SUBNORMAL)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    return result;
}

double tan(double value)
{
    double result = (double)tanl((long double)value);
    if (__mg_fpclassify(value) == FP_SUBNORMAL)
        (void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
    return result;
}
