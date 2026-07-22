#include <terminal.h>
#include <font.h>

#define TERMINAL_MARGIN_X 10
#define TERMINAL_MARGIN_Y 10

typedef struct {
    u32 cursor_x;
    u32 cursor_y;

    u32 fg_color;
    u32 bg_color;

    u32 width;
    u32 height;
} terminal_t;

static terminal_t terminal;

void terminal_init(BOOT_INFO *BootInfo)
{
    (void)BootInfo;

    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y = TERMINAL_MARGIN_Y;

    terminal.fg_color = 0x4F9D69;
    terminal.bg_color = 0xFFFFFF;

    terminal.width  = BootInfo->FramebufferWidth;
    terminal.height = BootInfo->FramebufferHeight;
}

static void terminal_newline(void)
{
    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y += font_height();
}

void terminal_putc(char c)
{
    if (c == '\n') {
        terminal_newline();
        return;
    }

    if (terminal.cursor_x + font_width() >
        terminal.width - TERMINAL_MARGIN_X) {
        terminal_newline();
    }

    draw_char(c, terminal.cursor_x, terminal.cursor_y,
        terminal.fg_color, terminal.bg_color);

    terminal.cursor_x += font_width();
}
