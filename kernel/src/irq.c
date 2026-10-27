/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <irq.h>
#include <idt.h>
#include <stddef.h>
#include <spinlock.h>

typedef struct {
    irq_handler_t handler;
    irq_gate_t gate;
    volatile u32 permanent;
} irq_slot_t;

static irq_slot_t irq_slots[256];
static spinlock_t irq_registry_lock = { .value = 0 };

bool irq_register_vector(u8 vector, irq_handler_t handler)
{
    irq_slot_t *slot;
    u64 flags;

    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || !handler)
        return false;

    slot = &irq_slots[vector];
    flags = spin_lock_irqsave(&irq_registry_lock);
    if (__atomic_load_n(&slot->permanent, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return false;
    }
    if ((__atomic_load_n(&slot->gate.state, __ATOMIC_ACQUIRE) &
         IRQ_GATE_OPEN) &&
        __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) == handler) {
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return true;
    }
    if (__atomic_load_n(&slot->gate.state, __ATOMIC_ACQUIRE) != 0) {
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return false;
    }

    __atomic_store_n(&slot->handler, handler, __ATOMIC_RELAXED);
    if (!irq_gate_open(&slot->gate)) {
        __atomic_store_n(&slot->handler, NULL, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return false;
    }
    spin_unlock_irqrestore(&irq_registry_lock, flags);
    return true;
}

bool irq_register_permanent_vector(u8 vector, irq_handler_t handler)
{
    irq_slot_t *slot;
    u64 flags;

    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || !handler)
        return false;

    slot = &irq_slots[vector];
    flags = spin_lock_irqsave(&irq_registry_lock);
    if (__atomic_load_n(&slot->permanent, __ATOMIC_ACQUIRE)) {
        bool same = __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) == handler;
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return same;
    }
    if (__atomic_load_n(&slot->gate.state, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) != NULL) {
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return false;
    }

    __atomic_store_n(&slot->handler, handler, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->permanent, 1U, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&irq_registry_lock, flags);
    return true;
}

void irq_unregister_vector(u8 vector)
{
    irq_slot_t *slot;
    u64 flags;

    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST)
        return;

    slot = &irq_slots[vector];
    flags = spin_lock_irqsave(&irq_registry_lock);
    if (__atomic_load_n(&slot->permanent, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        return;
    }
    if (!irq_gate_begin_close(&slot->gate)) {
        bool closing = irq_gate_is_closing(&slot->gate);
        spin_unlock_irqrestore(&irq_registry_lock, flags);
        if (closing) {
            while (irq_gate_is_closing(&slot->gate))
                __asm__ volatile("pause" ::: "memory");
        }
        return;
    }

    /* Do not retain the registry lock while waiting: handlers never take it. */
    spin_unlock_irqrestore(&irq_registry_lock, flags);

    irq_gate_wait_drained(&slot->gate);

    flags = spin_lock_irqsave(&irq_registry_lock);
    if (irq_gate_is_drained(&slot->gate)) {
        __atomic_store_n(&slot->handler, NULL, __ATOMIC_RELAXED);
        irq_gate_reset(&slot->gate);
    }
    spin_unlock_irqrestore(&irq_registry_lock, flags);
}

void irq_dispatch(struct cpu_registers *regs)
{
    if (!regs || regs->vec_no > 0xFFU)
        return;

    if (regs->vec_no >= IRQ_VECTOR_FIRST && regs->vec_no <= IRQ_VECTOR_LAST) {
        irq_slot_t *slot = &irq_slots[regs->vec_no];
        if (__atomic_load_n(&slot->permanent, __ATOMIC_ACQUIRE)) {
            irq_handler_t handler =
                __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE);
            if (handler)
                handler(regs);
        } else if (irq_gate_enter(&slot->gate)) {
            irq_handler_t handler =
                __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE);
            if (handler)
                handler(regs);
            irq_gate_leave(&slot->gate);
        }
    }
}
