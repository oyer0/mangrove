/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* C17 type-generic selection for the currently supplied real math family.
 * The controlling expressions are unevaluated; each argument is passed once
 * to the selected function. Complex math and other unimplemented families are
 * deliberately absent. */
#include <math.h>

#define __MG_TG_TYPE(x) _Generic((x), float: (float)0, \
    long double: (long double)0, default: (double)0)
#define __MG_TG_PICK_UNARY(x, f, d, l) \
    _Generic(__MG_TG_TYPE(x), float: f, double: d, long double: l)
#define __MG_TG_UNARY(x, f, d, l) __MG_TG_PICK_UNARY((x), f, d, l)((x))
#define __MG_TG_BINARY(x, y, f, d, l) \
    _Generic((__MG_TG_TYPE(x) + __MG_TG_TYPE(y)), \
        float: f, double: d, long double: l)((x), (y))

#define fabs(x) __MG_TG_UNARY((x), fabsf, fabs, fabsl)
#define ceil(x) __MG_TG_UNARY((x), ceilf, ceil, ceill)
#define floor(x) __MG_TG_UNARY((x), floorf, floor, floorl)
#define trunc(x) __MG_TG_UNARY((x), truncf, trunc, truncl)
#define round(x) __MG_TG_UNARY((x), roundf, round, roundl)
#define rint(x) __MG_TG_UNARY((x), rintf, rint, rintl)
#define nearbyint(x) __MG_TG_UNARY((x), nearbyintf, nearbyint, nearbyintl)
#define sqrt(x) __MG_TG_UNARY((x), sqrtf, sqrt, sqrtl)
#define ldexp(x, n) __MG_TG_PICK_UNARY((x), ldexpf, ldexp, ldexpl)((x), (n))
#define scalbn(x, n) __MG_TG_PICK_UNARY((x), scalbnf, scalbn, scalbnl)((x), (n))
#define scalbln(x, n) __MG_TG_PICK_UNARY((x), scalblnf, scalbln, scalblnl)((x), (n))
#define frexp(x, e) __MG_TG_PICK_UNARY((x), frexpf, frexp, frexpl)((x), (e))
#define modf(x, i) __MG_TG_PICK_UNARY((x), modff, modf, modfl)((x), (i))
#define ilogb(x) __MG_TG_UNARY((x), ilogbf, ilogb, ilogbl)
#define logb(x) __MG_TG_UNARY((x), logbf, logb, logbl)
#define copysign(x, y) __MG_TG_BINARY((x), (y), copysignf, copysign, copysignl)
#define fmin(x, y) __MG_TG_BINARY((x), (y), fminf, fmin, fminl)
#define fmax(x, y) __MG_TG_BINARY((x), (y), fmaxf, fmax, fmaxl)
#define fdim(x, y) __MG_TG_BINARY((x), (y), fdimf, fdim, fdiml)
#define nextafter(x, y) __MG_TG_BINARY((x), (y), nextafterf, nextafter, nextafterl)
#define nexttoward(x, y) __MG_TG_PICK_UNARY((x), nexttowardf, nexttoward, nexttowardl)((x), (y))
#define fmod(x, y) __MG_TG_BINARY((x), (y), fmodf, fmod, fmodl)
#define remainder(x, y) \
    __MG_TG_BINARY((x), (y), remainderf, remainder, remainderl)
#define remquo(x, y, q) \
    _Generic((__MG_TG_TYPE(x) + __MG_TG_TYPE(y)), \
        float: remquof, double: remquo, long double: remquol)((x), (y), (q))
