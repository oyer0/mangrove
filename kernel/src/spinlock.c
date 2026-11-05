/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <spinlock.h>
#include <cpu_relax.h>
#include <cpu.h>
#include <panic.h>

#if defined(SCHEDULER_CONTENTION_GUEST_TEST)
static spinlock_t *contention_observed_lock;
static volatile u64 *contention_observed_acquisitions;
static volatile u64 *contention_observed_wait_iterations;

void spinlock_observe_irqsave_contention(spinlock_t *lock,
                                         volatile u64 *acquisitions,
                                         volatile u64 *wait_iterations)
{
    contention_observed_lock = lock;
    contention_observed_acquisitions = acquisitions;
    contention_observed_wait_iterations = wait_iterations;
}
#endif

static void spinlock_depth_enter(void)
{
    cpu_local_t *cpu = cpu_current();

    if (cpu && __atomic_add_fetch(&cpu->spinlock_depth, 1U,
                                  __ATOMIC_ACQ_REL) == 0U)
        panic("spinlock: held-depth overflow");
}

static void spinlock_depth_leave(void)
{
    cpu_local_t *cpu = cpu_current();

    if (cpu && __atomic_sub_fetch(&cpu->spinlock_depth, 1U,
                                  __ATOMIC_ACQ_REL) == ~(u32)0)
        panic("spinlock: held-depth underflow");
}

u32 spinlock_depth_current(void)
{
    cpu_local_t *cpu = cpu_current();

    if (!cpu)
        return ~(u32)0;
    return __atomic_load_n(&cpu->spinlock_depth, __ATOMIC_ACQUIRE);
}

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}

bool spin_try_lock(spinlock_t *lock)
{
    u32 expected = 0U;
    bool acquired;

    /* Count before the compare-exchange so an interrupt cannot preempt in
     * the gap between successful acquisition and publishing the held state.
     * A failed try immediately removes its temporary count. */
    spinlock_depth_enter();
    acquired = __atomic_compare_exchange_n(
        &lock->value, &expected, 1U, false, __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED);
    if (!acquired)
        spinlock_depth_leave();
    return acquired;
}

void spin_lock(spinlock_t *lock)
{
    for (;;) {
        while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED) != 0U)
            cpu_relax();

        if (spin_try_lock(lock))
            return;
        cpu_relax();
    }
}

void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
    spinlock_depth_leave();
}

static u64 spin_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void spin_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

u64 spin_lock_irqsave(spinlock_t *lock)
{
#if defined(SCHEDULER_CONTENTION_GUEST_TEST)
    if (lock == contention_observed_lock && lock)
        return spin_lock_irqsave_observe(lock,
            contention_observed_acquisitions,
            contention_observed_wait_iterations);
#endif
    u64 flags = spin_irq_save();

    spin_lock(lock);
    return flags;
}

#if defined(SCHEDULER_CONTENTION_GUEST_TEST)
u64 spin_lock_irqsave_observe(spinlock_t *lock,
                              volatile u64 *contended_acquisitions,
                              volatile u64 *wait_iterations)
{
    u64 flags = spin_irq_save();
    u64 iterations = 0;
    bool contended = false;

    for (;;) {
        while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED) != 0U) {
            contended = true;
            if (iterations != ~(u64)0)
                iterations++;
            cpu_relax();
        }

        if (spin_try_lock(lock))
            break;

        contended = true;
        if (iterations != ~(u64)0)
            iterations++;
        cpu_relax();
    }

    if (contended) {
        if (contended_acquisitions)
            __atomic_add_fetch(contended_acquisitions, 1U, __ATOMIC_RELAXED);
        if (wait_iterations)
            __atomic_add_fetch(wait_iterations, iterations, __ATOMIC_RELAXED);
    }
    return flags;
}
#endif

bool spin_try_lock_irqsave(spinlock_t *lock, u64 *flags_out)
{
    u64 flags;

    if (!lock || !flags_out)
        return false;
    flags = spin_irq_save();
    if (!spin_try_lock(lock)) {
        spin_irq_restore(flags);
        return false;
    }
    *flags_out = flags;
    return true;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    spin_unlock(lock);
    spin_irq_restore(flags);
}
