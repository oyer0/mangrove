/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <math.h>

/* Mangrove's x86-64 GCC compiler provides the C complex scalar types. */
#define complex _Complex
#define _Complex_I (__builtin_complex(0.0f, 1.0f))
#define I _Complex_I
#define CMPLXF(real_value, imag_value) \
    __builtin_complex((float)(real_value), (float)(imag_value))
#define CMPLX(real_value, imag_value) \
    __builtin_complex((double)(real_value), (double)(imag_value))
#define CMPLXL(real_value, imag_value) \
    __builtin_complex((long double)(real_value), (long double)(imag_value))

float complex cacosf(float complex);
double complex cacos(double complex);
long double complex cacosl(long double complex);
float complex casinf(float complex);
double complex casin(double complex);
long double complex casinl(long double complex);
float complex catanf(float complex);
double complex catan(double complex);
long double complex catanl(long double complex);
float complex ccosf(float complex);
double complex ccos(double complex);
long double complex ccosl(long double complex);
float complex csinf(float complex);
double complex csin(double complex);
long double complex csinl(long double complex);
float complex ctanf(float complex);
double complex ctan(double complex);
long double complex ctanl(long double complex);
float complex cacoshf(float complex);
double complex cacosh(double complex);
long double complex cacoshl(long double complex);
float complex casinhf(float complex);
double complex casinh(double complex);
long double complex casinhl(long double complex);
float complex catanhf(float complex);
double complex catanh(double complex);
long double complex catanhl(long double complex);
float complex ccoshf(float complex);
double complex ccosh(double complex);
long double complex ccoshl(long double complex);
float complex csinhf(float complex);
double complex csinh(double complex);
long double complex csinhl(long double complex);
float complex ctanhf(float complex);
double complex ctanh(double complex);
long double complex ctanhl(long double complex);
float complex cexpf(float complex);
double complex cexp(double complex);
long double complex cexpl(long double complex);
float complex clogf(float complex);
double complex clog(double complex);
long double complex clogl(long double complex);
float cabsf(float complex);
double cabs(double complex);
long double cabsl(long double complex);
float complex cpowf(float complex, float complex);
double complex cpow(double complex, double complex);
long double complex cpowl(long double complex, long double complex);
float complex csqrtf(float complex);
double complex csqrt(double complex);
long double complex csqrtl(long double complex);
float cargf(float complex);
double carg(double complex);
long double cargl(long double complex);
float cimagf(float complex);
double cimag(double complex);
long double cimagl(long double complex);
float complex conjf(float complex);
double complex conj(double complex);
long double complex conjl(long double complex);
float complex cprojf(float complex);
double complex cproj(double complex);
long double complex cprojl(long double complex);
float crealf(float complex);
double creal(double complex);
long double creall(long double complex);
