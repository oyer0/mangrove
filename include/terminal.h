#pragma once

#include <bootinfo.h>
#include <types.h>

void terminal_init(BOOT_INFO *boot_info);

void terminal_putc(char c);
void terminal_write(const char *str);

void terminal_backspace(void);

void terminal_clear(void);

void terminal_set_color(u32 color);
void terminal_set_background(u32 color);

void terminal_cursor_show(void);
void terminal_cursor_hide(void);
