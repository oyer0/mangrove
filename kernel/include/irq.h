/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once


#include <types.h>

struct cpu_registers;

typedef void (*irq_handler_t)(struct cpu_registers *);

/* A lock-free admission/drain gate for state used by an IRQ callback. Owners
 * close admission, then wait for the admitted references to leave before
 * releasing callback-visible state. Reset only after the gate has drained. */
#define IRQ_GATE_OPEN      (1U << 31)
#define IRQ_GATE_CLOSING   (1U << 30)
#define IRQ_GATE_REFS_MASK (IRQ_GATE_CLOSING - 1U)

typedef struct {
    volatile u32 state;
} irq_gate_t;

static inline bool irq_gate_enter(irq_gate_t *gate)
{
    u32 state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    while (state & IRQ_GATE_OPEN) {
        if ((state & IRQ_GATE_REFS_MASK) == IRQ_GATE_REFS_MASK)
            return false;
        if (__atomic_compare_exchange_n(&gate->state, &state, state + 1U,
                                        true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

static inline void irq_gate_leave(irq_gate_t *gate)
{
    __atomic_fetch_sub(&gate->state, 1U, __ATOMIC_RELEASE);
}

static inline bool irq_gate_open(irq_gate_t *gate)
{
    u32 expected = 0;
    return __atomic_compare_exchange_n(&gate->state, &expected, IRQ_GATE_OPEN,
                                       false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
}

static inline bool irq_gate_begin_close(irq_gate_t *gate)
{
    u32 state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    for (;;) {
        u32 closing;
        if (state & IRQ_GATE_CLOSING)
            return false;
        if (!(state & IRQ_GATE_OPEN))
            return false;
        closing = IRQ_GATE_CLOSING | (state & IRQ_GATE_REFS_MASK);
        if (__atomic_compare_exchange_n(&gate->state, &state, closing, true,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
    }
}

static inline bool irq_gate_is_closing(const irq_gate_t *gate)
{
    return (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) &
            IRQ_GATE_CLOSING) != 0;
}

static inline bool irq_gate_is_drained(const irq_gate_t *gate)
{
    u32 state = __atomic_load_n(&gate->state, __ATOMIC_ACQUIRE);
    return (state & IRQ_GATE_CLOSING) &&
           !(state & IRQ_GATE_REFS_MASK);
}

static inline void irq_gate_wait_drained(const irq_gate_t *gate)
{
    while (!irq_gate_is_drained(gate))
        __asm__ volatile("pause" ::: "memory");
}

static inline void irq_gate_reset(irq_gate_t *gate)
{
    __atomic_store_n(&gate->state, 0, __ATOMIC_RELEASE);
}

#define IRQ_VECTOR_PIT       0x20U
#define IRQ_VECTOR_PS2      0x21U
#define IRQ_VECTOR_XHCI     0x22U
#define IRQ_VECTOR_RTL8168  0x23U
#define IRQ_VECTOR_E1000    0x24U
#define IRQ_VECTOR_ACPI_SCI 0x25U
#define IRQ_VECTOR_LAPIC_TIMER 0x26U
#define IRQ_VECTOR_TLB_SHOOTDOWN 0x30U
#define IRQ_VECTOR_FIRST    0x20U
#define IRQ_VECTOR_LAST     IRQ_VECTOR_TLB_SHOOTDOWN

bool irq_register_vector(u8 vector, irq_handler_t handler);
/* Permanently registers a handler that will not be unregistered. Dispatches
 * do not take a reference gate, keeping high-frequency vectors uncontended. */
bool irq_register_permanent_vector(u8 vector, irq_handler_t handler);
/* Closes dispatch admission and waits for handlers already admitted to
 * return. Call from thread/initialization context, never from an IRQ handler.
 * Applies only to handlers registered with irq_register_vector(). Device
 * drivers must mask the device source before unregistering. */
void irq_unregister_vector(u8 vector);
void irq_dispatch(struct cpu_registers *regs);
