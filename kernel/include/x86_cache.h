/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Boot-only policy: BSP preparation precedes permanent mappings; an AP must
 * agree before it becomes an online CPU. No runtime PAT/MTRR reconfiguration. */
bool x86_cache_init_bsp(void);
bool x86_cache_init_ap(u32 index);
bool x86_cache_cpu_ready(u32 index);
u64 x86_cache_uc_flags(void);
bool x86_cache_framebuffer_wc(u64 physical, u64 size, u64 *flags);

/* Switch away from the loader's mappings with caches and all local TLB
 * entries drained. Called once, with IF clear, before AP startup. */
void x86_cache_activate_boot_root(u64 physical);
