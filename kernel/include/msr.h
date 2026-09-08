/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define MSR_IA32_GS_BASE        0xC0000101U
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102U

u64 rdmsr(u32 msr);
void wrmsr(u32 msr, u64 value);
