/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MG_MATH_PRIVATE_H
#define MG_MATH_PRIVATE_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t significand;
    uint16_t sign_exponent;
    unsigned char padding[6];
} mg_x87_value;

static inline uint32_t mg_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static inline float mg_float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline uint64_t mg_double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static inline double mg_double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline mg_x87_value mg_x87_unpack(long double value)
{
    mg_x87_value raw;
    memcpy(&raw, &value, sizeof value);
    return raw;
}

static inline long double mg_x87_pack(mg_x87_value raw)
{
    long double value;
    memcpy(&value, &raw, sizeof value);
    return value;
}

static inline unsigned mg_x87_exponent(mg_x87_value raw)
{
    return raw.sign_exponent & 0x7fffU;
}

static inline int mg_x87_negative(mg_x87_value raw)
{
    return (raw.sign_exponent & 0x8000U) != 0;
}

static inline uint64_t mg_x87_integer_bit(void)
{
    return UINT64_C(1) << 63;
}

int __mg_fpclassifyf(float);
int __mg_fpclassify(double);
int __mg_fpclassifyl(long double);
int __mg_signbitf(float);
int __mg_signbit(double);
int __mg_signbitl(long double);

#endif
