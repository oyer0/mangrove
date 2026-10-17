/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <tgmath.h>

#define TYPE_IS(expression, type) _Generic((expression), type: 1, default: 0)

static unsigned calls;
static float next_float(void) { ++calls; return 0.25f; }

int main(void)
{
    float f = 0.5f;
    double d = 0.5;
    long double l = 0.5L;
    int exponent;

    if (!TYPE_IS(fabs(f), float) || !TYPE_IS(fabs(d), double) ||
        !TYPE_IS(fabs(l), long double) || !TYPE_IS(fabs(1), double) ||
        !TYPE_IS(sqrt(f), float) || !TYPE_IS(sqrt(d), double) ||
        !TYPE_IS(sqrt(l), long double) || !TYPE_IS(fmin(f, f), float) ||
        !TYPE_IS(fmin(f, 1), double) || !TYPE_IS(copysign(l, f), long double) ||
        !TYPE_IS(nextafter(f, f), float) || !TYPE_IS(nextafter(f, d), double) ||
        !TYPE_IS(nexttoward(f, l), float) ||
        !TYPE_IS(cbrt(f), float) || !TYPE_IS(cbrt(d), double) ||
        !TYPE_IS(cbrt(l), long double) ||
        !TYPE_IS(exp(f), float) || !TYPE_IS(exp2(d), double) ||
        !TYPE_IS(expm1(l), long double) || !TYPE_IS(log(f), float) ||
        !TYPE_IS(log2(d), double) || !TYPE_IS(log10(l), long double) ||
        !TYPE_IS(log1p(f), float) || !TYPE_IS(sinh(f), float) ||
        !TYPE_IS(cosh(d), double) || !TYPE_IS(tanh(l), long double) ||
        !TYPE_IS(asinh(f), float) || !TYPE_IS(acosh(d), double) ||
        !TYPE_IS(atanh(l), long double) ||
        !TYPE_IS(atan(f), float) || !TYPE_IS(atan(d), double) ||
        !TYPE_IS(atan(l), long double) ||
        !TYPE_IS(asin(f), float) || !TYPE_IS(asin(d), double) ||
        !TYPE_IS(asin(l), long double) ||
        !TYPE_IS(acos(f), float) || !TYPE_IS(acos(d), double) ||
        !TYPE_IS(acos(l), long double) ||
        !TYPE_IS(atan2(f, f), float) || !TYPE_IS(atan2(f, d), double) ||
        !TYPE_IS(atan2(l, f), long double) ||
        !TYPE_IS(sin(f), float) || !TYPE_IS(sin(d), double) ||
        !TYPE_IS(sin(l), long double) ||
        !TYPE_IS(cos(f), float) || !TYPE_IS(cos(d), double) ||
        !TYPE_IS(cos(l), long double) ||
        !TYPE_IS(tan(f), float) || !TYPE_IS(tan(d), double) ||
        !TYPE_IS(tan(l), long double) ||
        !TYPE_IS(lrint(f), long) || !TYPE_IS(lrint(d), long) ||
        !TYPE_IS(lrint(l), long) ||
        !TYPE_IS(llrint(f), long long) || !TYPE_IS(llrint(d), long long) ||
        !TYPE_IS(llrint(l), long long) ||
        !TYPE_IS(lround(f), long) || !TYPE_IS(lround(d), long) ||
        !TYPE_IS(lround(l), long) ||
        !TYPE_IS(llround(f), long long) || !TYPE_IS(llround(d), long long) ||
        !TYPE_IS(llround(l), long long) ||
        !TYPE_IS(ldexp(f, 1), float) ||
        !TYPE_IS(scalbn(d, 1), double) ||
        !TYPE_IS(scalbln(l, 1L), long double) ||
        !TYPE_IS(fmod(f, d), double) ||
        !TYPE_IS(remainder(f, f), float) ||
        !TYPE_IS(hypot(f, d), double) ||
        !TYPE_IS(hypot(l, f), long double) ||
        !TYPE_IS(remquo(l, f, &exponent), long double) ||
        !TYPE_IS(fma(f, f, f), float) ||
        !TYPE_IS(fma(f, d, f), double) ||
        !TYPE_IS(fma(f, d, l), long double) ||
        !TYPE_IS(pow(f, f), float) || !TYPE_IS(pow(f, d), double) ||
        !TYPE_IS(pow(l, f), long double) ||
        !TYPE_IS(frexp(f, &exponent), float) ||
        !TYPE_IS(modf(d, &d), double))
        return 1;

    (void)fabs(next_float());
    if (calls != 1)
        return 2;
    (void)fmin(next_float(), next_float());
    if (calls != 3)
        return 3;
    (void)pow(next_float(), next_float());
    if (calls != 5)
        return 4;
    return 0;
}
