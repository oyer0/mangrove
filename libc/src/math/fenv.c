/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fenv.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(fenv_t) >= 32, "fenv_t must hold x87 and SSE state");

static uint16_t x87_word(const fenv_t *env, unsigned offset)
{
    uint16_t value;
    memcpy(&value, env->__x87_environment + offset, sizeof value);
    return value;
}

static void set_x87_word(fenv_t *env, unsigned offset, uint16_t value)
{
    memcpy(env->__x87_environment + offset, &value, sizeof value);
}

int fegetenv(fenv_t *env)
{
    if (!env)
        return -1;
    __asm__ volatile("fnstenv %0\n\tfldenv %0"
                     : "+m"(env->__x87_environment) : : "memory");
    __asm__ volatile("stmxcsr %0" : "=m"(env->__mxcsr));
    return 0;
}

int fesetenv(const fenv_t *env)
{
    if (env == FE_DFL_ENV) {
        const uint32_t default_mxcsr = UINT32_C(0x1f80);
        __asm__ volatile("fninit" : : : "memory");
        __asm__ volatile("ldmxcsr %0" : : "m"(default_mxcsr));
        return 0;
    }
    if (!env)
        return -1;
    __asm__ volatile("fldenv %0" : : "m"(env->__x87_environment) : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(env->__mxcsr));
    return 0;
}

int fegetround(void)
{
    uint16_t control;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    unsigned x87_rounding = control & 0x0c00U;
    unsigned sse_rounding = (mxcsr >> 3) & 0x0c00U;
    return x87_rounding == sse_rounding ? (int)x87_rounding : -1;
}

int fesetround(int mode)
{
    if (mode != FE_TONEAREST && mode != FE_DOWNWARD &&
        mode != FE_UPWARD && mode != FE_TOWARDZERO)
        return -1;
    uint16_t control;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    control = (uint16_t)((control & ~0x0c00U) | (unsigned)mode);
    mxcsr = (mxcsr & ~UINT32_C(0x6000)) | ((unsigned)mode << 3);
    __asm__ volatile("fldcw %0" : : "m"(control));
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    return 0;
}

int fetestexcept(int excepts)
{
    uint16_t status;
    uint32_t mxcsr;
    __asm__ volatile("fnstsw %0" : "=am"(status));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    return (status | mxcsr) & excepts & FE_ALL_EXCEPT;
}

int fegetexceptflag(fexcept_t *flag, int excepts)
{
    if (!flag)
        return -1;
    *flag = (fexcept_t)fetestexcept(excepts);
    return 0;
}

int feclearexcept(int excepts)
{
    excepts &= FE_ALL_EXCEPT;
    fenv_t env;
    if (fegetenv(&env) != 0)
        return -1;
    uint16_t status = x87_word(&env, 4);
    status &= (uint16_t)~excepts;
    set_x87_word(&env, 4, status);
    env.__mxcsr &= ~(unsigned)excepts;
    return fesetenv(&env);
}

int fesetexceptflag(const fexcept_t *flag, int excepts)
{
    if (!flag)
        return -1;
    excepts &= FE_ALL_EXCEPT;
    fenv_t env;
    if (fegetenv(&env) != 0)
        return -1;
    uint16_t status = x87_word(&env, 4);
    status = (uint16_t)((status & ~excepts) | (*flag & excepts));
    set_x87_word(&env, 4, status);
    env.__mxcsr = (env.__mxcsr & ~(unsigned)excepts) | (*flag & excepts);
    return fesetenv(&env);
}

int feraiseexcept(int excepts)
{
    excepts &= FE_ALL_EXCEPT;
    fenv_t env;
    if (fegetenv(&env) != 0)
        return -1;
    uint16_t status = x87_word(&env, 4);
    set_x87_word(&env, 4, (uint16_t)(status | excepts));
    env.__mxcsr |= (unsigned)excepts;
    return fesetenv(&env);
}

int feholdexcept(fenv_t *env)
{
    if (!env || fegetenv(env) != 0)
        return -1;
    uint16_t control = x87_word(env, 0);
    uint16_t status = x87_word(env, 4);
    control |= 0x003fU;
    status &= (uint16_t)~0x003fU;
    set_x87_word(env, 0, control);
    set_x87_word(env, 4, status);
    env->__mxcsr = (env->__mxcsr | UINT32_C(0x1f80)) & ~UINT32_C(0x3f);
    return fesetenv(env);
}

int feupdateenv(const fenv_t *env)
{
    int exceptions = fetestexcept(FE_ALL_EXCEPT);
    if (fesetenv(env) != 0)
        return -1;
    return feraiseexcept(exceptions);
}
