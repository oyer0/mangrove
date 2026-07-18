// console.h

#pragma once

#include <uefi.h>

void console_init(EFI_SYSTEM_TABLE *SystemTable);
void console_write(const CHAR16 *String);
void console_clear(void);
void console_set_cursor(usize Column, usize Row);
void console_enable_cursor(u8 Visible);
void console_set_attribute(usize Attribute);

typedef enum
{
    CONSOLE_BLACK = 0x00,
    CONSOLE_BLUE = 0x01,
    CONSOLE_GREEN = 0x02,
    CONSOLE_CYAN = 0x03,
    CONSOLE_RED = 0x04,
    CONSOLE_MAGENTA = 0x05,
    CONSOLE_BROWN = 0x06,
    CONSOLE_LIGHT_GRAY = 0x07,
    
    CONSOLE_DARK_GRAY = 0x08,
    CONSOLE_LIGHT_BLUE = 0x09,
    CONSOLE_LIGHT_GREEN = 0x0A,
    CONSOLE_LIGHT_CYAN = 0x0B,
    CONSOLE_LIGHT_RED = 0x0C,
    CONSOLE_LIGHT_MAGENTA = 0x0D,
    CONSOLE_YELLOW = 0x0E,
    CONSOLE_WHITE = 0x0F,
} console_color_t;
