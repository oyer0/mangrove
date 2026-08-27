#pragma once


#include <types.h>

struct cpu_registers;

typedef void (*irq_handler_t)(struct cpu_registers *);

#define IRQ_VECTOR_PIT       0x20U
#define IRQ_VECTOR_PS2      0x21U
#define IRQ_VECTOR_XHCI     0x22U
#define IRQ_VECTOR_RTL8168  0x23U
#define IRQ_VECTOR_E1000    0x24U
#define IRQ_VECTOR_ACPI_SCI 0x25U
#define IRQ_VECTOR_FIRST    0x20U
#define IRQ_VECTOR_LAST     0x2FU

bool irq_register_vector(u8 vector, irq_handler_t handler);
void irq_unregister_vector(u8 vector);
void irq_dispatch(struct cpu_registers *regs);
