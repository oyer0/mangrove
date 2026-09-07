#pragma once

#include <bootinfo.h>
#include <types.h>

void terminal_init(BOOT_INFO *boot_info);

/* Feeds one byte from the byte-oriented console ABI into the stateful UTF-8
 * output decoder. */
void terminal_putc(char c);
void terminal_put_codepoint(u32 codepoint);
void terminal_write(const char *str);

void terminal_clear(void);

void terminal_set_color(u32 color);
void terminal_set_background(u32 color);

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
i64 terminal_set_raw_input_for_process(u64 process_id, bool enabled);
i64 terminal_read_key_for_process(u64 process_id, u32 timeout_ms, u32 *key);
bool terminal_begin_batch_for_process(u64 process_id);
bool terminal_end_batch_for_process(u64 process_id);

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
