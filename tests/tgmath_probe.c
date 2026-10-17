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
        !TYPE_IS(ldexp(f, 1), float) ||
        !TYPE_IS(scalbn(d, 1), double) ||
        !TYPE_IS(scalbln(l, 1L), long double) ||
        !TYPE_IS(fmod(f, d), double) ||
        !TYPE_IS(remainder(f, f), float) ||
        !TYPE_IS(remquo(l, f, &exponent), long double) ||
        !TYPE_IS(frexp(f, &exponent), float) ||
        !TYPE_IS(modf(d, &d), double))
        return 1;

    (void)fabs(next_float());
    if (calls != 1)
        return 2;
    (void)fmin(next_float(), next_float());
    if (calls != 3)
        return 3;
    return 0;
}
