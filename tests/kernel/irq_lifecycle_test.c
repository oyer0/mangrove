/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#include <irq.h>
#include <idt.h>
#include <spinlock.h>

#include "../../kernel/src/irq.c"

static _Atomic u32 entered_handlers;
static _Atomic u32 completed_handlers;
static _Atomic bool release_handlers;
static _Atomic bool unregister_done;
static struct cpu_registers irq_frame;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE))
        sched_yield();
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

static void blocked_handler(struct cpu_registers *regs)
{
    assert(regs == &irq_frame);
    atomic_fetch_add_explicit(&entered_handlers, 1U, memory_order_release);
    while (!atomic_load_explicit(&release_handlers, memory_order_acquire))
        sched_yield();
    atomic_fetch_add_explicit(&completed_handlers, 1U, memory_order_release);
}

static void replacement_handler(struct cpu_registers *regs)
{
    assert(regs == &irq_frame);
    atomic_fetch_add_explicit(&completed_handlers, 1U, memory_order_relaxed);
}

static void *dispatch_worker(void *unused)
{
    (void)unused;
    irq_dispatch(&irq_frame);
    return NULL;
}

static void *unregister_worker(void *unused)
{
    (void)unused;
    irq_unregister_vector((u8)irq_frame.vec_no);
    atomic_store_explicit(&unregister_done, true, memory_order_release);
    return NULL;
}

int main(void)
{
    pthread_t dispatchers[2];
    pthread_t unregister_thread;
    irq_frame.vec_no = IRQ_VECTOR_E1000;

    assert(irq_register_vector((u8)irq_frame.vec_no, blocked_handler));
    assert(irq_register_vector((u8)irq_frame.vec_no, blocked_handler));
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_create(&dispatchers[index], NULL, dispatch_worker,
                              NULL) == 0);
    while (atomic_load_explicit(&entered_handlers, memory_order_acquire) != 2U)
        sched_yield();

    assert(pthread_create(&unregister_thread, NULL, unregister_worker,
                          NULL) == 0);
    while (!irq_gate_is_closing(&irq_slots[IRQ_VECTOR_E1000].gate))
        sched_yield();
    assert((__atomic_load_n(&irq_slots[IRQ_VECTOR_E1000].gate.state,
                            __ATOMIC_ACQUIRE) & IRQ_GATE_REFS_MASK) == 2U);

    /* Closing admission must reject later dispatches while the admitted
     * callbacks retain access to their device-owned state. */
    irq_dispatch(&irq_frame);
    assert(atomic_load_explicit(&entered_handlers, memory_order_acquire) == 2U);
    assert(!atomic_load_explicit(&unregister_done, memory_order_acquire));

    atomic_store_explicit(&release_handlers, true, memory_order_release);
    for (u32 index = 0; index < 2U; index++)
        assert(pthread_join(dispatchers[index], NULL) == 0);
    assert(pthread_join(unregister_thread, NULL) == 0);
    assert(atomic_load_explicit(&completed_handlers, memory_order_acquire) ==
           2U);
    assert(atomic_load_explicit(&unregister_done, memory_order_acquire));
    assert(__atomic_load_n(&irq_slots[IRQ_VECTOR_E1000].gate.state,
                           __ATOMIC_ACQUIRE) == 0U);
    assert(__atomic_load_n(&irq_slots[IRQ_VECTOR_E1000].handler,
                           __ATOMIC_ACQUIRE) == NULL);

    assert(irq_register_vector((u8)irq_frame.vec_no, replacement_handler));
    irq_dispatch(&irq_frame);
    assert(atomic_load_explicit(&completed_handlers, memory_order_acquire) ==
           3U);
    irq_unregister_vector((u8)irq_frame.vec_no);

    irq_frame.vec_no = IRQ_VECTOR_TLB_SHOOTDOWN;
    assert(irq_register_permanent_vector((u8)irq_frame.vec_no,
                                          replacement_handler));
    assert(__atomic_load_n(&irq_slots[IRQ_VECTOR_TLB_SHOOTDOWN].gate.state,
                           __ATOMIC_ACQUIRE) == 0U);
    irq_dispatch(&irq_frame);
    irq_unregister_vector((u8)irq_frame.vec_no);
    irq_dispatch(&irq_frame);
    assert(atomic_load_explicit(&completed_handlers, memory_order_acquire) ==
           5U);

    puts("IRQ lifecycle: unregister blocks new dispatch and drains admitted handlers");
    return 0;
}
