#pragma once

#include <bootinfo.h>
#include <types.h>

void terminal_init(BOOT_INFO *boot_info);

void terminal_putc(char c);
void terminal_write(const char *str);

void terminal_clear(void);

void terminal_set_color(u32 color);
void terminal_set_background(u32 color);

void terminal_cursor_show(void);
void terminal_cursor_hide(void);

void terminal_cursor_enable(void);
void terminal_cursor_disable(void);

void terminal_redraw(void);

/* Batch rendering: defer VRAM flush until terminal_end_batch(). */
void terminal_begin_batch(void);
void terminal_end_batch(void);

typedef struct {
    u64 batch_count;
    u64 full_redraw_count;
    u64 glyph_render_count;
    u64 ram_bytes_shifted;
    u64 vram_flush_count;
    u64 vram_bytes_copied;
} terminal_stats_t;

void terminal_get_stats(terminal_stats_t *out_stats);
void terminal_reset_stats(void);
