/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <types.h>

u64 rdmsr(u32 msr);
void wrmsr(u32 msr, u64 value);
