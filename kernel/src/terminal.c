#include <terminal.h>
#include <font.h>

#include <stdarg.h>

#define TERMINAL_MARGIN_X   10 
#define TERMINAL_MARGIN_Y   10 

#define TERMINAL_LINE_SPACING 4
#define TERMINAL_CURSOR_HEIGHT 2

#define TERMINAL_DEFAULT_FG 0x0B6623
#define TERMINAL_DEFAULT_BG 0xFFFFFFFF

typedef struct terminal {
    u32 cursor_x;
    u32 cursor_y;
    u32 fg_color;
    u32 bg_color;
    BOOT_INFO *boot_info;
} terminal_t;

/* Global state for the kernel text terminal. */
static terminal_t terminal;

static void itoa(u64 value, char *str, u32 base) {
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
    /* TODO: Expose font metrics through font.c instead of reading the PSF header directly. */
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 line_height = font_header->height + TERMINAL_LINE_SPACING;
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    u32 width = terminal.boot_info->FramebufferWidth;
    u32 height = terminal.boot_info->FramebufferHeight;
    u32 scanline = terminal.boot_info->PixelsPerScanLine;

    for (u32 y = TERMINAL_MARGIN_Y; y < height - line_height - TERMINAL_MARGIN_Y; y++) {
        for (u32 x = TERMINAL_MARGIN_X; x < width - TERMINAL_MARGIN_X; x++) {
            fb[y * scanline + x] = fb[(y + line_height) * scanline + x];
        }
    }

    for (u32 y = height - line_height - TERMINAL_MARGIN_Y; y < height - TERMINAL_MARGIN_Y; y++) {
        for (u32 x = TERMINAL_MARGIN_X; x < width - TERMINAL_MARGIN_X; x++) {
            fb[y * scanline + x] = terminal.bg_color;
        }
    }

    terminal.cursor_y -= line_height;
}

static void clear_cursor(void) {
    if (!terminal.boot_info)
        return;

    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    u32 scanline = terminal.boot_info->PixelsPerScanLine;

    for (u32 py = 0; py < font_header->height; py++) {
        for (u32 px = 0; px < font_header->width; px++) {
            usize fb_index =
                (terminal.cursor_y + py) * scanline +
                (terminal.cursor_x + px);

            fb[fb_index] = terminal.bg_color;
        }
    }
}

static void terminal_newline(void) {
    psf2_header_t *font_header =
        (psf2_header_t *)_binary_src_font_psf_start;

    u32 line_height = font_header->height + TERMINAL_LINE_SPACING;

    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y += line_height;

    if (terminal.cursor_y + line_height >=
        terminal.boot_info->FramebufferHeight - TERMINAL_MARGIN_Y) {
        terminal_scroll();
    }
}

static void draw_cursor(void) {
    if (!terminal.boot_info)
        return;

    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;
    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    u32 scanline = terminal.boot_info->PixelsPerScanLine;

    for (u32 py = font_header->height - TERMINAL_CURSOR_HEIGHT;
         py < font_header->height;
         py++) {

        for (u32 px = 0; px < font_header->width; px++) {
            usize fb_index =
                (terminal.cursor_y + py) * scanline +
                (terminal.cursor_x + px);

            fb[fb_index] = terminal.fg_color;
        }
    }
}

void terminal_putc(char c) {
    psf2_header_t *font_header = (psf2_header_t *)_binary_src_font_psf_start;

    clear_cursor();

    if (c == '\b') {
        if (terminal.cursor_x > TERMINAL_MARGIN_X) {
            terminal.cursor_x -= font_header->width;

            draw_char(
                ' ',
                terminal.cursor_x,
                terminal.cursor_y,
                terminal.fg_color,
                terminal.bg_color,
                terminal.boot_info
            );
        }

        draw_cursor();
        return;
    }

    if (c == '\n') {
        terminal_newline();
        draw_cursor();
        return;
    }

    if (terminal.cursor_x + font_header->width >=
        terminal.boot_info->FramebufferWidth - TERMINAL_MARGIN_X) {

        terminal_newline();
    }

    draw_char(c, terminal.cursor_x, terminal.cursor_y,
        terminal.fg_color, terminal.bg_color, terminal.boot_info);

    terminal.cursor_x += font_header->width;

    draw_cursor();
}

void terminal_init(BOOT_INFO *BootInfo) {
    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y = TERMINAL_MARGIN_Y;
    terminal.fg_color = TERMINAL_DEFAULT_FG;
    terminal.bg_color = TERMINAL_DEFAULT_BG;
    terminal.boot_info = BootInfo;
}

void kprint(const char *format, ...) {
    if (!terminal.boot_info)
        return;

    va_list args;
    va_start(args, format);

    char buf[64];

    while (*format) {
        if (*format == '%') {
            format++;

            switch (*format) {
            case 's': {
                char *s = va_arg(args, char *);

                while (*s)
                    terminal_putc(*s++);

                break;
            }

            case 'd': {
                i64 d = va_arg(args, i64);

                if (d < 0) {
                    terminal_putc('-');
                    d = -d;
                }

                itoa((u64)d, buf, 10);

                for (int i = 0; buf[i]; i++)
                    terminal_putc(buf[i]);

                break;
            }

            case 'u': {
                u64 u = va_arg(args, u64);

                itoa(u, buf, 10);

                for (int i = 0; buf[i]; i++)
                    terminal_putc(buf[i]);

                break;
            }

            case 'x': {
                u64 x = va_arg(args, u64);

                terminal_putc('0');
                terminal_putc('x');

                itoa(x, buf, 16);

                for (int i = 0; buf[i]; i++)
                    terminal_putc(buf[i]);

                break;
            }

            case 'p': {
                u64 p = (u64)va_arg(args, void *);

                terminal_putc('0');
                terminal_putc('x');

                itoa(p, buf, 16);

                int len = 0;
                while (buf[len])
                    len++;

                for (int i = 0; i < 16 - len; i++)
                    terminal_putc('0');

                for (int i = 0; buf[i]; i++)
                    terminal_putc(buf[i]);

                break;
            }

            case 'c':
                terminal_putc((char)va_arg(args, int));
                break;

            case '%':
                terminal_putc('%');
                break;
            }

        } else {
            terminal_putc(*format);
        }

        format++;
    }

    va_end(args);
}

void kprint_centered(const char *text) {
    terminal.cursor_x = TERMINAL_MARGIN_X;

    int len = 0;

    while (text[len])
        len++;

    const int banner_width = 55;
    int left = (banner_width - len) / 2;

    for (int i = 0; i < left; i++)
        terminal_putc(' ');

    while (*text)
        terminal_putc(*text++);
}

void kprint_set_color(u32 color) {
    terminal.fg_color = color;
}

void kprint_clear_screen(u32 color) {
    if (!terminal.boot_info)
        return;

    terminal.bg_color = color;

    u32 *fb = (u32 *)terminal.boot_info->FramebufferBase;
    usize total_pixels =
        terminal.boot_info->FramebufferSize / sizeof(u32);

    for (usize i = 0; i < total_pixels; i++)
        fb[i] = color;

    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y = TERMINAL_MARGIN_Y;
    draw_cursor();
}
