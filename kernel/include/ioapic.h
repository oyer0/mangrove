#pragma once

#include <types.h>

#define IOAPIC_ID        0x00
#define IOAPIC_VERSION   0x01
#define IOAPIC_ARB       0x02

#define IOAPIC_REDTBL    0x10

bool ioapic_present(void);

void ioapic_init(void);

u32 ioapic_read(u8 reg);
void ioapic_write(u8 reg, u32 value);

u64 ioapic_read_redirection(u8 irq);
void ioapic_write_redirection(u8 irq, u64 value);

void ioapic_route_irq(u8 irq, u8 vector, u8 apic_id);
