#include <irq.h>
#include <idt.h>

static irq_handler_t irq_handlers[16] = {0};

void irq_register_handler(u8 irq, irq_handler_t handler)
{
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void irq_unregister_handler(u8 irq)
{
    if (irq < 16) {
        irq_handlers[irq] = 0;
    }
}

void irq_dispatch(struct cpu_registers *regs)
{
    u8 irq = (u8)(regs->vec_no - 32);

    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    }
}
