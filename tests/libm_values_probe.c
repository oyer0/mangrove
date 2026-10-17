/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t random_state = UINT64_C(0x4d414e47524f5645);
static unsigned records;

static uint64_t next_u64(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static double double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static long double x87_from_parts(uint64_t significand, uint16_t sign_exponent)
{
    struct {
        uint64_t significand;
        uint16_t sign_exponent;
        unsigned char padding[6];
    } raw = {significand, sign_exponent, {0}};
    long double value;
    memcpy(&value, &raw, sizeof value);
    return value;
}

static long double random_x87(void)
{
    uint16_t exponent = (uint16_t)(1U + next_u64() % 0x7ffeU);
    if (next_u64() & 1U)
        exponent |= UINT16_C(0x8000);
    return x87_from_parts(UINT64_C(0x8000000000000000) |
                          (next_u64() & UINT64_C(0x7fffffffffffffff)),
                          exponent);
}

#define RECORD_D(name, input, output) \
    do { printf("D %s %a %a\n", #name, (input), (output)); ++records; } while (0)
#define RECORD_F(name, input, output) \
    do { printf("F %s %a %a\n", #name, (double)(input), (double)(output)); ++records; } while (0)
#define RECORD_L(name, input, output) \
    do { printf("L %s %La %La\n", #name, (input), (output)); ++records; } while (0)
#define RECORD_BINARY_D(name, x, y, output) \
    do { printf("D %s %a %a %a\n", #name, (x), (y), (output)); ++records; } while (0)
#define RECORD_BINARY_F(name, x, y, output) \
    do { printf("F %s %a %a %a\n", #name, (double)(x), (double)(y), \
                (double)(output)); ++records; } while (0)
#define RECORD_BINARY_L(name, x, y, output) \
    do { printf("L %s %La %La %La\n", #name, (x), (y), (output)); ++records; } while (0)
#define RECORD_QUO_D(name, x, y, output, quotient) \
    do { printf("D %s %a %a %a %d\n", #name, (x), (y), (output), \
                (quotient)); ++records; } while (0)
#define RECORD_QUO_F(name, x, y, output, quotient) \
    do { printf("F %s %a %a %a %d\n", #name, (double)(x), (double)(y), \
                (double)(output), (quotient)); ++records; } while (0)
#define RECORD_QUO_L(name, x, y, output, quotient) \
    do { printf("L %s %La %La %La %d\n", #name, (x), (y), (output), \
                (quotient)); ++records; } while (0)
#define RECORD_SCALE_D(name, x, n, output) \
    do { printf("D %s %a %d %a\n", #name, (x), (n), (output)); ++records; } while (0)
#define RECORD_SCALE_F(name, x, n, output) \
    do { printf("F %s %a %d %a\n", #name, (double)(x), (n), \
                (double)(output)); ++records; } while (0)
#define RECORD_SCALE_L(name, x, n, output) \
    do { printf("L %s %La %d %La\n", #name, (x), (n), (output)); ++records; } while (0)
#define RECORD_CUBERT_D(kind, name, x, output) \
    do { printf("%s %s %a %a\n", kind, #name, (x), (output)); ++records; } while (0)
#define RECORD_CUBERT_F(x, output) \
    do { printf("F cbrt %a %a\n", (double)(x), (double)(output)); ++records; } while (0)
#define RECORD_CUBERT_L(x, output) \
    do { printf("L cbrt %La %La\n", (x), (output)); ++records; } while (0)
#define RECORD_HYPOT_D(kind, name, x, y, output) \
    do { printf("%s %s %a %a %a\n", kind, #name, (x), (y), (output)); ++records; } while (0)
#define RECORD_HYPOT_F(x, y, output) \
    do { printf("F hypot %a %a %a\n", (double)(x), (double)(y), \
                (double)(output)); ++records; } while (0)
#define RECORD_HYPOT_L(x, y, output) \
    do { printf("L hypot %La %La %La\n", (x), (y), (output)); ++records; } while (0)
#define RECORD_CUBERT_ROUND_F(mode, output) \
    do { printf("F cbrt-round -0x1p+1 %d %a\n", (mode), \
                (double)(output)); ++records; } while (0)
#define RECORD_CUBERT_ROUND_D(mode, output) \
    do { printf("D cbrt-round -0x1p+1 %d %a\n", (mode), (output)); ++records; } while (0)
#define RECORD_CUBERT_ROUND_L(mode, output) \
    do { printf("L cbrt-round -0x1p+1 %d %La\n", (mode), (output)); ++records; } while (0)

static int edge_checks(void)
{
    volatile float fsub = FLT_TRUE_MIN;
    volatile double dsub = DBL_TRUE_MIN;
    volatile long double lsub = LDBL_TRUE_MIN;
    if (sizeof(long double) != 16 || FLT_RADIX != 2 || LDBL_MANT_DIG != 64 ||
        LDBL_MAX_EXP != 16384 || math_errhandling != MATH_ERREXCEPT ||
        fpclassify(0.0f) != FP_ZERO || fpclassify(fsub) != FP_SUBNORMAL ||
        fpclassify(dsub) != FP_SUBNORMAL || fpclassify(lsub) != FP_SUBNORMAL ||
        fpclassify(INFINITY) != FP_INFINITE || fpclassify(NAN) != FP_NAN ||
        !signbit(-0.0L) || !isfinite(0.0) || isfinite(INFINITY) ||
        !isunordered(NAN, 1.0) || isless(1.0, NAN) ||
        fmin(NAN, 4.0) != 4.0 || fmax(-0.0, 0.0) != 0.0 ||
        signbit(fmin(-0.0, 0.0)) == 0 || signbit(fmax(-0.0, 0.0)) != 0)
        return 1;

    if (trunc(-3.75) != -3.0 || floor(-3.25) != -4.0 || ceil(-3.25) != -3.0 ||
        round(0.5) != 1.0 || round(-0.5) != -1.0 || rint(2.5) != 2.0 ||
        roundf(0.5f) != 1.0f || roundl(-0.5L) != -1.0L ||
        nearbyint(3.5) != 4.0 || trunc(-0.25) != 0.0 || !signbit(trunc(-0.25)) ||
        floorl(-0x1p-100L) != -1.0L || ceill(0x1p-100) != 1.0)
        return 2;

    int exponent = 0;
    double integral = 0.0;
    long double lintegral = 0.0L;
    double nan_integral = 0.0;
    if (frexp(8.0, &exponent) != 0.5 || exponent != 4 ||
        frexp(DBL_TRUE_MIN, &exponent) != 0.5 || exponent != -1073 ||
        frexpl(LDBL_TRUE_MIN, &exponent) != 0.5L || exponent != -16444 ||
        ilogb(DBL_TRUE_MIN) != -1074 || ilogbl(LDBL_TRUE_MIN) != -16445 ||
        modf(-3.25, &integral) != -0.25 || integral != -3.0 ||
        modfl(0x1.0000000000000008p+0L, &lintegral) != 0x1p-61L ||
        lintegral != 1.0L || !isnan(modf(NAN, &nan_integral)) ||
        !isnan(nan_integral) || modf(INFINITY, &integral) != 0.0 ||
        !isinf(integral) || !isinf(logb(0.0)) || !signbit(logb(0.0)) ||
        !(fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO))
        return 3;

    if (feclearexcept(FE_ALL_EXCEPT) || ilogbf(0.0f) != FP_ILOGB0 ||
        !(fetestexcept(FE_INVALID) & FE_INVALID) ||
        feclearexcept(FE_ALL_EXCEPT) || ilogb(INFINITY) != INT_MAX ||
        !(fetestexcept(FE_INVALID) & FE_INVALID) ||
        feclearexcept(FE_ALL_EXCEPT) || ilogbl(NAN) != FP_ILOGBNAN ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 15;

    volatile float snan_f = float_from_bits(UINT32_C(0x7f800001));
    volatile double snan_d = double_from_bits(UINT64_C(0x7ff0000000000001));
    volatile long double snan_l = x87_from_parts(
        (UINT64_C(1) << 63) | 1U, UINT16_C(0x7fff));
    if (feclearexcept(FE_ALL_EXCEPT) ||
        isgreater(snan_f, 0.0f) || isgreaterequal(snan_f, 0.0f) ||
        isless(snan_f, 0.0f) || islessequal(snan_f, 0.0f) ||
        islessgreater(snan_f, 0.0f) || !isunordered(snan_f, 0.0f) ||
        isgreater(snan_d, 0.0) || islessgreater(snan_d, 0.0) ||
        !isunordered(snan_d, 0.0) || isgreater(snan_l, 0.0L) ||
        islessgreater(snan_l, 0.0L) || !isunordered(snan_l, 0.0L) ||
        fetestexcept(FE_INVALID) != 0)
        return 16;

    if (nextafter(0.0, 1.0) != DBL_TRUE_MIN ||
        nextafter(-0.0, -1.0) != -DBL_TRUE_MIN ||
        nextafterl(0.0L, 1.0L) != LDBL_TRUE_MIN ||
        nextafter(1.0, 2.0) != 0x1.0000000000001p+0 ||
        nextafter(0x1.fffffffffffffp-1, 1.0) != 1.0 ||
        nextafter(1.0, 0.0) != 0x1.fffffffffffffp-1 ||
        nextafterf(FLT_MAX, HUGE_VALF) != HUGE_VALF ||
        nextafter(HUGE_VAL, 0.0) != DBL_MAX ||
        nextafterl(nextafterl(1.0L, 0.0L), 1.0L) != 1.0L ||
        nextafterl(LDBL_MAX, HUGE_VALL) != HUGE_VALL ||
        nextafterl(HUGE_VALL, 0.0L) != LDBL_MAX ||
        nexttoward(1.0, 1.0L + 0x1p-60L) != 0x1.0000000000001p+0 ||
        nexttowardf(1.0f, 1.0L + 0x1p-60L) != 0x1.000002p+0f)
        return 4;

    volatile float next_f;
    volatile double next_d;
    volatile long double next_l;
    if (feclearexcept(FE_ALL_EXCEPT)) return 9;
    next_f = nextafterf(0.0f, 1.0f);
    if (next_f != FLT_TRUE_MIN ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT))) return 10;
    if (feclearexcept(FE_ALL_EXCEPT)) return 11;
    next_d = nextafter(0.0, 1.0);
    if (next_d != DBL_TRUE_MIN ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT))) return 12;
    if (feclearexcept(FE_ALL_EXCEPT)) return 13;
    next_l = nextafterl(0.0L, 1.0L);
    if (next_l != LDBL_TRUE_MIN ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT))) return 14;

    volatile double invalid_root = sqrt(-1.0);
    if (!isnan(invalid_root) || fetestexcept(FE_INVALID) == 0)
        return 5;
    (void)feclearexcept(FE_ALL_EXCEPT);
    if (sqrt(-0.0) != 0.0 || !signbit(sqrt(-0.0)) ||
        sqrt(DBL_TRUE_MIN) != 0x1p-537 ||
        !(sqrtl(LDBL_TRUE_MIN) > 0.0L) ||
        fpclassify(sqrtl(LDBL_TRUE_MIN)) != FP_NORMAL)
        return 6;

    fenv_t original;
    if (fegetenv(&original) || fesetround(FE_DOWNWARD) ||
        rint(0.5) != 0.0 || rintl(0.5L) != 0.0L ||
        fesetround(FE_UPWARD) || rint(0.5) != 1.0 ||
        nearbyint(0.5) != 1.0 || fegetround() != FE_UPWARD ||
        fesetenv(&original) || fegetround() != FE_TONEAREST)
        return 7;
    if (feclearexcept(FE_ALL_EXCEPT) || rint(0.25) != 0.0 ||
        !(fetestexcept(FE_INEXACT) & FE_INEXACT) ||
        feclearexcept(FE_ALL_EXCEPT) || nearbyint(0.25) != 0.0 ||
        fetestexcept(FE_INEXACT) != 0)
        return 8;

    int quotient = 99;
    if (scalbn(0.0, INT_MAX) != 0.0 ||
        scalbn(1.0, -1074) != DBL_TRUE_MIN ||
        ldexpf(FLT_TRUE_MIN, 1) != FLT_TRUE_MIN * 2.0f ||
        scalblnl(1.0L, 1000) != 0x1p1000L ||
        fmod(7.0, 3.0) != 1.0 || fmod(-7.0, 3.0) != -1.0 ||
        fmod(7.0, -3.0) != 1.0 ||
        remainder(7.0, 2.0) != -1.0 || remainder(5.0, 2.0) != 1.0 ||
        remainder(-3.0, 2.0) != 1.0 ||
        remquo(30.0, 4.0, &quotient) != -2.0 || quotient != 0 ||
        remquo(19.0, 4.0, &quotient) != -1.0 || quotient != 5 ||
        remquo(19.0, -4.0, &quotient) != -1.0 || quotient != -5 ||
        remquo(3.0, 2.0, &quotient) != -1.0 || quotient != 2 ||
        remquol(-29.0L, 4.0L, &quotient) != -1.0L || quotient != -7 ||
        scalbnl(1.0L, -16445) != LDBL_TRUE_MIN ||
        cbrtf(-0.0f) != -0.0f || !signbit(cbrtf(-0.0f)) ||
        cbrt(-8.0) != -2.0 || cbrtl(LDBL_TRUE_MIN) <= 0.0L ||
        cbrtl(LDBL_MAX) <= 0.0L ||
        hypot(3.0, 4.0) != 5.0 || hypotl(LDBL_MAX, LDBL_TRUE_MIN) != LDBL_MAX ||
        hypot(INFINITY, NAN) != INFINITY || signbit(hypot(-0.0, -0.0)) ||
        !signbit(fmodf(-0.0f, 3.0f)) || !signbit(fmod(-0.0, 3.0)) ||
        !signbit(fmodl(-0.0L, 3.0L)) ||
        !signbit(remainderf(-0.0f, 3.0f)) ||
        !signbit(remainder(-0.0, 3.0)) ||
        !signbit(remainderl(-0.0L, 3.0L)) ||
        fmod(2.0, INFINITY) != 2.0 || remainder(-2.0, INFINITY) != -2.0)
        return 17;
    if (feclearexcept(FE_ALL_EXCEPT) || !isnan(fmod(1.0, 0.0)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 18;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        scalbn(DBL_MAX, 1) != HUGE_VAL ||
        (fetestexcept(FE_OVERFLOW | FE_INEXACT) !=
         (FE_OVERFLOW | FE_INEXACT)))
        return 19;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        scalbn(DBL_TRUE_MIN, -1) != 0.0 ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT)))
        return 20;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        hypotl(LDBL_MAX, LDBL_TRUE_MIN) != LDBL_MAX ||
        (fetestexcept(FE_INEXACT) == 0) ||
        (fetestexcept(FE_UNDERFLOW) != 0))
        return 22;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        hypotf(0x1p-130f, FLT_TRUE_MIN) != 0x1p-130f ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT)))
        return 27;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        hypot(0x1p-1040, DBL_TRUE_MIN) != 0x1p-1040 ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT)))
        return 28;
    fenv_t hypot_environment;
    if (fegetenv(&hypot_environment) || fesetround(FE_UPWARD) ||
        hypotl(1.0L, LDBL_TRUE_MIN) != nextafterl(1.0L, HUGE_VALL) ||
        fesetround(FE_DOWNWARD) || hypotl(1.0L, LDBL_TRUE_MIN) != 1.0L ||
        fesetenv(&hypot_environment))
        return 23;
    fenv_t saved;
    if (fegetenv(&saved) || fesetround(FE_UPWARD) ||
        scalbn(DBL_TRUE_MIN, -1) != DBL_TRUE_MIN || fesetenv(&saved) ||
        fegetround() != FE_TONEAREST)
        return 21;

    return 0;
}

int main(void)
{
    int check = edge_checks();
    printf("CHECK %d\n", check);
    if (check)
        return check;

    for (unsigned i = 0; i < 512; ++i) {
        uint64_t raw = next_u64();
        if (((raw >> 52) & 0x7ffU) == 0x7ffU)
            raw &= ~UINT64_C(0x0010000000000000);
        double d = double_from_bits(raw);
        uint32_t fraw = (uint32_t)next_u64();
        if (((fraw >> 23) & 0xffU) == 0xffU)
            fraw &= ~UINT32_C(0x00800000);
        float f = float_from_bits(fraw);
        long double l;
        if (i == 0) {
            l = LDBL_MAX;
        } else if (i == 1) {
            l = LDBL_TRUE_MIN;
        } else if ((i & 3U) == 0) {
            uint64_t significand = next_u64();
            unsigned exponent = (unsigned)(next_u64() % 0x7fffU);
            uint16_t sign = (uint16_t)(next_u64() & 0x8000U);
            if (exponent) {
                significand |= UINT64_C(1) << 63;
            } else {
                significand &= ~(UINT64_C(1) << 63);
                if (!significand) significand = 1;
            }
            l = x87_from_parts(significand, (uint16_t)(sign | exponent));
        } else {
            l = ((long double)(next_u64() >> 1) / 0x1p62L - 1.0L) * 0x1p32L;
            l += (long double)(next_u64() & 0xffffU) * 0x1p-60L;
            if (i & 1U) l = -l;
        }

        RECORD_D(trunc, d, trunc(d));
        RECORD_D(floor, d, floor(d));
        RECORD_D(ceil, d, ceil(d));
        RECORD_D(round, d, round(d));
        RECORD_D(sqrt, fabs(d), sqrt(fabs(d)));
        RECORD_F(trunc, f, truncf(f));
        RECORD_F(floor, f, floorf(f));
        RECORD_F(ceil, f, ceilf(f));
        RECORD_F(round, f, roundf(f));
        RECORD_F(sqrt, fabsf(f), sqrtf(fabsf(f)));
        RECORD_L(trunc, l, truncl(l));
        RECORD_L(floor, l, floorl(l));
        RECORD_L(ceil, l, ceill(l));
        RECORD_L(round, l, roundl(l));
        RECORD_L(sqrt, fabsl(l), sqrtl(fabsl(l)));

        double dx = double_from_bits(next_u64() & UINT64_C(0xffefffffffffffff));
        double dy = double_from_bits(next_u64() & UINT64_C(0xffefffffffffffff));
        float fx = float_from_bits((uint32_t)next_u64() & UINT32_C(0xff7fffff));
        float fy = float_from_bits((uint32_t)next_u64() & UINT32_C(0xff7fffff));
        long double lx = random_x87();
        long double ly = random_x87();
        if (dy == 0.0) dy = 1.0;
        if (fy == 0.0f) fy = 1.0f;
        int qi;
        RECORD_BINARY_D(fmod, dx, dy, fmod(dx, dy));
        RECORD_BINARY_D(remainder, dx, dy, remainder(dx, dy));
        double rem_d = remquo(dx, dy, &qi);
        RECORD_QUO_D(remquo, dx, dy, rem_d, qi);
        RECORD_BINARY_F(fmod, fx, fy, fmodf(fx, fy));
        RECORD_BINARY_F(remainder, fx, fy, remainderf(fx, fy));
        float rem_f = remquof(fx, fy, &qi);
        RECORD_QUO_F(remquo, fx, fy, rem_f, qi);
        RECORD_BINARY_L(fmod, lx, ly, fmodl(lx, ly));
        RECORD_BINARY_L(remainder, lx, ly, remainderl(lx, ly));
        long double rem_l = remquol(lx, ly, &qi);
        RECORD_QUO_L(remquo, lx, ly, rem_l, qi);

        int scale_n = (int)(next_u64() % 201U) - 100;
        double scale_d = 1.0 + (double)(next_u64() & UINT64_C(0x000fffffffffffff)) * 0x1p-52;
        float scale_f = 1.0f + (float)(next_u64() & UINT32_C(0x007fffff)) * 0x1p-23f;
        long double scale_l = x87_from_parts(
            UINT64_C(0x8000000000000000) |
                (next_u64() & UINT64_C(0x7fffffffffffffff)),
            (uint16_t)(0x3fffU | ((next_u64() & 1U) << 15)));
        RECORD_SCALE_D(scalbn, scale_d, scale_n, scalbn(scale_d, scale_n));
        RECORD_SCALE_F(scalbn, scale_f, scale_n, scalbnf(scale_f, scale_n));
        RECORD_SCALE_L(scalbn, scale_l, scale_n, scalbnl(scale_l, scale_n));
        RECORD_SCALE_D(ldexp, scale_d, scale_n, ldexp(scale_d, scale_n));
        RECORD_SCALE_F(ldexp, scale_f, scale_n, ldexpf(scale_f, scale_n));
        RECORD_SCALE_L(ldexp, scale_l, scale_n, ldexpl(scale_l, scale_n));
        RECORD_SCALE_D(scalbln, scale_d, scale_n, scalbln(scale_d, scale_n));
        RECORD_SCALE_F(scalbln, scale_f, scale_n, scalblnf(scale_f, scale_n));
        RECORD_SCALE_L(scalbln, scale_l, scale_n, scalblnl(scale_l, scale_n));
        RECORD_CUBERT_F(fx, cbrtf(fx));
        RECORD_CUBERT_D("D", cbrt, dx, cbrt(dx));
        RECORD_CUBERT_L(lx, cbrtl(lx));
        RECORD_HYPOT_F(fx, fy, hypotf(fx, fy));
        RECORD_HYPOT_D("D", hypot, dx, dy, hypot(dx, dy));
        RECORD_HYPOT_L(lx, ly, hypotl(lx, ly));
    }

    fenv_t rounding_environment;
    const int rounding_modes[] = {
        FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD
    };
    if (fegetenv(&rounding_environment)) return 24;
    for (unsigned i = 0; i < sizeof rounding_modes / sizeof rounding_modes[0]; ++i) {
        if (fesetround(rounding_modes[i])) return 25;
        RECORD_CUBERT_ROUND_F(rounding_modes[i], cbrtf(-2.0f));
        RECORD_CUBERT_ROUND_D(rounding_modes[i], cbrt(-2.0));
        RECORD_CUBERT_ROUND_L(rounding_modes[i], cbrtl(-2.0L));
    }
    if (fesetenv(&rounding_environment)) return 26;

    printf("COMPLETE records=%u seed=0x4d414e47524f5645\n", records);
    return 0;
}
