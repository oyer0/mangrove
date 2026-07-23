#include <terminal.h>
#include <font.h>
#include <framebuffer.h>
#include <stdbool.h>

#define TERMINAL_MARGIN_X 10
#define TERMINAL_MARGIN_Y 10
#define TERMINAL_LINE_SPACING 2

#define TERMINAL_FG_COLOR 0x0B6623
#define TERMINAL_BG_COLOR 0xFFFFFF

static void terminal_scroll(void);

typedef struct {
    u32 cursor_x;
    u32 cursor_y;

    u32 fg_color;
    u32 bg_color;

    u32 width;
    u32 height;

    bool cursor_visible;
} terminal_t;
static terminal_t terminal;

static u32 terminal_line_height(void){
    return font_height() + TERMINAL_LINE_SPACING;
}

void terminal_init(BOOT_INFO *BootInfo){
    (void)BootInfo;

    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y = TERMINAL_MARGIN_Y;

    terminal.fg_color = TERMINAL_FG_COLOR;
    terminal.bg_color = TERMINAL_BG_COLOR;

    terminal.width  = BootInfo->FramebufferWidth;
    terminal.height = BootInfo->FramebufferHeight;

    terminal.cursor_visible = false;
}

void terminal_cursor_show(void){
    if (terminal.cursor_visible)
        return;

    draw_char(
        '_',
        terminal.cursor_x,
        terminal.cursor_y,
        terminal.fg_color,
        terminal.bg_color
    );

    terminal.cursor_visible = true;
}

void terminal_cursor_hide(void){
    if (!terminal.cursor_visible)
        return;

    draw_char(
        ' ',
        terminal.cursor_x,
        terminal.cursor_y,
        terminal.fg_color,
        terminal.bg_color
    );

    terminal.cursor_visible = false;
}

static void terminal_newline(void) {
    terminal.cursor_x = TERMINAL_MARGIN_X;

    u32 line_height = terminal_line_height();
    
    u32 usable_height = terminal.height - (TERMINAL_MARGIN_Y * 2);
    u32 max_lines = usable_height / line_height;
    u32 bottom_cursor_y = TERMINAL_MARGIN_Y + ((max_lines - 1) * line_height);

    if (terminal.cursor_y >= bottom_cursor_y) {
        terminal_scroll();
        terminal.cursor_y = bottom_cursor_y;
    } else {
        terminal.cursor_y += line_height;
    }
}

static void terminal_scroll(void)
{
    u32 line_height = terminal_line_height();

    u32 usable_height = terminal.height - (TERMINAL_MARGIN_Y * 2);
    u32 max_lines = usable_height / line_height;
    u32 grid_height = max_lines * line_height;

    u32 first_text_row = TERMINAL_MARGIN_Y;
    u32 copy_height = grid_height - line_height;

    framebuffer_copy_rows(
        first_text_row,               
        first_text_row + line_height,
        copy_height
    );

    framebuffer_fill_rows(
        first_text_row + copy_height,
        line_height,
        terminal.bg_color
    );
}

void terminal_putc(char c) {
    terminal_cursor_hide();

    if (c == '\n') {
        terminal_newline();
        terminal_cursor_show();
        return;
    }

    if (terminal.cursor_x + font_width() >
        terminal.width - TERMINAL_MARGIN_X) {
        terminal_newline();
    }

    draw_char(c, terminal.cursor_x, terminal.cursor_y,
        terminal.fg_color, terminal.bg_color);

    terminal.cursor_x += font_width();
    terminal_cursor_show();
}

void terminal_write(const char *str) {
    while (*str) {
        terminal_putc(*str++);
    }
}

void terminal_backspace(void)
{
    terminal_cursor_hide();

    if (terminal.cursor_x <= TERMINAL_MARGIN_X) {
        terminal_cursor_show();
        return;
    }

    terminal.cursor_x -= font_width();

    draw_char(
        ' ',
        terminal.cursor_x,
        terminal.cursor_y,
        terminal.fg_color,
        terminal.bg_color
    );

    terminal_cursor_show();
}

void terminal_clear(void){
    framebuffer_clear(terminal.bg_color);

    terminal.cursor_x = TERMINAL_MARGIN_X;
    terminal.cursor_y = TERMINAL_MARGIN_Y;
}
