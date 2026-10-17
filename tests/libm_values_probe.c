/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t random_state = UINT64_C(0x4d414e47524f5645);
static uint64_t pow_random_state = UINT64_C(0x706f772d4d414e47);
static uint64_t error_random_state = UINT64_C(0x6572662d65726366);
static unsigned records;

static uint64_t next_u64(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static uint64_t next_pow_u64(void)
{
    pow_random_state ^= pow_random_state << 13;
    pow_random_state ^= pow_random_state >> 7;
    pow_random_state ^= pow_random_state << 17;
    return pow_random_state;
}

static uint64_t next_error_u64(void)
{
    error_random_state ^= error_random_state << 13;
    error_random_state ^= error_random_state >> 7;
    error_random_state ^= error_random_state << 17;
    return error_random_state;
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

static float random_range_f(unsigned bound)
{
    int whole = (int)(next_u64() % (2U * bound + 1U)) - (int)bound;
    float fraction = (float)(next_u64() & UINT32_C(0x007fffff)) * 0x1p-24f;
    return (float)whole + fraction;
}

static double random_range_d(unsigned bound)
{
    int whole = (int)(next_u64() % (2U * bound + 1U)) - (int)bound;
    double fraction = (double)(next_u64() & UINT64_C(0x000fffffffffffff)) * 0x1p-53;
    return (double)whole + fraction;
}

static long double random_range_l(unsigned bound)
{
    int whole = (int)(next_u64() % (2U * bound + 1U)) - (int)bound;
    long double fraction = (long double)(next_u64() >> 1) * 0x1p-64L;
    return (long double)whole + fraction;
}

static float random_error_f(void)
{
    int whole = (int)(next_error_u64() % 17U) - 8;
    float fraction = (float)(next_error_u64() & UINT32_C(0x007fffff)) * 0x1p-24f;
    return (float)whole + fraction;
}

static double random_error_d(void)
{
    int whole = (int)(next_error_u64() % 17U) - 8;
    double fraction = (double)(next_error_u64() & UINT64_C(0x000fffffffffffff)) * 0x1p-53;
    return (double)whole + fraction;
}

static long double random_error_l(void)
{
    int whole = (int)(next_error_u64() % 17U) - 8;
    long double fraction = (long double)(next_error_u64() >> 1) * 0x1p-64L;
    return (long double)whole + fraction;
}

static float random_log1p_f(void)
{
    float value = (float)(next_u64() & UINT32_C(0x007fffff)) * 0x1p-23f;
    return next_u64() & 1U ? -value : value;
}

static double random_log1p_d(void)
{
    double value = (double)(next_u64() & UINT64_C(0x000fffffffffffff)) * 0x1p-52;
    return next_u64() & 1U ? -value : value;
}

static long double random_log1p_l(void)
{
    long double value = (long double)(next_u64() >> 1) * 0x1p-63L;
    return next_u64() & 1U ? -value : value;
}

static float random_pow_base_f(void)
{
    long double unit = (long double)(next_pow_u64() >> 1) * 0x1p-63L;
    return (float)(0.5L + unit * 1.5L);
}

static double random_pow_base_d(void)
{
    long double unit = (long double)(next_pow_u64() >> 1) * 0x1p-63L;
    return (double)(0.5L + unit * 1.5L);
}

static long double random_pow_base_l(void)
{
    long double unit = (long double)(next_pow_u64() >> 1) * 0x1p-63L;
    return 0.5L + unit * 1.5L;
}

static long double random_pow_exponent(void)
{
    int whole = (int)(next_pow_u64() % 65U) - 32;
    if (next_pow_u64() & 1U)
        return (long double)whole;
    long double fraction = (long double)(next_pow_u64() >> 1) * 0x1p-63L;
    return (long double)whole + (fraction - 0.5L) * 0.5L;
}

static float random_hyperbolic_f(void)
{
    float value = (float)(next_u64() & UINT32_C(0x007fffff)) * 0x1p-23f;
    return (next_u64() & 1U) ? value : -value;
}

static double random_hyperbolic_d(void)
{
    double value = (double)(next_u64() & UINT64_C(0x000fffffffffffff)) * 0x1p-52;
    return (next_u64() & 1U) ? value : -value;
}

static long double random_hyperbolic_l(void)
{
    long double value = (long double)(next_u64() >> 1) * 0x1p-63L;
    return (next_u64() & 1U) ? value : -value;
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
#define RECORD_UNARY_F(name, input, output) \
    do { printf("F %s %a %a\n", #name, (double)(input), (double)(output)); ++records; } while (0)
#define RECORD_UNARY_D(name, input, output) \
    do { printf("D %s %a %a\n", #name, (input), (output)); ++records; } while (0)
#define RECORD_UNARY_L(name, input, output) \
    do { printf("L %s %La %La\n", #name, (input), (output)); ++records; } while (0)
#define RECORD_DIRECTED_F(name, input, mode, output) \
    do { printf("F %s-round %a %d %a\n", #name, (double)(input), (mode), \
                (double)(output)); ++records; } while (0)
#define RECORD_DIRECTED_D(name, input, mode, output) \
    do { printf("D %s-round %a %d %a\n", #name, (input), (mode), \
                (output)); ++records; } while (0)
#define RECORD_DIRECTED_L(name, input, mode, output) \
    do { printf("L %s-round %La %d %La\n", #name, (input), (mode), \
                (output)); ++records; } while (0)
#define RECORD_POWER_ROUND_F(input, exponent, mode, output) \
    do { printf("F pow-round %a %a %d %a\n", (double)(input), \
                (double)(exponent), (mode), (double)(output)); ++records; } while (0)
#define RECORD_POWER_ROUND_D(input, exponent, mode, output) \
    do { printf("D pow-round %a %a %d %a\n", (input), (exponent), \
                (mode), (output)); ++records; } while (0)
#define RECORD_POWER_ROUND_L(input, exponent, mode, output) \
    do { printf("L pow-round %La %La %d %La\n", (input), (exponent), \
                (mode), (output)); ++records; } while (0)
#define RECORD_POWER_NEGATIVE_ROUND_F(input, exponent, mode, output) \
    do { printf("F pow-negative-round %a %a %d %a\n", (double)(input), \
                (double)(exponent), (mode), (double)(output)); ++records; } while (0)
#define RECORD_POWER_NEGATIVE_ROUND_D(input, exponent, mode, output) \
    do { printf("D pow-negative-round %a %a %d %a\n", (input), (exponent), \
                (mode), (output)); ++records; } while (0)
#define RECORD_POWER_NEGATIVE_ROUND_L(input, exponent, mode, output) \
    do { printf("L pow-negative-round %La %La %d %La\n", (input), (exponent), \
                (mode), (output)); ++records; } while (0)
#define RECORD_POWER_LARGE_ROUND_D(input, exponent, mode, output) \
    do { printf("D pow-negative-large-round %a %a %d %a\n", (input), \
                (exponent), (mode), (output)); ++records; } while (0)
#define RECORD_POWER_LARGE_ROUND_L(input, exponent, mode, output) \
    do { printf("L pow-negative-large-round %La %La %d %La\n", (input), \
                (exponent), (mode), (output)); ++records; } while (0)

static int edge_checks(void)
{
    if (!isnan(nanf("float-tag")) || !isnan(nan("double-tag")) ||
        !isnan(nanl("extended-tag")) || signbit(nan("")) ||
        signbit(nanl("")))
        return 29;
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
        floorl(-0x1p-100L) != -1.0L || ceill(0x1p-100) != 1.0 ||
        floorl(-0xf.f8c12p+0L) != -16.0L ||
        ceill(0xf.f8c12p+0L) != 16.0L ||
        roundl(-0xf.8p+0L) != -16.0L)
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
    if (sinh(-0.0) != 0.0 || !signbit(sinh(-0.0)) ||
        cosh(-INFINITY) != INFINITY || tanh(INFINITY) != 1.0 ||
        asinh(-INFINITY) != -INFINITY || acosh(1.0) != 0.0 ||
        atanh(-0.0) != 0.0 || !signbit(atanh(-0.0)) ||
        !isnan(acosh(0.5)) || !isnan(acoshl(-HUGE_VALL)) ||
        !isnan(atanh(1.5)))
        return 17;
    if (feclearexcept(FE_ALL_EXCEPT) || atanh(1.0) != INFINITY ||
        !(fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO) ||
        feclearexcept(FE_ALL_EXCEPT) || acosh(0.5) == acosh(0.5) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 18;
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
    if (pow(1.0, NAN) != 1.0 || pow(NAN, 0.0) != 1.0 ||
        pow(-1.0, INFINITY) != 1.0 || pow(-2.0, 3.0) != -8.0 ||
        pow(-2.0, 4.0) != 16.0 || pow(-2.0, 0.5) == pow(-2.0, 0.5) ||
        pow(-0.0, 3.0) != 0.0 || !signbit(pow(-0.0, 3.0)) ||
        pow(-0.0, 2.0) != 0.0 || signbit(pow(-0.0, 2.0)) ||
        feclearexcept(FE_ALL_EXCEPT) || pow(0.0, -1.0) != INFINITY ||
        !(fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO) ||
        feclearexcept(FE_ALL_EXCEPT) || pow(-2.0, 0.5) == pow(-2.0, 0.5) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID) ||
        powl(0x1.0000000000000008p+0L, 128.0L) <= 1.0L ||
        powf(1.25f, -3.0f) != 0.512f ||
        feclearexcept(FE_ALL_EXCEPT) || !isinf(pow(0.5, -20000.0)) ||
        !(fetestexcept(FE_OVERFLOW | FE_INEXACT) ==
          (FE_OVERFLOW | FE_INEXACT)) ||
        feclearexcept(FE_ALL_EXCEPT) || pow(2.0, -20000.0) != 0.0 ||
        !(fetestexcept(FE_UNDERFLOW | FE_INEXACT) ==
          (FE_UNDERFLOW | FE_INEXACT)))
        return 47;
    if (feclearexcept(FE_ALL_EXCEPT) || powf(2.0f, 3.0f) != 8.0f ||
        pow(2.0, 3.0) != 8.0 || powl(2.0L, 3.0L) != 8.0L ||
        fetestexcept(FE_INEXACT) != 0)
        return 48;
    const int power_round_modes[] = {
        FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO
    };
    fenv_t power_environment;
    volatile long double negative_underflow, negative_overflow;
    if (fegetenv(&power_environment))
        return 49;
    for (unsigned i = 0; i < 4; ++i) {
        if (fesetround(power_round_modes[i]) ||
            feclearexcept(FE_ALL_EXCEPT))
            return 50;
        negative_underflow = powl(-2.0L, -20001.0L);
        long double expected_underflow =
            power_round_modes[i] == FE_DOWNWARD ? -LDBL_TRUE_MIN : -0.0L;
        if (negative_underflow != expected_underflow ||
            !signbit(negative_underflow) ||
            fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) ||
            feclearexcept(FE_ALL_EXCEPT))
            return 50;
        negative_overflow = powl(-2.0L, 20001.0L);
        int finite_overflow = power_round_modes[i] == FE_UPWARD ||
                              power_round_modes[i] == FE_TOWARDZERO;
        if ((finite_overflow ? negative_overflow != -LDBL_MAX :
             !isinf(negative_overflow) || !signbit(negative_overflow)) ||
            fetestexcept(FE_OVERFLOW | FE_INEXACT) !=
            (FE_OVERFLOW | FE_INEXACT))
            return 50;
    }
    if (fesetround(FE_TOWARDZERO) || feclearexcept(FE_ALL_EXCEPT))
        return 51;
    negative_underflow = powl(-2.0L, -4294967297.0L);
    if (negative_underflow != 0.0L || !signbit(negative_underflow) ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
        (FE_UNDERFLOW | FE_INEXACT) || fesetround(FE_DOWNWARD) ||
        feclearexcept(FE_ALL_EXCEPT))
        return 51;
    negative_underflow = powl(-2.0L, -4294967297.0L);
    if (negative_underflow != -LDBL_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
        (FE_UNDERFLOW | FE_INEXACT) || fesetenv(&power_environment))
        return 52;
    volatile long double near_overflow_base = expl(5700.0L);
    if (feclearexcept(FE_ALL_EXCEPT) ||
        !isinf(powl(near_overflow_base, 2.0L)) ||
        fetestexcept(FE_OVERFLOW | FE_INEXACT) !=
        (FE_OVERFLOW | FE_INEXACT))
        return 53;
    volatile long double near_underflow_base = expl(-5700.0L);
    if (feclearexcept(FE_ALL_EXCEPT) ||
        powl(near_underflow_base, 2.0L) != 0.0L ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
        (FE_UNDERFLOW | FE_INEXACT))
        return 54;
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
    if (feclearexcept(FE_ALL_EXCEPT) ||
        exp2l(-16445.0L) != LDBL_TRUE_MIN || fetestexcept(FE_UNDERFLOW))
        return 29;
    if (feclearexcept(FE_ALL_EXCEPT) || exp2l(-16446.0L) != 0.0L ||
        (fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
         (FE_UNDERFLOW | FE_INEXACT)))
        return 30;
    if (feclearexcept(FE_ALL_EXCEPT) || !isinf(exp2l(16384.0L)) ||
        (fetestexcept(FE_OVERFLOW | FE_INEXACT) !=
         (FE_OVERFLOW | FE_INEXACT)))
        return 31;
    if (expm1l(-0.0L) != 0.0L || !signbit(expm1l(-0.0L)) ||
        expm1l(-INFINITY) != -1.0L || expm1l(INFINITY) != INFINITY ||
        log1pl(-0.0L) != 0.0L || !signbit(log1pl(-0.0L)))
        return 32;
    if (feclearexcept(FE_ALL_EXCEPT) || !isinf(logl(0.0L)) ||
        !(fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO))
        return 33;
    if (feclearexcept(FE_ALL_EXCEPT) || !isnan(logl(-1.0L)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 34;
    if (feclearexcept(FE_ALL_EXCEPT) || !isnan(log1pl(-2.0L)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 35;

    if (atanl(-0.0L) != 0.0L || !signbit(atanl(-0.0L)) ||
        atan(INFINITY) != acos(-1.0) * 0.5 ||
        asinf(-0.0f) != 0.0f || !signbit(asinf(-0.0f)) ||
        asin(1.0) != atan2(1.0, 0.0) ||
        acosl(-1.0L) != -atan2l(-0.0L, -1.0L) ||
        atan2(0.0, -1.0) != acos(-1.0) ||
        atan2(-0.0, -1.0) != -acos(-1.0) ||
        !signbit(atan2(-0.0, 1.0)) ||
        atan2(INFINITY, INFINITY) != atan2(1.0, 1.0) ||
        atan2(-INFINITY, -INFINITY) != atan2(-1.0, -1.0))
        return 36;
    if (feclearexcept(FE_ALL_EXCEPT) || !isnan(asin(1.0001)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID) ||
        feclearexcept(FE_ALL_EXCEPT) || !isnan(acos(-1.0001L)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 37;

    fenv_t integer_round_environment;
    if (lround(0.5) != 1 || lround(-0.5) != -1 ||
        llroundl(-2.5L) != -3 || lrint(2.5) != 2 ||
        llrint(-1.5) != -2 || llround(-0x1p63) != LLONG_MIN ||
        llround(0x1.fffffffffffffp+62) != LLONG_MAX - 1023 ||
        fegetenv(&integer_round_environment) || fesetround(FE_DOWNWARD) ||
        lrint(2.5) != 2 || lrint(-2.5) != -3 || lround(2.5) != 3 ||
        fesetround(FE_UPWARD) || lrint(2.5) != 3 || lrint(-2.5) != -2 ||
        fesetenv(&integer_round_environment))
        return 38;
    if (feclearexcept(FE_ALL_EXCEPT) || llround(0x1p63) != 0 ||
        !(fetestexcept(FE_INVALID) & FE_INVALID) ||
        feclearexcept(FE_ALL_EXCEPT) || llrint(NAN) != 0 ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 39;
    if (feclearexcept(FE_ALL_EXCEPT) || llrint(2.25) != 2 ||
        !(fetestexcept(FE_INEXACT) & FE_INEXACT) ||
        feclearexcept(FE_ALL_EXCEPT) || llround(2.25) != 2 ||
        fetestexcept(FE_INEXACT) != 0)
        return 40;
    if (sinl(-0.0L) != 0.0L || !signbit(sinl(-0.0L)) ||
        cosf(-0.0f) != 1.0f || tan(-0.0) != 0.0 || !signbit(tan(-0.0)) ||
        sin(INFINITY) == sin(INFINITY) || cos(NAN) == cos(NAN) ||
        tanl(NAN) == tanl(NAN))
        return 41;
    fenv_t trig_environment;
    if (fegetenv(&trig_environment) || fesetround(FE_DOWNWARD) ||
        sinl(0.75L) == 0.0L || fegetround() != FE_DOWNWARD ||
        fesetenv(&trig_environment))
        return 42;
    if (feclearexcept(FE_ALL_EXCEPT) || sin(INFINITY) == sin(INFINITY) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 43;
    volatile long double tiny_sine;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        (tiny_sine = sinl(LDBL_TRUE_MIN)) != LDBL_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT))
        return 44;
    if (feclearexcept(FE_ALL_EXCEPT) || cosl(LDBL_TRUE_MIN) != 1.0L ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) != FE_INEXACT)
        return 45;
    if (feclearexcept(FE_ALL_EXCEPT) || sinf(FLT_TRUE_MIN) != FLT_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT))
        return 46;
    if (erf(-0.0) != 0.0 || !signbit(erf(-0.0)) ||
        erfl(-0.0L) != 0.0L || !signbit(erfl(-0.0L)) ||
        erf(INFINITY) != 1.0 || erf(-INFINITY) != -1.0 ||
        erfc(INFINITY) != 0.0 || erfc(-INFINITY) != 2.0 ||
        erf(0.0) != 0.0 || erfc(0.0) != 1.0 ||
        !isnan(erf(NAN)) || !isnan(erfc(NAN)) ||
        !isnan(erff(NAN)) || !isnan(erfcl(NAN)))
        return 55;
    if (feclearexcept(FE_ALL_EXCEPT) || erfc(200.0L) != 0.0L ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) ||
        feclearexcept(FE_ALL_EXCEPT) || erfcl(-200.0L) != 2.0L ||
        fetestexcept(FE_UNDERFLOW) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) || erfcl(-128.0L) != 2.0L ||
        fetestexcept(FE_UNDERFLOW) != 0 ||
        (fetestexcept(FE_INEXACT) == 0) ||
        feclearexcept(FE_ALL_EXCEPT) || erfcl(128.0L) != 0.0L ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT))
        return 56;
    if (feclearexcept(FE_ALL_EXCEPT) || erf(20.0) != 1.0 ||
        fetestexcept(FE_INEXACT) == 0 ||
        feclearexcept(FE_ALL_EXCEPT) || erf(-20.0) != -1.0 ||
        fetestexcept(FE_INEXACT) == 0)
        return 57;

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
        if (!isfinite(f)) f = 0.75f;
        if (!isfinite(d)) d = -0.75;
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

        float error_f = random_error_f();
        double error_d = random_error_d();
        long double error_l = random_error_l();
        RECORD_UNARY_F(erf, error_f, erff(error_f));
        RECORD_UNARY_D(erf, error_d, erf(error_d));
        RECORD_UNARY_L(erf, error_l, erfl(error_l));
        RECORD_UNARY_F(erfc, error_f, erfcf(error_f));
        RECORD_UNARY_D(erfc, error_d, erfc(error_d));
        RECORD_UNARY_L(erfc, error_l, erfcl(error_l));

        float pbf = random_pow_base_f();
        double pbd = random_pow_base_d();
        long double pbl = random_pow_base_l();
        long double py = random_pow_exponent();
        float pyf = (float)py;
        double pyd = (double)py;
        long double pyl = py;
        RECORD_BINARY_F(pow, pbf, pyf, powf(pbf, pyf));
        RECORD_BINARY_D(pow, pbd, pyd, pow(pbd, pyd));
        RECORD_BINARY_L(pow, pbl, pyl, powl(pbl, pyl));

        float ef = random_range_f(80), e2f = random_range_f(100);
        double ed = random_range_d(700), e2d = random_range_d(900);
        long double el = random_range_l(11000), e2l = random_range_l(16000);
        float lf = fabsf(f);
        double ld = fabs(d);
        long double ll = fabsl(l);
        if (lf == 0.0f) lf = 1.0f;
        if (ld == 0.0) ld = 1.0;
        if (ll == 0.0L) ll = 1.0L;
        float l1f = random_log1p_f();
        double l1d = random_log1p_d();
        long double l1l = random_log1p_l();
        RECORD_UNARY_F(exp, ef, expf(ef));
        RECORD_UNARY_D(exp, ed, exp(ed));
        RECORD_UNARY_L(exp, el, expl(el));
        RECORD_UNARY_F(exp2, e2f, exp2f(e2f));
        RECORD_UNARY_D(exp2, e2d, exp2(e2d));
        RECORD_UNARY_L(exp2, e2l, exp2l(e2l));
        RECORD_UNARY_F(expm1, ef, expm1f(ef));
        RECORD_UNARY_D(expm1, ed, expm1(ed));
        RECORD_UNARY_L(expm1, el, expm1l(el));
        RECORD_UNARY_F(log, lf, logf(lf));
        RECORD_UNARY_D(log, ld, log(ld));
        RECORD_UNARY_L(log, ll, logl(ll));
        RECORD_UNARY_F(log2, lf, log2f(lf));
        RECORD_UNARY_D(log2, ld, log2(ld));
        RECORD_UNARY_L(log2, ll, log2l(ll));
        RECORD_UNARY_F(log10, lf, log10f(lf));
        RECORD_UNARY_D(log10, ld, log10(ld));
        RECORD_UNARY_L(log10, ll, log10l(ll));
        RECORD_UNARY_F(log1p, l1f, log1pf(l1f));
        RECORD_UNARY_D(log1p, l1d, log1p(l1d));
        RECORD_UNARY_L(log1p, l1l, log1pl(l1l));

        float hf = random_hyperbolic_f() * 12.0f;
        double hd = random_hyperbolic_d() * 350.0;
        long double hl = random_hyperbolic_l() * 7000.0L;
        float af = random_hyperbolic_f() * 0.999f;
        double ad = random_hyperbolic_d() * 0.999999;
        long double al = random_hyperbolic_l() * 0.999999999999L;
        float cf = 1.0f + fabsf(random_hyperbolic_f()) * 100.0f;
        double cd = 1.0 + fabs(random_hyperbolic_d()) * 1000.0;
        long double cl = 1.0L + fabsl(random_hyperbolic_l()) * 10000.0L;
        RECORD_UNARY_F(sinh, hf, sinhf(hf));
        RECORD_UNARY_D(sinh, hd, sinh(hd));
        RECORD_UNARY_L(sinh, hl, sinhl(hl));
        RECORD_UNARY_F(cosh, hf, coshf(hf));
        RECORD_UNARY_D(cosh, hd, cosh(hd));
        RECORD_UNARY_L(cosh, hl, coshl(hl));
        RECORD_UNARY_F(tanh, hf, tanhf(hf));
        RECORD_UNARY_D(tanh, hd, tanh(hd));
        RECORD_UNARY_L(tanh, hl, tanhl(hl));
        RECORD_UNARY_F(asinh, hf, asinhf(hf));
        RECORD_UNARY_D(asinh, hd, asinh(hd));
        RECORD_UNARY_L(asinh, hl, asinhl(hl));
        RECORD_UNARY_F(acosh, cf, acoshf(cf));
        RECORD_UNARY_D(acosh, cd, acosh(cd));
        RECORD_UNARY_L(acosh, cl, acoshl(cl));
        RECORD_UNARY_F(atanh, af, atanhf(af));
        RECORD_UNARY_D(atanh, ad, atanh(ad));
        RECORD_UNARY_L(atanh, al, atanhl(al));

        float wf = fabsf(float_from_bits((uint32_t)next_u64() &
                                         UINT32_C(0x7f7fffff)));
        double wd = fabs(double_from_bits(next_u64() &
                                          UINT64_C(0x7fefffffffffffff)));
        long double wl = fabsl(random_x87());
        RECORD_UNARY_F(asinh, wf, asinhf(wf));
        RECORD_UNARY_D(asinh, wd, asinh(wd));
        RECORD_UNARY_L(asinh, wl, asinhl(wl));
        RECORD_UNARY_F(tanh, wf, tanhf(wf));
        RECORD_UNARY_D(tanh, wd, tanh(wd));
        RECORD_UNARY_L(tanh, wl, tanhl(wl));
        RECORD_UNARY_F(acosh, wf + 1.0f, acoshf(wf + 1.0f));
        RECORD_UNARY_D(acosh, wd + 1.0, acosh(wd + 1.0));
        RECORD_UNARY_L(acosh, wl + 1.0L, acoshl(wl + 1.0L));

        float tf = random_hyperbolic_f() * 0x1p+50f;
        double td = random_hyperbolic_d() * 0x1p+500;
        long double tl = random_hyperbolic_l() * 0x1p+5000L;
        float sf = random_hyperbolic_f();
        double sd = random_hyperbolic_d();
        long double sl = random_hyperbolic_l();
        float y_f = random_hyperbolic_f() * 8.0f;
        float x_f = random_hyperbolic_f() * 8.0f;
        double y_d = random_hyperbolic_d() * 8.0;
        double x_d = random_hyperbolic_d() * 8.0;
        long double y_l = random_hyperbolic_l() * 8.0L;
        long double x_l = random_hyperbolic_l() * 8.0L;
        RECORD_UNARY_F(atan, tf, atanf(tf));
        RECORD_UNARY_D(atan, td, atan(td));
        RECORD_UNARY_L(atan, tl, atanl(tl));
        RECORD_UNARY_F(asin, sf, asinf(sf));
        RECORD_UNARY_D(asin, sd, asin(sd));
        RECORD_UNARY_L(asin, sl, asinl(sl));
        RECORD_UNARY_F(acos, sf, acosf(sf));
        RECORD_UNARY_D(acos, sd, acos(sd));
        RECORD_UNARY_L(acos, sl, acosl(sl));
        RECORD_BINARY_F(atan2, y_f, x_f, atan2f(y_f, x_f));
        RECORD_BINARY_D(atan2, y_d, x_d, atan2(y_d, x_d));
        RECORD_BINARY_L(atan2, y_l, x_l, atan2l(y_l, x_l));
        RECORD_UNARY_F(sin, f, sinf(f));
        RECORD_UNARY_D(sin, d, sin(d));
        RECORD_UNARY_L(sin, l, sinl(l));
        RECORD_UNARY_F(cos, f, cosf(f));
        RECORD_UNARY_D(cos, d, cos(d));
        RECORD_UNARY_L(cos, l, cosl(l));
        RECORD_UNARY_F(tan, f, tanf(f));
        RECORD_UNARY_D(tan, d, tan(d));
        RECORD_UNARY_L(tan, l, tanl(l));

    }

    /* Explicitly hit the least subnormal and the ill-conditioned log1p
     * boundary in all three result formats. */
    RECORD_UNARY_F(exp2, -149.0f, exp2f(-149.0f));
    RECORD_UNARY_D(exp2, -1074.0, exp2(-1074.0));
    RECORD_UNARY_L(exp2, -16445.0L, exp2l(-16445.0L));
    RECORD_UNARY_F(log, FLT_TRUE_MIN, logf(FLT_TRUE_MIN));
    RECORD_UNARY_D(log, DBL_TRUE_MIN, log(DBL_TRUE_MIN));
    RECORD_UNARY_L(log, LDBL_TRUE_MIN, logl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(expm1, FLT_TRUE_MIN, expm1f(FLT_TRUE_MIN));
    RECORD_UNARY_D(expm1, DBL_TRUE_MIN, expm1(DBL_TRUE_MIN));
    RECORD_UNARY_L(expm1, LDBL_TRUE_MIN, expm1l(LDBL_TRUE_MIN));
    RECORD_UNARY_F(log1p, FLT_TRUE_MIN, log1pf(FLT_TRUE_MIN));
    RECORD_UNARY_D(log1p, DBL_TRUE_MIN, log1p(DBL_TRUE_MIN));
    RECORD_UNARY_L(log1p, LDBL_TRUE_MIN, log1pl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(log1p, -0x1.fffffep-1f, log1pf(-0x1.fffffep-1f));
    RECORD_UNARY_D(log1p, -0x1.fffffffffffffp-1,
                   log1p(-0x1.fffffffffffffp-1));
    RECORD_UNARY_L(log1p, -0xf.fffffffffffffffp-4L,
                   log1pl(-0xf.fffffffffffffffp-4L));

    RECORD_UNARY_F(erf, FLT_TRUE_MIN, erff(FLT_TRUE_MIN));
    RECORD_UNARY_D(erf, DBL_TRUE_MIN, erf(DBL_TRUE_MIN));
    RECORD_UNARY_L(erf, LDBL_TRUE_MIN, erfl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(erfc, FLT_TRUE_MIN, erfcf(FLT_TRUE_MIN));
    RECORD_UNARY_D(erfc, DBL_TRUE_MIN, erfc(DBL_TRUE_MIN));
    RECORD_UNARY_L(erfc, LDBL_TRUE_MIN, erfcl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(erf, 0.5f, erff(0.5f));
    RECORD_UNARY_D(erf, 0.5, erf(0.5));
    RECORD_UNARY_L(erf, 0.5L, erfl(0.5L));
    RECORD_UNARY_F(erfc, 0.5f, erfcf(0.5f));
    RECORD_UNARY_D(erfc, 0.5, erfc(0.5));
    RECORD_UNARY_L(erfc, 0.5L, erfcl(0.5L));
    RECORD_UNARY_F(erf, 10.0f, erff(10.0f));
    RECORD_UNARY_D(erf, 10.0, erf(10.0));
    RECORD_UNARY_L(erf, 10.0L, erfl(10.0L));
    RECORD_UNARY_F(erfc, 10.0f, erfcf(10.0f));
    RECORD_UNARY_D(erfc, 10.0, erfc(10.0));
    RECORD_UNARY_L(erfc, 10.0L, erfcl(10.0L));
    RECORD_UNARY_F(erf, nextafterf(1.0f, 0.0f), erff(nextafterf(1.0f, 0.0f)));
    RECORD_UNARY_D(erf, nextafter(1.0, 0.0), erf(nextafter(1.0, 0.0)));
    RECORD_UNARY_L(erf, nextafterl(1.0L, 0.0L), erfl(nextafterl(1.0L, 0.0L)));
    RECORD_UNARY_F(erfc, 1.0f, erfcf(1.0f));
    RECORD_UNARY_D(erfc, 1.0, erfc(1.0));
    RECORD_UNARY_L(erfc, 1.0L, erfcl(1.0L));
    RECORD_UNARY_F(erfc, nextafterf(1.0f, 2.0f), erfcf(nextafterf(1.0f, 2.0f)));
    RECORD_UNARY_D(erfc, nextafter(1.0, 2.0), erfc(nextafter(1.0, 2.0)));
    RECORD_UNARY_L(erfc, nextafterl(1.0L, 2.0L), erfcl(nextafterl(1.0L, 2.0L)));
    RECORD_UNARY_L(erfc, 0xd.5p+3L, erfcl(0xd.5p+3L));
    RECORD_UNARY_L(erfc, 0x8p+4L, erfcl(0x8p+4L));
    RECORD_UNARY_L(erfc, -0x8p+4L, erfcl(-0x8p+4L));
    RECORD_UNARY_F(erfc, nextafterf(2.0f, 0.0f), erfcf(nextafterf(2.0f, 0.0f)));
    RECORD_UNARY_F(erfc, 2.0f, erfcf(2.0f));
    RECORD_UNARY_F(erfc, nextafterf(2.0f, 4.0f), erfcf(nextafterf(2.0f, 4.0f)));
    RECORD_UNARY_D(erfc, nextafter(2.0, 0.0), erfc(nextafter(2.0, 0.0)));
    RECORD_UNARY_D(erfc, 2.0, erfc(2.0));
    RECORD_UNARY_D(erfc, nextafter(2.0, 4.0), erfc(nextafter(2.0, 4.0)));
    RECORD_UNARY_L(erfc, nextafterl(2.0L, 0.0L), erfcl(nextafterl(2.0L, 0.0L)));
    RECORD_UNARY_L(erfc, 2.0L, erfcl(2.0L));
    RECORD_UNARY_L(erfc, nextafterl(2.0L, 4.0L), erfcl(nextafterl(2.0L, 4.0L)));
    RECORD_UNARY_F(erfc, nextafterf(4.0f, 0.0f), erfcf(nextafterf(4.0f, 0.0f)));
    RECORD_UNARY_F(erfc, 4.0f, erfcf(4.0f));
    RECORD_UNARY_F(erfc, nextafterf(4.0f, 8.0f), erfcf(nextafterf(4.0f, 8.0f)));
    RECORD_UNARY_D(erfc, nextafter(4.0, 0.0), erfc(nextafter(4.0, 0.0)));
    RECORD_UNARY_D(erfc, 4.0, erfc(4.0));
    RECORD_UNARY_D(erfc, nextafter(4.0, 8.0), erfc(nextafter(4.0, 8.0)));
    RECORD_UNARY_L(erfc, nextafterl(4.0L, 0.0L), erfcl(nextafterl(4.0L, 0.0L)));
    RECORD_UNARY_L(erfc, 4.0L, erfcl(4.0L));
    RECORD_UNARY_L(erfc, nextafterl(4.0L, 8.0L), erfcl(nextafterl(4.0L, 8.0L)));
    RECORD_UNARY_F(erfc, 10.05f, erfcf(10.05f));
    RECORD_UNARY_F(erfc, 10.06f, erfcf(10.06f));
    RECORD_UNARY_D(erfc, 27.2, erfc(27.2));
    RECORD_UNARY_D(erfc, 27.3, erfc(27.3));
    RECORD_UNARY_L(erfc, 106.6L, erfcl(106.6L));
    RECORD_UNARY_L(erfc, 106.7L, erfcl(106.7L));
    RECORD_UNARY_L(erfc, 106.8L, erfcl(106.8L));
    RECORD_UNARY_L(erfc, 107.0L, erfcl(107.0L));

    RECORD_UNARY_F(sinh, 0x1p-20f, sinhf(0x1p-20f));
    RECORD_UNARY_D(sinh, 0x1.5p+9, sinh(0x1.5p+9));
    RECORD_UNARY_L(sinh, 0xb.17217f7d1cf79acp+10L,
                   sinhl(0xb.17217f7d1cf79acp+10L));
    RECORD_UNARY_F(atanh, 0x1.fffffep-1f, atanhf(0x1.fffffep-1f));
    RECORD_UNARY_D(atanh, 0x1.fffffffffffffp-1,
                   atanh(0x1.fffffffffffffp-1));
    RECORD_UNARY_L(atanh, 0xf.fffffffffffffffp-4L,
                   atanhl(0xf.fffffffffffffffp-4L));
    RECORD_UNARY_F(sinh, FLT_TRUE_MIN, sinhf(FLT_TRUE_MIN));
    RECORD_UNARY_D(sinh, DBL_TRUE_MIN, sinh(DBL_TRUE_MIN));
    RECORD_UNARY_L(sinh, LDBL_TRUE_MIN, sinhl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(asinh, FLT_TRUE_MIN, asinhf(FLT_TRUE_MIN));
    RECORD_UNARY_D(asinh, DBL_TRUE_MIN, asinh(DBL_TRUE_MIN));
    RECORD_UNARY_L(asinh, LDBL_TRUE_MIN, asinhl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(tanh, FLT_TRUE_MIN, tanhf(FLT_TRUE_MIN));
    RECORD_UNARY_D(tanh, DBL_TRUE_MIN, tanh(DBL_TRUE_MIN));
    RECORD_UNARY_L(tanh, LDBL_TRUE_MIN, tanhl(LDBL_TRUE_MIN));
    RECORD_UNARY_F(acosh, 0x1.000002p+0f, acoshf(0x1.000002p+0f));
    RECORD_UNARY_D(acosh, 0x1.0000000000001p+0,
                   acosh(0x1.0000000000001p+0));
    RECORD_UNARY_L(acosh, 0x1.0000000000000001p+0L,
                   acoshl(0x1.0000000000000001p+0L));

    RECORD_BINARY_F(pow, 2.0f, 127.0f, powf(2.0f, 127.0f));
    RECORD_BINARY_F(pow, 0.5f, 149.0f, powf(0.5f, 149.0f));
    RECORD_BINARY_D(pow, 2.0, 1023.0, pow(2.0, 1023.0));
    RECORD_BINARY_D(pow, 0.5, 1074.0, pow(0.5, 1074.0));
    RECORD_BINARY_L(pow, 0.5L, 16445.0L, powl(0.5L, 16445.0L));
    RECORD_BINARY_L(pow, 2.0L, 16383.0L, powl(2.0L, 16383.0L));

    const int directed_modes[] = {
        FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO
    };
    float exp2_f_modes[4], log1p_f_modes[4], log1pn_f_modes[4];
    double exp2_d_modes[4], log1p_d_modes[4], log1pn_d_modes[4];
    long double exp2_l_modes[4], log1p_l_modes[4], log1pn_l_modes[4];
    fenv_t directed_environment;
    if (fegetenv(&directed_environment)) return 36;
    for (unsigned i = 0; i < 4; ++i) {
        if (fesetround(directed_modes[i])) return 37;
        exp2_f_modes[i] = exp2f(0.5f);
        exp2_d_modes[i] = exp2(0.5);
        exp2_l_modes[i] = exp2l(0.5L);
        log1p_f_modes[i] = log1pf(0.5f);
        log1p_d_modes[i] = log1p(0.5);
        log1p_l_modes[i] = log1pl(0.5L);
        log1pn_f_modes[i] = log1pf(-0.25f);
        log1pn_d_modes[i] = log1p(-0.25);
        log1pn_l_modes[i] = log1pl(-0.25L);
        if (fegetround() != directed_modes[i]) return 39;
    }
    if (fesetenv(&directed_environment) || fegetround() != FE_TONEAREST)
        return 38;
    for (unsigned i = 0; i < 4; ++i) {
        RECORD_DIRECTED_F(exp2, 0.5f, directed_modes[i], exp2_f_modes[i]);
        RECORD_DIRECTED_D(exp2, 0.5, directed_modes[i], exp2_d_modes[i]);
        RECORD_DIRECTED_L(exp2, 0.5L, directed_modes[i], exp2_l_modes[i]);
        RECORD_DIRECTED_F(log1p, 0.5f, directed_modes[i], log1p_f_modes[i]);
        RECORD_DIRECTED_D(log1p, 0.5, directed_modes[i], log1p_d_modes[i]);
        RECORD_DIRECTED_L(log1p, 0.5L, directed_modes[i], log1p_l_modes[i]);
        RECORD_DIRECTED_F(log1p, -0.25f, directed_modes[i], log1pn_f_modes[i]);
        RECORD_DIRECTED_D(log1p, -0.25, directed_modes[i], log1pn_d_modes[i]);
        RECORD_DIRECTED_L(log1p, -0.25L, directed_modes[i], log1pn_l_modes[i]);
    }

    fenv_t tanh_environment;
    if (fegetenv(&tanh_environment)) return 40;
    for (unsigned i = 0; i < 4; ++i) {
        if (fesetround(directed_modes[i])) return 41;
        RECORD_DIRECTED_F(tanh, 32.0f, directed_modes[i], tanhf(32.0f));
        RECORD_DIRECTED_D(tanh, 64.0, directed_modes[i], tanh(64.0));
        RECORD_DIRECTED_L(tanh, 128.0L, directed_modes[i], tanhl(128.0L));
        RECORD_DIRECTED_F(cosh, FLT_TRUE_MIN, directed_modes[i],
                          coshf(FLT_TRUE_MIN));
        RECORD_DIRECTED_D(cosh, DBL_TRUE_MIN, directed_modes[i],
                          cosh(DBL_TRUE_MIN));
        RECORD_DIRECTED_L(cosh, LDBL_TRUE_MIN, directed_modes[i],
                          coshl(LDBL_TRUE_MIN));
        if (fegetround() != directed_modes[i]) return 42;
    }
    if (fesetenv(&tanh_environment)) return 43;

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

    fenv_t pow_environment;
    const int pow_modes[] = {FE_TONEAREST, FE_DOWNWARD,
                             FE_UPWARD, FE_TOWARDZERO};
    if (fegetenv(&pow_environment)) return 48;
    for (unsigned i = 0; i < sizeof pow_modes / sizeof pow_modes[0]; ++i) {
        if (fesetround(pow_modes[i])) return 49;
        float pf = powf(0x1.000002p+0f, 3.25f);
        double pd = pow(0x1.0000000000001p+0, 37.5);
        long double pl = powl(0x1.0000000000000002p+0L, 128.5L);
        if (fegetround() != pow_modes[i]) return 50;
        RECORD_POWER_ROUND_F(0x1.000002p+0f, 3.25f, pow_modes[i], pf);
        RECORD_POWER_ROUND_D(0x1.0000000000001p+0, 37.5, pow_modes[i], pd);
        RECORD_POWER_ROUND_L(0x1.0000000000000002p+0L, 128.5L, pow_modes[i], pl);
        RECORD_POWER_NEGATIVE_ROUND_F(-1.1f, 3.0f, pow_modes[i],
                                      powf(-1.1f, 3.0f));
        RECORD_POWER_NEGATIVE_ROUND_D(-1.1, 3.0, pow_modes[i],
                                      pow(-1.1, 3.0));
        RECORD_POWER_NEGATIVE_ROUND_L(-1.1L, 3.0L, pow_modes[i],
                                      powl(-1.1L, 3.0L));
        RECORD_POWER_LARGE_ROUND_D(-0x1.0000000000001p+0, 4294967297.0,
                                   pow_modes[i],
                                   pow(-0x1.0000000000001p+0, 4294967297.0));
        RECORD_POWER_LARGE_ROUND_L(-0x1.0000000000000002p+0L, 4294967297.0L,
                                   pow_modes[i],
                                   powl(-0x1.0000000000000002p+0L, 4294967297.0L));
    }
    if (fesetenv(&pow_environment)) return 51;

    printf("COMPLETE records=%u seed=0x4d414e47524f5645\n", records);
    return 0;
}
