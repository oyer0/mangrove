/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t state = UINT64_C(0x464d415f4d414e47);
static unsigned records;

static uint64_t random_u64(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static float float_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static double double_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static long double x87_value(uint64_t significand, uint16_t exponent)
{
    struct {
        uint64_t significand;
        uint16_t sign_exponent;
        unsigned char padding[6];
    } raw = {significand, exponent, {0}};
    long double value;
    memcpy(&value, &raw, sizeof value);
    return value;
}

#define RECORD_F(x, y, z) \
    do { printf("F fma %a %a %a %a\n", (double)(x), (double)(y), \
                (double)(z), (double)fmaf((x), (y), (z))); ++records; } while (0)
#define RECORD_D(x, y, z) \
    do { printf("D fma %a %a %a %a\n", (x), (y), (z), fma((x), (y), (z))); \
         ++records; } while (0)
#define RECORD_L(x, y, z) \
    do { printf("L fma %La %La %La %La\n", (x), (y), (z), \
                fmal((x), (y), (z))); ++records; } while (0)
#define RECORD_F_ROUND(mode, x, y, z) \
    do { printf("F fma-round %a %a %a %d %a\n", (double)(x), (double)(y), \
                (double)(z), (mode), (double)fmaf((x), (y), (z))); ++records; } while (0)
#define RECORD_D_ROUND(mode, x, y, z) \
    do { printf("D fma-round %a %a %a %d %a\n", (x), (y), (z), (mode), \
                fma((x), (y), (z))); ++records; } while (0)
#define RECORD_L_ROUND(mode, x, y, z) \
    do { printf("L fma-round %La %La %La %d %La\n", (x), (y), (z), \
                (mode), fmal((x), (y), (z))); ++records; } while (0)

static int edge_checks(void)
{
    if (fmaf(0x1.000002p+0f, 0x1.fffffcp-1f, -1.0f) != -0x1p-46f ||
        fma(1.0 + 0x1p-27, 1.0 - 0x1p-27, -1.0) != -0x1p-54 ||
        fmal(1.0L + 0x1p-63L, 1.0L - 0x1p-63L, -1.0L) != -0x1p-126L ||
        fma(DBL_MAX, 2.0, -DBL_MAX) != DBL_MAX ||
        fmal(LDBL_MAX, 2.0L, -LDBL_MAX) != LDBL_MAX)
        return 1;

    fenv_t original;
    if (fegetenv(&original) != 0)
        return 2;
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fma(1.0, 1.0, -1.0) != 0.0 ||
        signbit(fma(1.0, 1.0, -1.0)) || fetestexcept(FE_ALL_EXCEPT) != 0)
        return 3;
    if (fesetround(FE_DOWNWARD) != 0 ||
        !signbit(fma(1.0, 1.0, -1.0)) ||
        fesetenv(&original) != 0)
        return 4;

    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fma(DBL_MAX, 2.0, 0.0) != HUGE_VAL ||
        fetestexcept(FE_OVERFLOW | FE_INEXACT) !=
            (FE_OVERFLOW | FE_INEXACT))
        return 5;
    if (fesetround(FE_DOWNWARD) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fma(DBL_MAX, 2.0, 0.0) != DBL_MAX ||
        fetestexcept(FE_OVERFLOW | FE_INEXACT) !=
            (FE_OVERFLOW | FE_INEXACT) || fesetenv(&original) != 0)
        return 6;
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fma(DBL_TRUE_MIN, 0.5, 0.0) != 0.0 ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT))
        return 7;
    if (fesetround(FE_UPWARD) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fma(DBL_TRUE_MIN, 0.5, 0.0) != DBL_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) || fesetenv(&original) != 0)
        return 8;
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fmaf(FLT_TRUE_MIN, 0.5f, 0.0f) != 0.0f ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) || fesetround(FE_UPWARD) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fmaf(FLT_TRUE_MIN, 0.5f, 0.0f) != FLT_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) || fesetenv(&original) != 0)
        return 9;
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fmal(LDBL_TRUE_MIN, 0.5L, 0.0L) != 0.0L ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) || fesetround(FE_UPWARD) != 0 ||
        feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fmal(LDBL_TRUE_MIN, 0.5L, 0.0L) != LDBL_TRUE_MIN ||
        fetestexcept(FE_UNDERFLOW | FE_INEXACT) !=
            (FE_UNDERFLOW | FE_INEXACT) || fesetenv(&original) != 0)
        return 10;
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fma(DBL_TRUE_MIN, 1.0, 0.0) != DBL_TRUE_MIN ||
        fetestexcept(FE_ALL_EXCEPT) != 0 ||
        fma(DBL_MAX, 2.0, -DBL_MAX) != DBL_MAX ||
        fetestexcept(FE_ALL_EXCEPT) != 0)
        return 11;

    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        !isnan(fma(0.0, INFINITY, 1.0)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 12;
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        fmaf(INFINITY, INFINITY, 1.0f) != INFINITY ||
        fetestexcept(FE_INVALID) != 0 ||
        !isnan(fma(INFINITY, INFINITY, -INFINITY)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 13;
    uint32_t snan_bits = UINT32_C(0x7f800001);
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        !isnan(fmaf(float_bits(snan_bits), 1.0f, 0.0f)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 14;
    uint64_t dsnan_bits = UINT64_C(0x7ff0000000000001);
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        !isnan(fma(double_bits(dsnan_bits), 1.0, 0.0)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 15;
    long double lsnan = x87_value(UINT64_C(0x8000000000000001), 0x7fffU);
    if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
        !isnan(fmal(lsnan, 1.0L, 0.0L)) ||
        !(fetestexcept(FE_INVALID) & FE_INVALID))
        return 16;
    return 0;
}

int main(void)
{
    int edge = edge_checks();
    printf("CHECK %d\n", edge);
    if (edge)
        return edge;

    /* Cancellation, fused-only results, exact maximum-range recovery, and
     * ties under each supported rounding direction. */
    RECORD_F(0x1.000002p+0f, 0x1.fffffcp-1f, -1.0f);
    RECORD_D(1.0 + 0x1p-27, 1.0 - 0x1p-27, -1.0);
    RECORD_L(1.0L + 0x1p-63L, 1.0L - 0x1p-63L, -1.0L);
    RECORD_D(DBL_MAX, 2.0, -DBL_MAX);
    RECORD_L(LDBL_MAX, 2.0L, -LDBL_MAX);
    RECORD_F(FLT_MAX, FLT_TRUE_MIN, -0x1p-21f);
    RECORD_D(DBL_MAX, DBL_TRUE_MIN, -0x1p-50);
    RECORD_L(LDBL_MAX, LDBL_TRUE_MIN, -0x1p-61L);
    const int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        if (fesetround(modes[i]) != 0)
            return 17;
        RECORD_F_ROUND(modes[i], 1.0f + 0x1p-23f,
                       1.0f + 0x1p-23f, -1.0f);
        RECORD_F_ROUND(modes[i], -(1.0f + 0x1p-23f),
                       1.0f + 0x1p-23f, 1.0f);
        RECORD_D_ROUND(modes[i], 1.0 + 0x1p-52,
                       1.0 + 0x1p-52, -1.0);
        RECORD_D_ROUND(modes[i], -(1.0 + 0x1p-52),
                       1.0 + 0x1p-52, 1.0);
        RECORD_L_ROUND(modes[i], 1.0L + 0x1p-63L,
                       1.0L + 0x1p-63L, -1.0L);
        RECORD_L_ROUND(modes[i], -(1.0L + 0x1p-63L),
                       1.0L + 0x1p-63L, 1.0L);
        RECORD_F_ROUND(modes[i], 1.0f, 1.0f, FLT_TRUE_MIN);
        RECORD_F_ROUND(modes[i], -1.0f, 1.0f, -FLT_TRUE_MIN);
        RECORD_D_ROUND(modes[i], 1.0, 1.0, DBL_TRUE_MIN);
        RECORD_D_ROUND(modes[i], -1.0, 1.0, -DBL_TRUE_MIN);
        RECORD_L_ROUND(modes[i], 1.0L, 1.0L, LDBL_TRUE_MIN);
        RECORD_L_ROUND(modes[i], -1.0L, 1.0L, -LDBL_TRUE_MIN);
    }
    if (fesetround(FE_TONEAREST) != 0)
        return 18;

    for (unsigned i = 0; i < 512; ++i) {
        uint32_t fb[3];
        uint64_t db[3];
        int float_exponent = (int)(random_u64() % 253U) - 126;
        int double_exponent = (int)(random_u64() % 2045U) - 1022;
        int long_exponent = (int)(random_u64() % 32765U) - 16382;
        for (unsigned j = 0; j < 3; ++j) {
            int fe = j == 0 ? float_exponent :
                     j == 1 ? -float_exponent :
                     (int)(random_u64() % 9U) - 4;
            int de = j == 0 ? double_exponent :
                     j == 1 ? -double_exponent :
                     (int)(random_u64() % 9U) - 4;
            uint32_t fsign = random_u64() & 1U ? UINT32_C(0x80000000) : 0;
            uint64_t dsign = random_u64() & 1U ?
                             UINT64_C(0x8000000000000000) : 0;
            fb[j] = fsign | ((uint32_t)(fe + 127) << 23) |
                    ((uint32_t)random_u64() & UINT32_C(0x007fffff));
            db[j] = dsign | ((uint64_t)(de + 1023) << 52) |
                    (random_u64() & UINT64_C(0x000fffffffffffff));
        }
        uint16_t le[3];
        uint64_t ls[3];
        for (unsigned j = 0; j < 3; ++j) {
            int e = j == 0 ? long_exponent :
                    j == 1 ? -long_exponent :
                    (int)(random_u64() % 9U) - 4;
            unsigned sign = random_u64() & 1U ? 0x8000U : 0;
            le[j] = (uint16_t)(sign | (unsigned)(e + 16383));
            ls[j] = random_u64() | (UINT64_C(1) << 63);
        }
        RECORD_F(float_bits(fb[0]), float_bits(fb[1]), float_bits(fb[2]));
        RECORD_D(double_bits(db[0]), double_bits(db[1]), double_bits(db[2]));
        RECORD_L(x87_value(ls[0], le[0]), x87_value(ls[1], le[1]),
                 x87_value(ls[2], le[2]));
    }
    printf("COMPLETE records=%u seed=0x464d415f4d414e47\n", records);
    return 0;
}
