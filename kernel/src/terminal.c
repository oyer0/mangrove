#include <terminal.h>
#include <font.h>
#include <framebuffer.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define TERMINAL_MARGIN_X 10
#define TERMINAL_MARGIN_Y 10
#define TERMINAL_LINE_SPACING 2

#define TERMINAL_FG_COLOR 0x0B6623
#define TERMINAL_BG_COLOR 0xFFFFFF

#define TERMINAL_BUFFER_ROWS 512
#define TERMINAL_MAX_COLS    256

#define TERMINAL_ESCAPE_NONE 0U
#define TERMINAL_ESCAPE_SEEN 1U
#define TERMINAL_ESCAPE_CSI  2U
#define TERMINAL_ESCAPE_SGR  3U

typedef struct {
    char ascii;
    u32 fg_color;
    u32 bg_color;
} terminal_cell_t;

typedef struct {
    terminal_cell_t cells[TERMINAL_BUFFER_ROWS * TERMINAL_MAX_COLS];

    u32 top_row_idx;        // Physical ring buffer index of top visible screen row
    u32 cursor_phys_row;    // Physical ring buffer index of current cursor row
    u32 cursor_col;         // Current cursor column (0 .. visible_cols - 1)

    u32 fg_color;
    u32 bg_color;

    u32 visible_cols;       // Visible columns count
    u32 visible_rows;       // Visible rows count
    u32 width;
    u32 height;

    bool cursor_visible;
    bool cursor_enabled;
    bool vram_valid;        // Indicates if VRAM cache is synchronized with ring buffer
    bool inverse_video;
    u8 escape_state;
    char escape_parameter;
} terminal_t;

static terminal_t terminal;

static void terminal_scroll(void);

static u32 terminal_line_height(void){
    return font_height() + TERMINAL_LINE_SPACING;
}

/* Centralized helper to map physical (row, col) to a cell pointer. Returns NULL on invalid index. */
static inline terminal_cell_t *terminal_cell_at(u32 phys_row, u32 col) {
    if (phys_row >= TERMINAL_BUFFER_ROWS || col >= terminal.visible_cols)
        return NULL;
    return &terminal.cells[(phys_row * terminal.visible_cols) + col];
}

static inline u32 phys_to_screen_row(u32 phys_row) {
    return (phys_row - terminal.top_row_idx + TERMINAL_BUFFER_ROWS) % TERMINAL_BUFFER_ROWS;
}

static inline u32 screen_to_phys_row(u32 screen_row) {
    return (terminal.top_row_idx + screen_row) % TERMINAL_BUFFER_ROWS;
}

static void terminal_clear_phys_row(u32 phys_row, u32 bg_color) {
    for (u32 c = 0; c < terminal.visible_cols; c++) {
        terminal_cell_t *cell = terminal_cell_at(phys_row, c);
        if (cell) {
            cell->ascii = ' ';
            cell->fg_color = terminal.fg_color;
            cell->bg_color = bg_color;
        }
    }
}

/* Canonical renderer: defines authoritative reference output for any terminal state */
void terminal_redraw(void) {
    for (u32 r = 0; r < terminal.visible_rows; r++) {
        u32 phys_r = screen_to_phys_row(r);
        u32 py = TERMINAL_MARGIN_Y + (r * terminal_line_height());

        for (u32 c = 0; c < terminal.visible_cols; c++) {
            terminal_cell_t *cell = terminal_cell_at(phys_r, c);
            u32 px = TERMINAL_MARGIN_X + (c * font_width());
            char ch = (cell && cell->ascii) ? cell->ascii : ' ';
            u32 fg = cell ? cell->fg_color : terminal.fg_color;
            u32 bg = cell ? cell->bg_color : terminal.bg_color;

            draw_char(ch, px, py, fg, bg);
        }
    }
    terminal.vram_valid = true;
}

void terminal_init(BOOT_INFO *BootInfo){
    (void)BootInfo;

    terminal.fg_color = TERMINAL_FG_COLOR;
    terminal.bg_color = TERMINAL_BG_COLOR;

    terminal.width  = BootInfo->FramebufferWidth;
    terminal.height = BootInfo->FramebufferHeight;

    terminal.visible_cols = (terminal.width - (TERMINAL_MARGIN_X * 2)) / font_width();
    if (terminal.visible_cols > TERMINAL_MAX_COLS) {
        terminal.visible_cols = TERMINAL_MAX_COLS;
    }

    u32 line_h = terminal_line_height();
    terminal.visible_rows = (terminal.height - (TERMINAL_MARGIN_Y * 2)) / line_h;

    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;

    terminal.cursor_visible = false;
    terminal.cursor_enabled = true;
    terminal.vram_valid = false;
    terminal.inverse_video = false;
    terminal.escape_state = TERMINAL_ESCAPE_NONE;
    terminal.escape_parameter = 0;

    for (u32 r = 0; r < TERMINAL_BUFFER_ROWS; r++) {
        terminal_clear_phys_row(r, terminal.bg_color);
    }

    terminal_clear();
}

void terminal_cursor_show(void){
    if (!terminal.cursor_enabled || terminal.cursor_visible)
        return;

    u32 srow = phys_to_screen_row(terminal.cursor_phys_row);
    if (srow >= terminal.visible_rows)
        return;

    u32 px = TERMINAL_MARGIN_X + (terminal.cursor_col * font_width());
    u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());

    terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row,
                                             terminal.cursor_col);
    if (cell && cell->ascii) {
        draw_char(cell->ascii, px, py, terminal.bg_color,
                  terminal.fg_color);
    } else {
        draw_char(' ', px, py, terminal.bg_color, terminal.fg_color);
    }
    terminal.cursor_visible = true;
}

void terminal_cursor_hide(void){
    if (!terminal.cursor_visible)
        return;

    u32 srow = phys_to_screen_row(terminal.cursor_phys_row);
    if (srow < terminal.visible_rows) {
        u32 px = TERMINAL_MARGIN_X + (terminal.cursor_col * font_width());
        u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());

        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, terminal.cursor_col);
        char ch = (cell && cell->ascii) ? cell->ascii : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;
        draw_char(ch, px, py, fg, bg);
    }

    terminal.cursor_visible = false;
}

static void terminal_scroll(void) {
    /* 1. Advance ring buffer indices in O(1) time */
    terminal.top_row_idx = (terminal.top_row_idx + 1) % TERMINAL_BUFFER_ROWS;
    terminal.cursor_phys_row = (terminal.top_row_idx + terminal.visible_rows - 1) % TERMINAL_BUFFER_ROWS;

    /* 2. Clear new bottom row in text ring buffer */
    terminal_clear_phys_row(terminal.cursor_phys_row, terminal.bg_color);

    /* Self-healing check: if VRAM cache is untrusted, fall back to canonical redraw */
    if (!terminal.vram_valid) {
        terminal_redraw();
        return;
    }

    /* 3. Fast-path VRAM shift: copy visible pixel scanlines UP by 1 line height */
    u32 line_h = terminal_line_height();
    u32 copy_height = (terminal.visible_rows - 1) * line_h;

    framebuffer_copy_rows(
        TERMINAL_MARGIN_Y,
        TERMINAL_MARGIN_Y + line_h,
        copy_height
    );

    /* 4. Clear newly exposed bottom VRAM scanline */
    framebuffer_fill_rows(
        TERMINAL_MARGIN_Y + copy_height,
        line_h,
        terminal.bg_color
    );

    /* 5. Render new bottom line cells onto VRAM */
    u32 bottom_py = TERMINAL_MARGIN_Y + copy_height;
    for (u32 c = 0; c < terminal.visible_cols; c++) {
        terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, c);
        u32 px = TERMINAL_MARGIN_X + (c * font_width());
        char ch = (cell && cell->ascii) ? cell->ascii : ' ';
        u32 fg = cell ? cell->fg_color : terminal.fg_color;
        u32 bg = cell ? cell->bg_color : terminal.bg_color;
        draw_char(ch, px, bottom_py, fg, bg);
    }
}

static void terminal_newline(void) {
    terminal.cursor_col = 0;

    u32 current_srow = phys_to_screen_row(terminal.cursor_phys_row);

    if (current_srow >= terminal.visible_rows - 1) {
        terminal_scroll();
    } else {
        terminal.cursor_phys_row = (terminal.cursor_phys_row + 1) % TERMINAL_BUFFER_ROWS;
    }
}

void terminal_putc(char c) {
    /* TEMPORARY: mirror to COM1 for headless QEMU testing */
    {
        static bool serial_init_done = false;
        if (!serial_init_done) {
            serial_init_done = true;
            __asm__ volatile("outb %0, %1" :: "a"((u8)0x00), "Nd"((u16)0x3F9)); /* Disable interrupts */
            __asm__ volatile("outb %0, %1" :: "a"((u8)0x80), "Nd"((u16)0x3FB)); /* Enable DLAB */
            __asm__ volatile("outb %0, %1" :: "a"((u8)0x01), "Nd"((u16)0x3F8)); /* Divisor lo: 115200 */
            __asm__ volatile("outb %0, %1" :: "a"((u8)0x00), "Nd"((u16)0x3F9)); /* Divisor hi */
            __asm__ volatile("outb %0, %1" :: "a"((u8)0x03), "Nd"((u16)0x3FB)); /* 8N1 */
            __asm__ volatile("outb %0, %1" :: "a"((u8)0xC7), "Nd"((u16)0x3FA)); /* Enable FIFO */
            __asm__ volatile("outb %0, %1" :: "a"((u8)0x00), "Nd"((u16)0x3FC)); /* No modem ctrl */
        }
        u8 lsr;
        do {
            __asm__ volatile("inb %1, %0" : "=a"(lsr) : "Nd"((u16)0x3FD));
        } while (!(lsr & 0x20));
        __asm__ volatile("outb %0, %1" :: "a"((u8)c), "Nd"((u16)0x3F8));
    }
    terminal_cursor_hide();

    if (terminal.escape_state == TERMINAL_ESCAPE_SEEN) {
        terminal.escape_state = c == '[' ? TERMINAL_ESCAPE_CSI
                                         : TERMINAL_ESCAPE_NONE;
        terminal_cursor_show();
        return;
    }
    if (terminal.escape_state == TERMINAL_ESCAPE_CSI) {
        if (c == '0' || c == '7') {
            terminal.escape_parameter = c;
            terminal.escape_state = TERMINAL_ESCAPE_SGR;
        } else {
            terminal.escape_state = TERMINAL_ESCAPE_NONE;
        }
        terminal_cursor_show();
        return;
    }
    if (terminal.escape_state == TERMINAL_ESCAPE_SGR) {
        if (c == 'm') {
            terminal.inverse_video = terminal.escape_parameter == '7';
        }
        terminal.escape_state = TERMINAL_ESCAPE_NONE;
        terminal_cursor_show();
        return;
    }
    if ((u8)c == 0x1b) {
        terminal.escape_state = TERMINAL_ESCAPE_SEEN;
        terminal_cursor_show();
        return;
    }

    if (c == '\r') {
        terminal.cursor_col = 0;
        terminal_cursor_show();
        return;
    }

    if (c == '\b') {
        if (terminal.cursor_col > 0) terminal.cursor_col--;
        terminal_cursor_show();
        return;
    }

    if (c == '\n') {
        terminal_newline();
        terminal_cursor_show();
        return;
    }

    if (terminal.cursor_col >= terminal.visible_cols) {
        terminal_newline();
    }

    /* Track in text buffer */
    terminal_cell_t *cell = terminal_cell_at(terminal.cursor_phys_row, terminal.cursor_col);
    if (cell) {
        cell->ascii = c;
        cell->fg_color = terminal.inverse_video ? terminal.bg_color
                                                : terminal.fg_color;
        cell->bg_color = terminal.inverse_video ? terminal.fg_color
                                                : terminal.bg_color;
    }

    u32 srow = phys_to_screen_row(terminal.cursor_phys_row);
    if (srow < terminal.visible_rows) {
        u32 px = TERMINAL_MARGIN_X + (terminal.cursor_col * font_width());
        u32 py = TERMINAL_MARGIN_Y + (srow * terminal_line_height());
        draw_char(c, px, py, cell ? cell->fg_color : terminal.fg_color,
                  cell ? cell->bg_color : terminal.bg_color);
    }

    terminal.cursor_col++;
    terminal_cursor_show();
}

void terminal_write(const char *str) {
    while (*str) {
        terminal_putc(*str++);
    }
}

void terminal_clear(void) {
    terminal_cursor_hide();

    terminal.top_row_idx = 0;
    terminal.cursor_phys_row = 0;
    terminal.cursor_col = 0;
    terminal.inverse_video = false;
    terminal.escape_state = TERMINAL_ESCAPE_NONE;

    for (u32 r = 0; r < TERMINAL_BUFFER_ROWS; r++) {
        terminal_clear_phys_row(r, terminal.bg_color);
    }

    framebuffer_clear(terminal.bg_color);
    terminal.vram_valid = true;
    terminal.cursor_visible = false;
    terminal_cursor_show();
}

void terminal_set_color(u32 color) {
    terminal.fg_color = color;
}

void terminal_set_background(u32 color) {
    terminal.bg_color = color;
}

void terminal_cursor_enable(void) {
    terminal.cursor_enabled = true;
}

void terminal_cursor_disable(void) {
    terminal_cursor_hide();
    terminal.cursor_enabled = false;
}
