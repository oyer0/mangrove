/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "math_private.h"
#include <float.h>
#include <math.h>

_Static_assert(sizeof(float) == sizeof(uint32_t), "binary32 float required");
_Static_assert(sizeof(double) == sizeof(uint64_t), "binary64 double required");
_Static_assert(sizeof(long double) == sizeof(mg_x87_value), "x87 long double ABI required");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "IEEE binary32 float required");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "IEEE binary64 double required");
_Static_assert(LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384,
               "x87 extended long double required");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Mangrove x86-64 math requires little-endian floating-point storage
#endif

int __mg_fpclassifyf(float value)
{
    uint32_t bits = mg_float_bits(value);
    uint32_t exponent = (bits >> 23) & 0xffU;
    uint32_t fraction = bits & UINT32_C(0x7fffff);

    if (exponent == 0xffU)
        return fraction ? FP_NAN : FP_INFINITE;
    if (exponent != 0)
        return FP_NORMAL;
    return fraction ? FP_SUBNORMAL : FP_ZERO;
}

int __mg_fpclassify(double value)
{
    uint64_t bits = mg_double_bits(value);
    uint64_t exponent = (bits >> 52) & 0x7ffU;
    uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);

    if (exponent == 0x7ffU)
        return fraction ? FP_NAN : FP_INFINITE;
    if (exponent != 0)
        return FP_NORMAL;
    return fraction ? FP_SUBNORMAL : FP_ZERO;
}

int __mg_fpclassifyl(long double value)
{
    mg_x87_value raw = mg_x87_unpack(value);
    unsigned exponent = mg_x87_exponent(raw);
    uint64_t integer_bit = mg_x87_integer_bit();

    if (exponent == 0x7fffU)
        return raw.significand == integer_bit ? FP_INFINITE : FP_NAN;
    if (exponent == 0)
        return raw.significand == 0 ? FP_ZERO :
               (raw.significand & integer_bit) ? FP_NORMAL : FP_SUBNORMAL;
    return (raw.significand & integer_bit) ? FP_NORMAL : FP_NAN;
}

int __mg_signbitf(float value)
{
    return (mg_float_bits(value) >> 31) != 0;
}

int __mg_signbit(double value)
{
    return (mg_double_bits(value) >> 63) != 0;
}

int __mg_signbitl(long double value)
{
    return mg_x87_negative(mg_x87_unpack(value));
}

/* Payload text is intentionally mapped to one canonical quiet NaN.  The
 * interpretation of the optional tag is implementation-defined by C; keeping
 * it out of the representation avoids locale, allocation and parsing rules. */
float nanf(const char *tag)
{
    (void)tag;
    return mg_float_from_bits(UINT32_C(0x7fc00000));
}

double nan(const char *tag)
{
    (void)tag;
    return mg_double_from_bits(UINT64_C(0x7ff8000000000000));
}

long double nanl(const char *tag)
{
    (void)tag;
    mg_x87_value value = {mg_x87_integer_bit() | (UINT64_C(1) << 62),
                          0x7fffU, {0}};
    return mg_x87_pack(value);
}

int __mg_isfinitef(float value) { return __mg_fpclassifyf(value) != FP_INFINITE && __mg_fpclassifyf(value) != FP_NAN; }
int __mg_isfinite(double value) { return __mg_fpclassify(value) != FP_INFINITE && __mg_fpclassify(value) != FP_NAN; }
int __mg_isfinitel(long double value) { return __mg_fpclassifyl(value) != FP_INFINITE && __mg_fpclassifyl(value) != FP_NAN; }

#define MG_COMPARE_IMPL(suffix, type, classify) \
int __mg_isgreater##suffix(type x, type y) \
{ return classify(x) != FP_NAN && classify(y) != FP_NAN && x > y; } \
int __mg_isgreaterequal##suffix(type x, type y) \
{ return classify(x) != FP_NAN && classify(y) != FP_NAN && x >= y; } \
int __mg_isless##suffix(type x, type y) \
{ return classify(x) != FP_NAN && classify(y) != FP_NAN && x < y; } \
int __mg_islessequal##suffix(type x, type y) \
{ return classify(x) != FP_NAN && classify(y) != FP_NAN && x <= y; } \
int __mg_islessgreater##suffix(type x, type y) \
{ return classify(x) != FP_NAN && classify(y) != FP_NAN && (x < y || x > y); } \
int __mg_isunordered##suffix(type x, type y) \
{ return classify(x) == FP_NAN || classify(y) == FP_NAN; }

MG_COMPARE_IMPL(f, float, __mg_fpclassifyf)
MG_COMPARE_IMPL(, double, __mg_fpclassify)
MG_COMPARE_IMPL(l, long double, __mg_fpclassifyl)
