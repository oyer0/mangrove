/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <fenv.h>
#include <limits.h>
#include <math.h>

_Static_assert(sizeof(long) == sizeof(long long),
               "Mangrove's x86-64 integer rounding ABI is LP64");
_Static_assert(LONG_MIN == LLONG_MIN && LONG_MAX == LLONG_MAX,
               "Mangrove's long and long long ranges must match");

static long long mg_integer_round(long double value, int current_mode)
{
    int category = __mg_fpclassifyl(value);
    if (category == FP_NAN || category == FP_INFINITE) {
        (void)feraiseexcept(FE_INVALID);
        return 0;
    }

    long double rounded = current_mode ? rintl(value) : roundl(value);
    if (rounded < (long double)LLONG_MIN ||
        rounded > (long double)LLONG_MAX) {
        (void)feraiseexcept(FE_INVALID);
        return 0;
    }
    return (long long)rounded;
}

#define MG_INTEGER_ROUND(suffix, type) \
long lrint##suffix(type value) \
{ return (long)mg_integer_round((long double)value, 1); } \
long long llrint##suffix(type value) \
{ return mg_integer_round((long double)value, 1); } \
long lround##suffix(type value) \
{ return (long)mg_integer_round((long double)value, 0); } \
long long llround##suffix(type value) \
{ return mg_integer_round((long double)value, 0); }

MG_INTEGER_ROUND(f, float)
MG_INTEGER_ROUND(, double)
MG_INTEGER_ROUND(l, long double)
