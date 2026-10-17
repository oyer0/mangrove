/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <mangrove.h>
#include <stdio.h>

static unsigned short x87_control_word(void)
{
    unsigned short value;
    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
}

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
    const float scaled_f = scalbnf(1.25f, 4);
    const double rem_d = fmod(-7.25, 2.0);
    const double nearest_d = remainder(7.0, 2.0);
    int remquo_l_bits;
    const long double rem_l = remquol(-29.0L, 4.0L, &remquo_l_bits);
    const long double scaled_l = scalblnl(1.25L, 64);
    const float root_f = cbrtf(-8.0f);
    const double hyp_d = hypot(3.0, 4.0);
    const long double root_l = cbrtl(4096.0L);
    const long double hyp_l = hypotl(3.0L, 4.0L);
    const float exp_f = expf(1.0f), exp2_f = exp2f(0.5f);
    const float expm1_f = expm1f(0.25f), log_f = logf(2.0f);
    const float log2_f = log2f(8.0f), log10_f = log10f(100.0f);
    const float log1p_f = log1pf(0.5f);
    const double exp_d = exp(-1.0), exp2_d = exp2(-2.0);
    const double expm1_d = expm1(-0.125), log_d = log(0.5);
    const double log2_d = log2(0.25), log10_d = log10(0.1);
    const double log1p_d = log1p(-0.25);
    const long double exp_l = expl(1.0L), exp2_l = exp2l(3.25L);
    const long double expm1_l = expm1l(0x1p-60L), log_l = logl(10.0L);
    const long double log2_l = log2l(16.0L), log10_l = log10l(1000.0L);
    const long double log1p_l = log1pl(0x1p-60L);
    const float sinh_f = sinhf(0.5f), asinh_f = asinhf(0.5f);
    const double cosh_d = cosh(0.5), acosh_d = acosh(2.0);
    const long double tanh_l = tanhl(20.0L), atanh_l = atanhl(0.75L);
    const float atan_f = atanf(0.5f), asin_f = asinf(0.5f);
    const float acos_f = acosf(0.5f);
    const double atan_d = atan(-0.75), asin_d = asin(-0.25);
    const double acos_d = acos(-0.25), atan2_d = atan2(1.0, -1.0);
    const long double atan_l = atanl(0x1p80L), asin_l = asinl(0.75L);
    const long double acos_l = acosl(0.75L), atan2_l = atan2l(-3.0L, 4.0L);
    const float sin_f = sinf(0x1.234p+20f), cos_f = cosf(-0x1.abc6p+18f);
    const float tan_f = tanf(0x1.2p-2f);
    const double sin_d = sin(0x1.23456789abcdep+200);
    const double cos_d = cos(-0x1.fedcba9876543p+140);
    const double tan_d = tan(0x1.8p-2);
    const long double sin_l = sinl(0x1.23456789abcdefp+1000L);
    const long double cos_l = cosl(-0x1.fedcba987654321p+900L);
    const long double tan_l = tanl(0x1.4p-2L);
    const float pow_f = powf(1.25f, 3.0f);
    const double pow_d = pow(1.125, -17.5);
    const long double pow_l = powl(0x1.0000000000000002p+0L, 128.0L);
    const float fused_f = fmaf(0x1.000002p+0f, 0x1.fffffcp-1f, -1.0f);
    const double fused_d = fma(1.0 + 0x1p-27, 1.0 - 0x1p-27, -1.0);
    const long double fused_l = fmal(1.0L + 0x1p-63L,
                                     1.0L - 0x1p-63L, -1.0L);
    const unsigned short final_x87_control = x87_control_word();
    if (scaled_f != 20.0f) return 58;
    if (rem_d != -1.25) return 59;
    if (nearest_d != -1.0) return 60;
    if (rem_l != -1.0L) return 61;
    if (remquo_l_bits != -7) {
        (void)printf("MATH diagnostic remquo=%La quotient=%d\n",
                     rem_l, remquo_l_bits);
        return 62;
    }
    if (scaled_l != 0x1.4p+64L) return 63;
    if (root_f != -2.0f) return 64;
    if (hyp_d != 5.0) return 65;
    if (root_l != 16.0L) return 66;
    if (hyp_l != 5.0L) return 67;
    if (!isfinite(exp_f) || !isfinite(exp_d) || !isfinite(exp_l) ||
        !isfinite(exp2_f) || !isfinite(exp2_d) || !isfinite(exp2_l) ||
        !isfinite(expm1_f) || !isfinite(expm1_d) || !isfinite(expm1_l) ||
        !isfinite(log_f) || !isfinite(log_d) || !isfinite(log_l) ||
        !isfinite(log2_f) || !isfinite(log2_d) || !isfinite(log2_l) ||
        !isfinite(log10_f) || !isfinite(log10_d) || !isfinite(log10_l) ||
        !isfinite(log1p_f) || !isfinite(log1p_d) || !isfinite(log1p_l) ||
        !isfinite(sinh_f) || !isfinite(asinh_f) || !isfinite(cosh_d) ||
        !isfinite(acosh_d) || !isfinite(tanh_l) || !isfinite(atanh_l) ||
        !signbit(sinh(-0.0)) || sinh(-0.0) != 0.0 ||
        cosh(-INFINITY) != INFINITY || tanh(INFINITY) != 1.0 ||
        !isfinite(atan_f) || !isfinite(atan_d) || !isfinite(atan_l) ||
        !isfinite(atan2_d) || !isfinite(atan2_l) || !isfinite(asin_f) ||
        !isfinite(asin_d) || !isfinite(asin_l) || !isfinite(acos_f) ||
        !isfinite(acos_d) || !isfinite(acos_l) ||
        !isfinite(sin_f) || !isfinite(sin_d) || !isfinite(sin_l) ||
        !isfinite(cos_f) || !isfinite(cos_d) || !isfinite(cos_l) ||
        !isfinite(tan_f) || !isfinite(tan_d) || !isfinite(tan_l) ||
        fused_f != -0x1p-46f || fused_d != -0x1p-54 ||
        fused_l != -0x1p-126L ||
        fma(DBL_MAX, 2.0, -DBL_MAX) != DBL_MAX ||
        fmal(LDBL_MAX, 2.0L, -LDBL_MAX) != LDBL_MAX ||
        !isnan(acoshl(-HUGE_VALL)))
        return 68;
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

    fenv_t power_environment;
    volatile long double negative_underflow;
    if (fegetenv(&power_environment) != 0 ||
        fesetround(FE_TOWARDZERO) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) != 0)
        return 69;
    negative_underflow = powl(-2.0L, -4294967297.0L);
    if (negative_underflow != 0.0L || !signbit(negative_underflow) ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
        (FE_UNDERFLOW | FE_INEXACT) || fesetround(FE_DOWNWARD) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) != 0)
        return 69;
    negative_underflow = powl(-2.0L, -4294967297.0L);
    if (negative_underflow != -LDBL_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
        (FE_UNDERFLOW | FE_INEXACT) || fesetenv(&power_environment) != 0)
        return 69;

    if (printf("MATH1 abi=16 mant=%d max_exp=%d x87cw=%04x\n",
               LDBL_MANT_DIG, LDBL_MAX_EXP, final_x87_control) < 0 ||
        printf("F sqrt 0x1p+1 %a\n", (double)fs) < 0 ||
        printf("D sqrt 0x1p+1 %a\n", ds) < 0 ||
        printf("L sqrt 0x1p+1 %La\n", ls) < 0 ||
        printf("F scalbn 0x1.4p+0 4 %a\n", (double)scaled_f) < 0 ||
        printf("D fmod -0x1.dp+2 0x1p+1 %a\n", rem_d) < 0 ||
        printf("D remainder 0x1.cp+2 0x1p+1 %a\n", nearest_d) < 0 ||
        printf("L remquo -0x1.dp+4 0x1p+2 %La %d\n", rem_l,
               remquo_l_bits) < 0 ||
        printf("L scalbn 0x1.4p+0 64 %La\n", scaled_l) < 0 ||
        printf("F cbrt -0x1p+3 %a\n", (double)root_f) < 0 ||
        printf("D hypot 0x1.8p+1 0x1p+2 %a\n", hyp_d) < 0 ||
        printf("L cbrt 0x1p+12 %La\n", root_l) < 0 ||
        printf("L hypot 0x1.8p+1 0x1p+2 %La\n", hyp_l) < 0 ||
        printf("F exp 0x1p+0 %a\n", (double)exp_f) < 0 ||
        printf("D exp -0x1p+0 %a\n", exp_d) < 0 ||
        printf("L exp 0x1p+0 %La\n", exp_l) < 0 ||
        printf("F exp2 0x1p-1 %a\n", (double)exp2_f) < 0 ||
        printf("D exp2 -0x1p+1 %a\n", exp2_d) < 0 ||
        printf("L exp2 0xdp-2 %La\n", exp2_l) < 0 ||
        printf("F expm1 0x1p-2 %a\n", (double)expm1_f) < 0 ||
        printf("D expm1 -0x1p-3 %a\n", expm1_d) < 0 ||
        printf("L expm1 0x1p-60 %La\n", expm1_l) < 0 ||
        printf("F log 0x1p+1 %a\n", (double)log_f) < 0 ||
        printf("D log 0x1p-1 %a\n", log_d) < 0 ||
        printf("L log 0xap+0 %La\n", log_l) < 0 ||
        printf("F log2 0x1p+3 %a\n", (double)log2_f) < 0 ||
        printf("D log2 0x1p-2 %a\n", log2_d) < 0 ||
        printf("L log2 0x1p+4 %La\n", log2_l) < 0 ||
        printf("F log10 0x1.9p+6 %a\n", (double)log10_f) < 0 ||
        printf("D log10 0x1.999999999999ap-4 %a\n", log10_d) < 0 ||
        printf("L log10 0xf.ap+6 %La\n", log10_l) < 0 ||
        printf("F log1p 0x1p-1 %a\n", (double)log1p_f) < 0 ||
        printf("D log1p -0x1p-2 %a\n", log1p_d) < 0 ||
        printf("L log1p 0x1p-60 %La\n", log1p_l) < 0 ||
        printf("F sinh 0x1p-1 %a\n", (double)sinh_f) < 0 ||
        printf("F asinh 0x1p-1 %a\n", (double)asinh_f) < 0 ||
        printf("D cosh 0x1p-1 %a\n", cosh_d) < 0 ||
        printf("D acosh 0x1p+1 %a\n", acosh_d) < 0 ||
        printf("L tanh 0xap+1 %La\n", tanh_l) < 0 ||
        printf("L atanh 0xcp-4 %La\n", atanh_l) < 0 ||
        printf("F atan 0x1p-1 %a\n", (double)atan_f) < 0 ||
        printf("D atan -0x1.8p-1 %a\n", atan_d) < 0 ||
        printf("L atan 0x1p+80 %La\n", atan_l) < 0 ||
        printf("F atan2 0x1p+0 -0x1p+0 %a\n", (double)atan2f(1.0f, -1.0f)) < 0 ||
        printf("D atan2 0x1p+0 -0x1p+0 %a\n", atan2_d) < 0 ||
        printf("L atan2 -0x1.8p+1 0x1p+2 %La\n", atan2_l) < 0 ||
        printf("F asin 0x1p-1 %a\n", (double)asin_f) < 0 ||
        printf("D asin -0x1p-2 %a\n", asin_d) < 0 ||
        printf("L asin 0x1.8p-1 %La\n", asin_l) < 0 ||
        printf("F acos 0x1p-1 %a\n", (double)acos_f) < 0 ||
        printf("D acos -0x1p-2 %a\n", acos_d) < 0 ||
        printf("L acos 0x1.8p-1 %La\n", acos_l) < 0 ||
        printf("F sin 0x1.234p+20 %a\n", (double)sin_f) < 0 ||
        printf("D sin 0x1.23456789abcdep+200 %a\n", sin_d) < 0 ||
        printf("L sin 0x1.23456789abcdefp+1000 %La\n", sin_l) < 0 ||
        printf("F cos -0x1.abc6p+18 %a\n", (double)cos_f) < 0 ||
        printf("D cos -0x1.fedcba9876543p+140 %a\n", cos_d) < 0 ||
        printf("L cos -0x1.fedcba987654321p+900 %La\n", cos_l) < 0 ||
        printf("F tan 0x1.2p-2 %a\n", (double)tan_f) < 0 ||
        printf("D tan 0x1.8p-2 %a\n", tan_d) < 0 ||
        printf("L tan 0x1.4p-2 %La\n", tan_l) < 0 ||
        printf("F pow 0x1.4p+0 0x1.8p+1 %a\n", (double)pow_f) < 0 ||
        printf("D pow 0x1.2p+0 -0x1.18p+4 %a\n", pow_d) < 0 ||
        printf("L pow 0x8.000000000000001p-3 0x8p+4 %La\n", pow_l) < 0 ||
        printf("F fma 0x1.000002p+0 0x1.fffffcp-1 -0x1p+0 %a\n",
               (double)fused_f) < 0 ||
        printf("D fma 0x1.0000002p+0 0x1.ffffffcp-1 -0x1p+0 %a\n",
               fused_d) < 0 ||
        printf("L fma 0x8.000000000000001p-3 0xf.ffffffffffffffep-4 -0x8p-3 %La\n",
               fused_l) < 0 ||
        printf("NEXT1 f=%d d=%d l=%d flags=%d/%d/%d\n",
               next_f == FLT_TRUE_MIN, next_d == DBL_TRUE_MIN,
               next_l == LDBL_TRUE_MIN, next_f_flags, next_d_flags,
               next_l_flags) < 0 ||
        printf("ILOGB1 f=%d d=%d l=%d flags=%d/%d/%d\n",
               ilogb_f_value == FP_ILOGB0,
               ilogb_d_value == INT_MAX,
               ilogb_l_value == FP_ILOGBNAN, ilogb_f_flags,
               ilogb_d_flags, ilogb_l_flags) < 0 ||
        puts("MATH4 complete records=66 fenv=syscall-context exceptions=verified") == EOF)
        return 55;
    return 0;
}
