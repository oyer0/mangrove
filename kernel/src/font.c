#include <font.h>
#include <bootinfo.h>

void draw_char(char c, u32 x_pos, u32 y_pos, u32 color, BOOT_INFO *BootInfo) {
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;

    if (font_header->magic[0] != PSF2_MAGIC0 || font_header->magic[1] != PSF2_MAGIC1 ||
        font_header->magic[2] != PSF2_MAGIC2 || font_header->magic[3] != PSF2_MAGIC3) {
        return;
    }

    u8 *glyph_buffer = (u8 *)_binary_src_font_psf_start + font_header->headerfile;

    if ((u32)c >= font_header->numglyph) {
        c = ' ';
    }
    u8 *glyph = glyph_buffer + (c * font_header->bytesperglyph);

    u32 *fb = (u32 *)BootInfo->FramebufferBase;

    for (u32 py = 0; py < font_header->height; py++) {
        u8 row_byte = glyph[py];

        for (u32 px = 0; px < font_header->width; px++) {
            if ((row_byte << px) & 0x80) {
                usize fb_index = (y_pos + py) * BootInfo->PixelsPerScanLine + (x_pos + px);
                fb[fb_index] = color;
            }
        }
    }
}
