#pragma once

#include <bootinfo.h>
#include <types.h>

void framebuffer_init(BOOT_INFO *boot_info);
/* Rebind the CPU framebuffer pointer after the permanent high ioremap exists. */
void framebuffer_set_mmio(void *virtual_address);

void framebuffer_put_pixel(
    u32 x,
    u32 y,
    u32 color
);

void framebuffer_clear(u32 color);

void framebuffer_copy_rows(
    u32 dst_y,
    u32 src_y,
    u32 row_count
);

void framebuffer_fill_rows(
    u32 start_y,
    u32 row_count,
    u32 color
);

u32 framebuffer_width(void);
u32 framebuffer_height(void);
u32 framebuffer_pitch(void);
void* framebuffer_ptr(void);

void framebuffer_flush_rows(u32 start_y, u32 flush_height);
void framebuffer_enable_backbuffer(void);
