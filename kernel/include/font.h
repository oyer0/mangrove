#ifndef FONT_H
#define FONT_H

#include <bootinfo.h>
#include <types.h>

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xB5
#define PSF2_MAGIC2 0x4A
#define PSF2_MAGIC3 0x86

typedef struct {
    u8  magic[4];
    u32 version;
    u32 headerfile;
    u32 flags;
    u32 numglyph;
    u32 bytesperglyph;
    u32 height;
    u32 width;
} psf2_header_t;

extern const unsigned char _binary_src_font_psf_start[];
extern const unsigned char _binary_src_font_psf_end[];

void draw_char(char c,
               u32 x,
               u32 y,
               u32 fg_color,
               u32 bg_color,
               BOOT_INFO *BootInfo);

void draw_string(const char *str,
                 u32 x,
                 u32 y,
                 u32 fg_color,
                 u32 bg_color,
                 BOOT_INFO *BootInfo);

#endif
