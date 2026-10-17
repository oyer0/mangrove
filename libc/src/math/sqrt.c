/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>

/* x86-64 guarantees SSE2. The scalar instructions implement the target's
 * IEEE square-root operation and use the active MXCSR rounding mode. x87
 * FSQRT operates directly at the extended precision of long double. */
float sqrtf(float value)
{
    float result;
    __asm__ volatile("sqrtss %1, %0" : "=x"(result) : "x"(value));
    return result;
}

double sqrt(double value)
{
    double result;
    __asm__ volatile("sqrtsd %1, %0" : "=x"(result) : "x"(value));
    return result;
}

long double sqrtl(long double value)
{
    long double result;
    __asm__ volatile("fldt %1\n\tfsqrt\n\tfstpt %0"
                     : "=m"(result) : "m"(value) : "st");
    return result;
}
