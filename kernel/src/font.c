#include <font.h>
#include <framebuffer.h>

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

extern char _binary_kernel_src_font_psf_start[];
extern char _binary_kernel_src_font_psf_end[];

static psf2_header_t *font_header = 0;
static u8 *glyph_buffer = 0;


void font_init(BOOT_INFO *BootInfo)
{
    (void)BootInfo;
    font_header = (psf2_header_t *)_binary_kernel_src_font_psf_start;

    if (font_header->magic[0] != PSF2_MAGIC0 ||
        font_header->magic[1] != PSF2_MAGIC1 ||
        font_header->magic[2] != PSF2_MAGIC2 ||
        font_header->magic[3] != PSF2_MAGIC3)
    {
        for (;;)
            __asm__ volatile ("hlt");
    }

    glyph_buffer =
        (u8 *)_binary_kernel_src_font_psf_start + font_header->headerfile;
}

u32 font_width(void)
{
    return font_header->width;
}

u32 font_height(void)
{
    return font_header->height;
}

void draw_char(char c,
               u32 x,
               u32 y,
               u32 fg_color,
               u32 bg_color)
{
    u32 glyph_index = (u32)(u8)c;

    if (glyph_index >= font_header->numglyph) {
        glyph_index = ' ';
    }

    u8 *glyph = glyph_buffer +
        (glyph_index * font_header->bytesperglyph);



    for (u32 py = 0; py < font_header->height; py++) {

        if (y + py >= framebuffer_height())
            break;

        u8 row = glyph[py];

        for (u32 px = 0; px < font_header->width; px++) {

            if (x + px >= framebuffer_width())
                break;

            framebuffer_put_pixel(x + px, y + py,
                ((row << px) & 0x80) ? fg_color : bg_color);
        }
    }
}
