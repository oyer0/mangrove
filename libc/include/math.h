/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <float.h>
#include <limits.h>

/* Classification values are distinct; their numeric values are Mangrove's
 * implementation choice, as permitted by C17. */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define FP_ILOGB0    (-INT_MAX)
#define FP_ILOGBNAN  INT_MAX

#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define MATH_ERRNO    1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT

int __mg_fpclassifyf(float);
int __mg_fpclassify(double);
int __mg_fpclassifyl(long double);
int __mg_signbitf(float);
int __mg_signbit(double);
int __mg_signbitl(long double);
int __mg_isfinitef(float);
int __mg_isfinite(double);
int __mg_isfinitel(long double);
int __mg_isgreaterf(float, float);
int __mg_isgreater(double, double);
int __mg_isgreaterl(long double, long double);
int __mg_isgreaterequalf(float, float);
int __mg_isgreaterequal(double, double);
int __mg_isgreaterequall(long double, long double);
int __mg_islessf(float, float);
int __mg_isless(double, double);
int __mg_islessl(long double, long double);
int __mg_islessequalf(float, float);
int __mg_islessequal(double, double);
int __mg_islessequall(long double, long double);
int __mg_islessgreaterf(float, float);
int __mg_islessgreater(double, double);
int __mg_islessgreaterl(long double, long double);
int __mg_isunorderedf(float, float);
int __mg_isunordered(double, double);
int __mg_isunorderedl(long double, long double);

#define __MG_FP_TYPE(x) _Generic((x), float: (float)0, \
    long double: (long double)0, default: (double)0)
#define fpclassify(x) _Generic(__MG_FP_TYPE(x), \
    float: __mg_fpclassifyf, double: __mg_fpclassify, \
    long double: __mg_fpclassifyl)((x))
#define signbit(x) _Generic(__MG_FP_TYPE(x), \
    float: __mg_signbitf, double: __mg_signbit, \
    long double: __mg_signbitl)((x))
#define isfinite(x) _Generic(__MG_FP_TYPE(x), \
    float: __mg_isfinitef, double: __mg_isfinite, \
    long double: __mg_isfinitel)((x))
#define isinf(x) (fpclassify(x) == FP_INFINITE)
#define isnan(x) (fpclassify(x) == FP_NAN)
#define isnormal(x) (fpclassify(x) == FP_NORMAL)

#define __MG_CMP_SELECT(x, y, sf, sd, sl) \
    _Generic((__MG_FP_TYPE(x) + __MG_FP_TYPE(y)), \
        float: sf, double: sd, long double: sl)((x), (y))
#define isgreater(x, y) __MG_CMP_SELECT(x, y, __mg_isgreaterf, __mg_isgreater, __mg_isgreaterl)
#define isgreaterequal(x, y) __MG_CMP_SELECT(x, y, __mg_isgreaterequalf, __mg_isgreaterequal, __mg_isgreaterequall)
#define isless(x, y) __MG_CMP_SELECT(x, y, __mg_islessf, __mg_isless, __mg_islessl)
#define islessequal(x, y) __MG_CMP_SELECT(x, y, __mg_islessequalf, __mg_islessequal, __mg_islessequall)
#define islessgreater(x, y) __MG_CMP_SELECT(x, y, __mg_islessgreaterf, __mg_islessgreater, __mg_islessgreaterl)
#define isunordered(x, y) __MG_CMP_SELECT(x, y, __mg_isunorderedf, __mg_isunordered, __mg_isunorderedl)

float fabsf(float);
double fabs(double);
long double fabsl(long double);
float copysignf(float, float);
double copysign(double, double);
long double copysignl(long double, long double);
float fminf(float, float);
double fmin(double, double);
long double fminl(long double, long double);
float fmaxf(float, float);
double fmax(double, double);
long double fmaxl(long double, long double);
float fdimf(float, float);
double fdim(double, double);
long double fdiml(long double, long double);

float truncf(float);
double trunc(double);
long double truncl(long double);
float floorf(float);
double floor(double);
long double floorl(long double);
float ceilf(float);
double ceil(double);
long double ceill(long double);
float roundf(float);
double round(double);
long double roundl(long double);
float rintf(float);
double rint(double);
long double rintl(long double);
float nearbyintf(float);
double nearbyint(double);
long double nearbyintl(long double);

float modff(float, float *);
double modf(double, double *);
long double modfl(long double, long double *);
float frexpf(float, int *);
double frexp(double, int *);
long double frexpl(long double, int *);
int ilogbf(float);
int ilogb(double);
int ilogbl(long double);
float logbf(float);
double logb(double);
long double logbl(long double);

float nextafterf(float, float);
double nextafter(double, double);
long double nextafterl(long double, long double);
float nexttowardf(float, long double);
double nexttoward(double, long double);
long double nexttowardl(long double, long double);

float sqrtf(float);
double sqrt(double);
long double sqrtl(long double);
float cbrtf(float);
double cbrt(double);
long double cbrtl(long double);
float hypotf(float, float);
double hypot(double, double);
long double hypotl(long double, long double);

float ldexpf(float, int);
double ldexp(double, int);
long double ldexpl(long double, int);
float scalbnf(float, int);
double scalbn(double, int);
long double scalbnl(long double, int);
float scalblnf(float, long);
double scalbln(double, long);
long double scalblnl(long double, long);

float fmodf(float, float);
double fmod(double, double);
long double fmodl(long double, long double);
float remainderf(float, float);
double remainder(double, double);
long double remainderl(long double, long double);
float remquof(float, float, int *);
double remquo(double, double, int *);
long double remquol(long double, long double, int *);
