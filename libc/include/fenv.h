/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* x86-64 userspace environment: x87 environment plus the SSE MXCSR. */
typedef unsigned int fexcept_t;
typedef struct {
    unsigned char __x87_environment[28];
    unsigned int __mxcsr;
} fenv_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00

/* This implementation-defined sentinel selects fninit and the default MXCSR. */
#define FE_DFL_ENV ((const fenv_t *)(long)-1)

int feclearexcept(int);
int fegetexceptflag(fexcept_t *, int);
int feraiseexcept(int);
int fesetexceptflag(const fexcept_t *, int);
int fetestexcept(int);
int fegetround(void);
int fesetround(int);
int fegetenv(fenv_t *);
int feholdexcept(fenv_t *);
int fesetenv(const fenv_t *);
int feupdateenv(const fenv_t *);
