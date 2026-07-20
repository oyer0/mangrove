#include <font.h>
#include <stdarg.h>

static struct {
    u32 cursor_x;
    u32 cursor_y;
    u32 color;
    u32 bg_color;
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

static void terminal_scroll(void) {
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 line_height = font_header->height + 4;
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    u32 width = terminal.boot_info->FramebufferWidth;
    u32 height = terminal.boot_info->FramebufferHeight;
    u32 scanline = terminal.boot_info->PixelsPerScanLine;

    for (u32 y = 10; y < height - line_height - 10; y++) {
        for (u32 x = 10; x < width - 10; x++) {
            fb[y * scanline + x] = fb[(y + line_height) * scanline + x];
        }
    }

    for (u32 y = height - line_height - 10; y < height - 10; y++) {
        for (u32 x = 10; x < width - 10; x++) {
            fb[y * scanline + x] = terminal.bg_color; 
        }
    }

    terminal.cursor_y -= line_height;
}

static void clear_cursor(void) {
    if (!terminal.boot_info) return;
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    u32 scanline = terminal.boot_info->PixelsPerScanLine;
    
    for (u32 py = 0; py < font_header->height; py++) {
        for (u32 px = 0; px < font_header->width; px++) {
            usize fb_index = (terminal.cursor_y + py) * scanline + (terminal.cursor_x + px);
            fb[fb_index] = terminal.bg_color; 
        }
    }
}

static void draw_cursor(void) {
    if (!terminal.boot_info) return;
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    u32 scanline = terminal.boot_info->PixelsPerScanLine;
    
    for (u32 py = font_header->height - 2; py < font_header->height; py++) {
        for (u32 px = 0; px < font_header->width; px++) {
            usize fb_index = (terminal.cursor_y + py) * scanline + (terminal.cursor_x + px);
            fb[fb_index] = terminal.color;
        }
    }
}

static void terminal_putc(char c) {
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 line_height = font_header->height + 4;

    clear_cursor();

    if (c == '\n') {
        terminal.cursor_x = 10;
        terminal.cursor_y += line_height;
        if (terminal.cursor_y + line_height >= terminal.boot_info->FramebufferHeight - 10) {
            terminal_scroll();
        }
        return;
    }

    if (terminal.cursor_x + font_header->width >= terminal.boot_info->FramebufferWidth - 10) {
        terminal.cursor_x = 10;
        terminal.cursor_y += line_height;
        if (terminal.cursor_y + line_height >= terminal.boot_info->FramebufferHeight - 10) {
            terminal_scroll();
        }
    }

    draw_char(c, terminal.cursor_x, terminal.cursor_y, terminal.color, terminal.boot_info);
    terminal.cursor_x += font_header->width;
}

void kprint_init(BOOT_INFO *BootInfo) {
    terminal.cursor_x = 10;
    terminal.cursor_y = 10;
    terminal.color = 0x0B6623;
    terminal.bg_color = 0xFFFFFFFF;
    terminal.boot_info = BootInfo;
}

void kprint(const char *format, ...) {
    if (!terminal.boot_info) return;

    va_list args;
    va_start(args, format);
    char buf[64];

    while (*format) {
        if (*format == '%') {
            format++;

            switch (*format) {
                case 's': {
                    char *s = va_arg(args, char *);
                    while (*s) {
                        terminal_putc(*s);
                        s++;
                    }
                    break;
                }
                case 'd': {
                    i64 d = va_arg(args, i64);
                    if (d < 0) {
                        terminal_putc('-');
                        d = -d;
                    }
                    itoa((u64)d, buf, 10);
                    for (int i = 0; buf[i] != '\0'; i++) {
                        terminal_putc(buf[i]);
                    }
                    break;
                }
                case 'u': {
                    u64 u = va_arg(args, u64);
                    itoa(u, buf, 10);
                    for (int i = 0; buf[i] != '\0'; i++) {
                        terminal_putc(buf[i]);
                    }
                    break;
                }
                case 'x': {
                    u64 x = va_arg(args, u64);
                    terminal_putc('0');
                    terminal_putc('x');
                    itoa(x, buf, 16);
                    for (int i = 0; buf[i] != '\0'; i++) {
                        terminal_putc(buf[i]);
                    }
                    break;
                }
                case 'p': {
                    u64 p = (u64)va_arg(args, void *);
                    terminal_putc('0');
                    terminal_putc('x');
                    itoa(p, buf, 16);
                    int len = 0;
                    while (buf[len] != '\0') len++;
                    int padding = 16 - len;
                    for (int i = 0; i < padding; i++) {
                        terminal_putc('0');
                    }
                    for (int i = 0; buf[i] != '\0'; i++) {
                        terminal_putc(buf[i]);
                    }
                    break;
                }
                case '%': {
                    terminal_putc('%');
                    break;
                }
            }
        } else {
            terminal_putc(*format);
        }
        format++;
    }

    draw_cursor();
    va_end(args);
}

void kprint_centered(const char *text)
{
    terminal.cursor_x = 10;

    int len = 0;
    while (text[len] != '\0')
        len++;

    const int banner_width = 55;
    int left = (banner_width - len) / 2;

    for (int i = 0; i < left; i++)
        terminal_putc(' ');

    while (*text)
        terminal_putc(*text++);
}

void kprint_set_color(u32 color) {
    terminal.color = color;
}

void kprint_clear_screen(u32 color) {
    if (!terminal.boot_info) return;
    terminal.bg_color = color; 
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    usize total_pixels = terminal.boot_info->FramebufferSize / sizeof(u32);
    for (usize i = 0; i < total_pixels; i++) {
        fb[i] = color;
    }
    terminal.cursor_x = 10;
    terminal.cursor_y = 10;
}
