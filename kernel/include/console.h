#pragma once

#include <types.h>

void console_init(void);
void console_input(char c);
u64 console_read_bytes(void *buffer, u64 length);
