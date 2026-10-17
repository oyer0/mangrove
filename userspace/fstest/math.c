/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <mangrove.h>
#include <stdio.h>

int fstest_math_phase(void)
{
    if (sizeof(long double) != 16 || LDBL_MANT_DIG != 64 ||
        LDBL_MAX_EXP != 16384 || fegetround() != FE_TONEAREST)
        return 51;

    fenv_t saved;
    if (fegetenv(&saved) != 0 || fesetround(FE_DOWNWARD) != 0 ||
        nearbyint(0.5) != 0.0 || nearbyintl(0.5L) != 0.0L ||
        fegetround() != FE_DOWNWARD)
        return 52;

    /* A syscall and scheduler handoff must preserve x87 and SSE state. */
    if (feraiseexcept(FE_DIVBYZERO) != 0 || process_sleep_ms(10) != MG_OK ||
        fegetround() != FE_DOWNWARD || nearbyint(0.5) != 0.0 ||
        nearbyintl(0.5L) != 0.0L ||
        !(fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO) ||
        fesetround(FE_UPWARD) != 0 || nearbyint(0.5) != 1.0 ||
        nearbyintl(0.5L) != 1.0L || fesetenv(&saved) != 0)
        return 53;

    int exponent;
    double integral;
    long double lintegral;
    const double fraction = modf(-3.25, &integral);
    const long double lfraction = modfl(0x1.0000000000000008p+0L, &lintegral);
    volatile double invalid_root = sqrt(-1.0);
    if (integral != -3.0 || fraction != -0.25 || lintegral != 1.0L ||
        lfraction != 0x1p-61L || frexp(8.0, &exponent) != 0.5 || exponent != 4 ||
        frexpl(0x1p-16445L, &exponent) != 0.5L || exponent != -16444 ||
        fpclassify(0x1p-16445L) != FP_SUBNORMAL ||
        fabs(-0.0) != 0.0 || !signbit(-0.0L) ||
        !isnan(invalid_root) || isinf(invalid_root) ||
        fmin(NAN, 4.0) != 4.0 ||
        nextafterl(1.0L, 2.0L) <= 1.0L ||
        round(-0.5) != -1.0 || floor(1.75) != 1.0 || ceil(-1.75) != -1.0)
        return 54;

    const float fs = sqrtf(2.0f);
    const double ds = sqrt(2.0);
    const long double ls = sqrtl(2.0L);
    int next_f_flags, next_d_flags, next_l_flags;
    int ilogb_f_flags, ilogb_d_flags, ilogb_l_flags;
    int ilogb_f_value, ilogb_d_value, ilogb_l_value;
    volatile float next_f;
    volatile double next_d;
    volatile long double next_l;
    if (feclearexcept(FE_ALL_EXCEPT) != 0)
        return 56;
    next_f = nextafterf(0.0f, 1.0f);
    next_f_flags = fetestexcept(FE_UNDERFLOW | FE_INEXACT);
    if (feclearexcept(FE_ALL_EXCEPT) != 0)
        return 56;
    next_d = nextafter(0.0, 1.0);
    next_d_flags = fetestexcept(FE_UNDERFLOW | FE_INEXACT);
    if (feclearexcept(FE_ALL_EXCEPT) != 0)
        return 56;
    next_l = nextafterl(0.0L, 1.0L);
    next_l_flags = fetestexcept(FE_UNDERFLOW | FE_INEXACT);
    if (next_f != FLT_TRUE_MIN || next_d != DBL_TRUE_MIN ||
        next_l != LDBL_TRUE_MIN ||
        next_f_flags != (FE_UNDERFLOW | FE_INEXACT) ||
        next_d_flags != (FE_UNDERFLOW | FE_INEXACT) ||
        next_l_flags != (FE_UNDERFLOW | FE_INEXACT))
        return 56;
    if (feclearexcept(FE_ALL_EXCEPT) != 0)
        return 57;
    ilogb_f_value = ilogbf(0.0f);
    ilogb_f_flags = fetestexcept(FE_INVALID);
    if (feclearexcept(FE_ALL_EXCEPT) != 0)
        return 57;
    ilogb_d_value = ilogb(INFINITY);
    ilogb_d_flags = fetestexcept(FE_INVALID);
    if (feclearexcept(FE_ALL_EXCEPT) != 0)
        return 57;
    ilogb_l_value = ilogbl(NAN);
    ilogb_l_flags = fetestexcept(FE_INVALID);
    if (ilogb_f_value != FP_ILOGB0 || ilogb_d_value != INT_MAX ||
        ilogb_l_value != FP_ILOGBNAN || !ilogb_f_flags ||
        !ilogb_d_flags || !ilogb_l_flags)
        return 57;

    if (printf("MATH1 abi=16 mant=%d max_exp=%d\n", LDBL_MANT_DIG,
               LDBL_MAX_EXP) < 0 ||
        printf("F sqrt 0x1p+1 %a\n", (double)fs) < 0 ||
        printf("D sqrt 0x1p+1 %a\n", ds) < 0 ||
        printf("L sqrt 0x1p+1 %La\n", ls) < 0 ||
        printf("NEXT1 f=%d d=%d l=%d flags=%d/%d/%d\n",
               next_f == FLT_TRUE_MIN, next_d == DBL_TRUE_MIN,
               next_l == LDBL_TRUE_MIN, next_f_flags, next_d_flags,
               next_l_flags) < 0 ||
        printf("ILOGB1 f=%d d=%d l=%d flags=%d/%d/%d\n",
               ilogb_f_value == FP_ILOGB0,
               ilogb_d_value == INT_MAX,
               ilogb_l_value == FP_ILOGBNAN, ilogb_f_flags,
               ilogb_d_flags, ilogb_l_flags) < 0 ||
        puts("MATH1 complete records=3 fenv=syscall-context exceptions=verified") == EOF)
        return 55;
    return 0;
}
