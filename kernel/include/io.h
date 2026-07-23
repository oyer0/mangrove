#pragma once

#include <types.h>

u8 inb(u16 port);
void outb(u16 port, u8 value);

u32 inl(u16 port);
void outl(u16 port, u32 value);

void io_wait(void);
