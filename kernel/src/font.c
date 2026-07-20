#include <font.h>
#include <stdarg.h>

static struct {
    u32 cursor_x;
    u32 cursor_y;
    u32 color;
    BOOT_INFO *boot_info;
} terminal;

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

void draw_string(const char *str, u32 x_pos, u32 y_pos, u32 color, BOOT_INFO *BootInfo) {
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 current_x = x_pos;

    while (*str) {
        draw_char(*str, current_x, y_pos, color, BootInfo);

        current_x += font_header->width;

        str++;
    }
}

void itoa(u64 value, char *str, u32 base) {
    char *rc;
    char *ptr;
    char *low;
    const char *digits = "0123456789ABCDEF";
    
    rc = ptr = str;

    do {
        *ptr++ = digits[value % base];
        value /= base;
    } while (value);

    *ptr = '\0';

    low = rc;
    ptr--;
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
}

void kprint_init(BOOT_INFO *BootInfo) {
    terminal.cursor_x = 10;
    terminal.cursor_y = 10;
    terminal.color = 0x0B6623;
    terminal.boot_info = BootInfo;
}

void kprint(const char *format, ...) {
    if (!terminal.boot_info) return;

    va_list args;
    va_start(args, format);

    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    char buf[64];

    while (*format) {
        if (*format == '%') {
            format++;

            switch (*format) {
                case 's': {
                    char *s = va_arg(args, char *);
                    while (*s) {
                        if (*s == '\n') {
                            terminal.cursor_x = 10;
                            terminal.cursor_y += font_header->height + 4;
                        } else {
                            draw_char(*s, terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                            terminal.cursor_x += font_header->width;
                        }
                        s++;
                    } break;
                }
                case 'd': {
                    i64 d = va_arg(args, i64);
                    if (d < 0) {
                        draw_char('-', terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                        terminal.cursor_x += font_header->width;
                        d = -d;
                    }
                    itoa((u64)d, buf, 10);
                    for (int i = 0; buf[i] != '\0'; i++) {
                        draw_char(buf[i], terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                        terminal.cursor_x += font_header->width;
                    }
                    break;
                }
                case 'x': {
                    u64 x = va_arg(args, u64);
                    draw_char('0', terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                    terminal.cursor_x += font_header->width;
                    draw_char('x', terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                    terminal.cursor_x += font_header->width;

                    itoa(x, buf, 16);
                    for (int i = 0; buf[i] != '\0'; i++) {
                        draw_char(buf[i], terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                        terminal.cursor_x += font_header->width;
                    }
                    break;
                }
                case '%': {
                    draw_char('%', terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
                    terminal.cursor_x += font_header->width;
                    break;
                }
            }
        } else if (*format == '\n') {
            terminal.cursor_x = 10;
            terminal.cursor_y += font_header->height + 4;
        } else {
            draw_char(*format, terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
            terminal.cursor_x += font_header->width;
        }
        format++;
    }

    va_end(args);
}
