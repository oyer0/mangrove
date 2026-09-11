/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define FPU_STATE_SIZE       512U
#define FPU_STATE_ALIGNMENT  16U

/* The baseline x86-64 floating-point state saved by FXSAVE/FXRSTOR.
 * AVX/XSAVE state is deliberately outside this first implementation. */
typedef struct fpu_state {
    u8 bytes[FPU_STATE_SIZE];
} __attribute__((aligned(FPU_STATE_ALIGNMENT))) fpu_state_t;

_Static_assert(sizeof(fpu_state_t) == FPU_STATE_SIZE,
               "unexpected FPU state size");
_Static_assert(__alignof__(fpu_state_t) >= FPU_STATE_ALIGNMENT,
               "FPU state must be FXSAVE aligned");

/* Normalize the architectural FPU control state on the executing CPU. */
bool fpu_init_cpu(void);

/* Give a new thread a deterministic x87/SSE state. */
void fpu_state_init(fpu_state_t *state);
void fpu_state_save(fpu_state_t *state);
void fpu_state_restore(const fpu_state_t *state);
