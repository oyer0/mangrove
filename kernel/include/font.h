#ifndef FONT_H
#define FONT_H

#include <types.h>
#include <bootinfo.h>
#include <stdint.h>

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86

typedef struct {
    u8 magic[4];
    u32 version;
    u32 headerfile;
    u32 flags;
    u32 numglyph;
    u32 bytesperglyph;
    u32 height;
    u32 width;
} __attribute__((packed)) psf2_header_t;

extern char _binary_src_font_psf_start[];
extern char _binary_src_font_psf_end[];

void draw_char(char c, u32 x_pos, u32 y_pos, u32 color, BOOT_INFO *BootInfo);
void draw_string(const char *str, u32 x_pos, u32 y_pos, u32 color, BOOT_INFO *BootInfo);

#endif
