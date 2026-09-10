/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <bootinfo.h>
#include <terminal_colors.h>
#include <types.h>

void terminal_init(BOOT_INFO *boot_info);

/* Feeds one byte from the byte-oriented console ABI into the stateful UTF-8
 * output decoder. */
void terminal_putc(char c);
void terminal_put_codepoint(u32 codepoint);
void terminal_write(const char *str);
/* Write a complete console buffer while keeping terminal state serialized.
 * Ordinary writes are presented by the bounded presentation worker. */
void terminal_write_bytes(const char *buffer, u64 length);

void terminal_clear(void);

void terminal_palette_set_foreground(terminal_color_t color);
void terminal_palette_set_background(terminal_color_t color);
void terminal_palette_set_colors(terminal_color_t foreground,
                                 terminal_color_t background);
void terminal_palette_reset_colors(void);

void terminal_cursor_show(void);
void terminal_cursor_hide(void);

void terminal_cursor_enable(void);
void terminal_cursor_disable(void);

/* Timer IRQs only mark a blink due; call this from normal kernel context to
 * perform the small cursor-cell redraw safely. */
void terminal_cursor_blink_timer_tick(void);
void terminal_cursor_blink_poll(void);
/* Start the scheduler-backed normal-context blink worker after the scheduler
 * has been initialized.  Timer IRQs never render the framebuffer directly. */
bool terminal_cursor_blink_start(void);
/* Start the bounded normal-console presentation worker. */
bool terminal_presentation_start(void);

void terminal_redraw(void);

/* Batch rendering: defer VRAM flush until terminal_end_batch(). */
void terminal_begin_batch(void);
void terminal_end_batch(void);
void terminal_force_end_batch(void);

/* Process-bound native full-screen terminal controls.  A zero process ID is
 * reserved for kernel callers and cannot acquire the alternate screen. */
bool terminal_alternate_enter_process(u64 process_id);
bool terminal_alternate_leave_process(u64 process_id);
bool terminal_alternate_abort(void);
bool terminal_process_controls(u64 process_id);
bool terminal_process_output_allowed(u64 process_id);
bool terminal_process_input_allowed(u64 process_id);
bool terminal_move_cursor(u32 row, u32 column);
bool terminal_set_cursor_visible(bool visible);
bool terminal_clear_current_line(void);
bool terminal_clear_current_to_end(void);
bool terminal_clear_cells(u32 first_row, u32 first_column,
                          u32 last_row, u32 last_column);
void terminal_get_dimensions(u32 *rows, u32 *columns);
void terminal_get_capability_mask(u32 *capabilities);
typedef struct {
    u32 codepoint;
    u8 foreground;
    u8 background;
    u16 reserved;
} terminal_overlay_cell_t;
bool terminal_overlay_set_for_process(u64 process_id, u32 rows, u32 columns,
                                      const terminal_overlay_cell_t *cells);
bool terminal_overlay_clear_for_process(u64 process_id);
i64 terminal_set_raw_input_for_process(u64 process_id, bool enabled);
i64 terminal_read_key_for_process(u64 process_id, u32 timeout_ms, u32 *key);
bool terminal_begin_batch_for_process(u64 process_id);
bool terminal_end_batch_for_process(u64 process_id);
i64 terminal_write_styled_for_process(u64 process_id, const char *buffer,
                                       u64 length, terminal_color_t foreground,
                                       terminal_color_t background);
i64 terminal_write_semantic_for_process(u64 process_id, const char *buffer,
                                         u64 length,
                                         terminal_style_role_t role);

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
