/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#ifndef BOOT_HANDOFF_CHECKPOINT
#define BOOT_HANDOFF_CHECKPOINT 0
#endif

#if BOOT_HANDOFF_CHECKPOINT < 0 || BOOT_HANDOFF_CHECKPOINT > 8
#error BOOT_HANDOFF_CHECKPOINT must be between 0 and 8
#endif

static inline void boot_checkpoint_mark(volatile u32 *framebuffer,
                                        u32 width, u32 height, u32 stride,
                                        u32 color)
{
    const u32 marker_size = 16U;

    if (!framebuffer || width < marker_size || height < marker_size ||
        stride < width)
        return;

    volatile u32 *row = framebuffer +
        (usize)(height - marker_size) * stride + width - marker_size;
    for (u32 y = 0; y < marker_size; y++) {
        for (u32 x = 0; x < marker_size; x++)
            row[x] = color;
        row += stride;
    }
}

static inline __attribute__((noreturn)) void boot_checkpoint_halt(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}
