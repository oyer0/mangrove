#include <irq.h>
#include <idt.h>

static irq_handler_t irq_handlers[256] = {0};

bool irq_register_vector(u8 vector, irq_handler_t handler)
{
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || !handler)
        return false;
    if (irq_handlers[vector] && irq_handlers[vector] != handler)
        return false;
    irq_handlers[vector] = handler;
    return true;
}

void irq_unregister_vector(u8 vector)
{
    if (vector >= IRQ_VECTOR_FIRST && vector <= IRQ_VECTOR_LAST)
        irq_handlers[vector] = 0;
}

void irq_dispatch(struct cpu_registers *regs)
{
    if (!regs || regs->vec_no > 0xFFU)
        return;

    if (regs->vec_no >= IRQ_VECTOR_FIRST &&
        regs->vec_no <= IRQ_VECTOR_LAST && irq_handlers[regs->vec_no])
        irq_handlers[regs->vec_no](regs);
}
