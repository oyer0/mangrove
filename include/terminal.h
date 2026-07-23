#ifndef TERMINAL_H
#define TERMINAL_H

#include <bootinfo.h>
#include <types.h>

void terminal_init(BOOT_INFO *boot_info);

void terminal_putc(char c);
void terminal_write(const char *str);

void terminal_backspace(void);

#endif
