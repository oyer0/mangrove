/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <math.h>

#if !defined(__x86_64__)
#error Mangrove scaling and remainder require the x86-64 x87 ABI
#endif
_Static_assert(sizeof(long) == sizeof(int64_t),
               "scalbln requires the x86-64 LP64 long ABI");

/* FSCALE changes only the exponent.  Keeping the operation in the target
 * extended format lets float and double results be rounded once on return,
 * while long double is evaluated directly at x87 precision. */
static long double mg_x87_scale(long double value, long exponent)
{
    long double result;
    __asm__ volatile("fldt %[value]\n\t"
                     "fildq %[exponent]\n\t"
                     "fxch %%st(1)\n\t"
                     "fscale\n\t"
                     "fstpt %[result]\n\t"
                     "fstp %%st(0)"
                     : [result] "=m" (result)
                     : [value] "m" (value), [exponent] "m" (exponent)
                     : "st", "st(1)", "memory");
    return result;
}

/* FPREM and FPREM1 perform exact binary remainder reduction.  Large exponent
 * gaps produce partial reductions; C2 requests another iteration.  On the
 * final reduction, x87 reports the low three quotient bits in C0/C3/C1. */
static long double mg_x87_rem(long double x, long double y, int nearest)
{
    long double result;
    unsigned short status;
    if (nearest) {
        __asm__ volatile("fldt %[x]\n\t"
                         "fldt %[y]\n\t"
                         "fxch %%st(1)\n\t"
                         "1: fprem1\n\t"
                         "fnstsw %%ax\n\t"
                         "testw $0x0400, %%ax\n\t"
                         "jnz 1b\n\t"
                         "fstpt %[result]\n\t"
                         "fstp %%st(0)"
                         : [result] "=m" (result), "=a" (status)
                         : [x] "m" (x), [y] "m" (y)
                         : "st", "st(1)", "cc", "memory");
    } else {
        __asm__ volatile("fldt %[x]\n\t"
                         "fldt %[y]\n\t"
                         "fxch %%st(1)\n\t"
                         "1: fprem\n\t"
                         "fnstsw %%ax\n\t"
                         "testw $0x0400, %%ax\n\t"
                         "jnz 1b\n\t"
                         "fstpt %[result]\n\t"
                         "fstp %%st(0)"
                         : [result] "=m" (result), "=a" (status)
                         : [x] "m" (x), [y] "m" (y)
                         : "st", "st(1)", "cc", "memory");
    }
    return result;
}

/* remquo reports at least the three low quotient bits.  Compute those bits
 * with bounded binary long division instead of depending on optional x87
 * status-word quotient fields, which are not consistently emulated. */
static unsigned mg_quotient_low(long double x, long double y)
{
    mg_x87_value xr = mg_x87_unpack(x);
    mg_x87_value yr = mg_x87_unpack(y);
    unsigned xe = mg_x87_exponent(xr);
    unsigned ye = mg_x87_exponent(yr);
    if (xe == 0) xe = 1;
    if (ye == 0) ye = 1;
    int exponent_gap = (int)xe - (int)ye;

    if (exponent_gap < -1)
        return 0;
    if (exponent_gap == -1)
        return xr.significand > yr.significand ? 1U : 0U;

    uint64_t denominator = yr.significand;
    unsigned __int128 remainder = 0;
    unsigned quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((xr.significand >> bit) & 1U);
        unsigned digit = remainder >= denominator;
        if (digit) remainder -= denominator;
        quotient = ((quotient << 1) | digit) & 7U;
    }
    for (int bit = 0; bit < exponent_gap; ++bit) {
        remainder <<= 1;
        unsigned digit = remainder >= denominator;
        if (digit) remainder -= denominator;
        quotient = ((quotient << 1) | digit) & 7U;
    }

    unsigned __int128 twice_remainder = remainder << 1;
    if (twice_remainder > denominator ||
        (twice_remainder == denominator && (quotient & 1U)))
        quotient = (quotient + 1U) & 7U;
    return quotient;
}

static long double mg_scale_checked(long double x, long n, int category)
{
    if (category == FP_NAN)
        return x + x;
    if (category == FP_INFINITE || category == FP_ZERO)
        return x;
    return mg_x87_scale(x, n);
}

float scalblnf(float x, long n)
{ return (float)mg_scale_checked((long double)x, n, __mg_fpclassifyf(x)); }
double scalbln(double x, long n)
{ return (double)mg_scale_checked((long double)x, n, __mg_fpclassify(x)); }
long double scalblnl(long double x, long n)
{ return mg_scale_checked(x, n, __mg_fpclassifyl(x)); }

float scalbnf(float x, int n) { return scalblnf(x, (long)n); }
double scalbn(double x, int n) { return scalbln(x, (long)n); }
long double scalbnl(long double x, int n) { return scalblnl(x, (long)n); }
float ldexpf(float x, int n) { return scalbnf(x, n); }
double ldexp(double x, int n) { return scalbn(x, n); }
long double ldexpl(long double x, int n) { return scalbnl(x, n); }

static long double mg_remainder_checked(long double x, long double y,
                                        int nearest, int *quotient,
                                        int x_category, int y_category)
{
    if (quotient)
        *quotient = 0;
    if (x_category == FP_NAN || y_category == FP_NAN)
        return x + y;
    if (x_category == FP_INFINITE || y_category == FP_ZERO) {
        (void)feraiseexcept(FE_INVALID);
        return (long double)NAN;
    }
    if (y_category == FP_INFINITE || x_category == FP_ZERO)
        return x;

    long double ax = fabsl(x);
    long double ay = fabsl(y);
    long double result = mg_x87_rem(ax, ay, nearest);
    if (signbit(x))
        result = -result;
    if (quotient && nearest) {
        int negative = signbit(x) != signbit(y);
        unsigned low = mg_quotient_low(ax, ay);
        *quotient = negative ? -(int)low : (int)low;
    }
    return result;
}

float fmodf(float x, float y)
{
    return (float)mg_remainder_checked((long double)x, (long double)y, 0,
                                       NULL, __mg_fpclassifyf(x),
                                       __mg_fpclassifyf(y));
}

double fmod(double x, double y)
{
    return (double)mg_remainder_checked((long double)x, (long double)y, 0,
                                        NULL, __mg_fpclassify(x),
                                        __mg_fpclassify(y));
}

long double fmodl(long double x, long double y)
{
    return mg_remainder_checked(x, y, 0, NULL, __mg_fpclassifyl(x),
                                __mg_fpclassifyl(y));
}

float remainderf(float x, float y)
{
    return (float)mg_remainder_checked((long double)x, (long double)y, 1,
                                       NULL, __mg_fpclassifyf(x),
                                       __mg_fpclassifyf(y));
}

double remainder(double x, double y)
{
    return (double)mg_remainder_checked((long double)x, (long double)y, 1,
                                        NULL, __mg_fpclassify(x),
                                        __mg_fpclassify(y));
}

long double remainderl(long double x, long double y)
{
    return mg_remainder_checked(x, y, 1, NULL, __mg_fpclassifyl(x),
                                __mg_fpclassifyl(y));
}

float remquof(float x, float y, int *quotient)
{
    return (float)mg_remainder_checked((long double)x, (long double)y, 1,
                                       quotient, __mg_fpclassifyf(x),
                                       __mg_fpclassifyf(y));
}

double remquo(double x, double y, int *quotient)
{
    return (double)mg_remainder_checked((long double)x, (long double)y, 1,
                                        quotient, __mg_fpclassify(x),
                                        __mg_fpclassify(y));
}

long double remquol(long double x, long double y, int *quotient)
{
    return mg_remainder_checked(x, y, 1, quotient, __mg_fpclassifyl(x),
                                __mg_fpclassifyl(y));
}
