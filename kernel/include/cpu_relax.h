/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Keep a busy-wait loop friendly to the executing x86 processor. */
static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}
