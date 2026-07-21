#ifndef TERMINAL_H
#define TERMINAL_H

#include <bootinfo.h>
#include <types.h>

void terminal_init(BOOT_INFO *BootInfo);

void kprint(const char *format, ...);
void kprint_centered(const char *text);

void kprint_set_color(u32 color);
void kprint_clear_screen(u32 color);

void terminal_putc(char c);

#endif
