#ifndef IRQ_H
#define IRQ_H

#include <types.h>

struct cpu_registers;

typedef void (*irq_handler_t)(struct cpu_registers *);

void irq_register_handler(u8 irq, irq_handler_t handler);
void irq_unregister_handler(u8 irq);
void irq_dispatch(struct cpu_registers *regs);

#endif
