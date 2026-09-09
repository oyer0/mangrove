/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <lapic.h>
#include <acpi.h>
#include <stddef.h>
#include <msr.h>
#include <vmm.h>
#include <kprint.h>
#include <cpu_relax.h>
#include <timer.h>
#include <cpu.h>
#include <irq.h>

#define LAPIC_TIMER_DIVIDE_BY_16 0x3U
#define LAPIC_TIMER_PERIOD_US    1000ULL

static volatile u32 *lapic = NULL;
static bool present = false;
static bool enabled = false;

bool lapic_present(void)
{
    return present;
}

bool lapic_enabled(void)
{
    return enabled;
}

u8 lapic_current_id(void)
{
    return lapic ? (u8)(lapic_read(LAPIC_ID) >> 24) : 0;
}

void lapic_init(void)
{
    present = false;
    enabled = false;
    lapic = NULL;

    acpi_madt_t *madt = acpi_madt();

    if (!madt)
    {
        return;
    }

    lapic = (volatile u32 *)vmm_map_mmio(
        (phys_addr_t)madt->local_apic_address, 0x1000);
    if (!lapic) return;

    present = true;
}

u32 lapic_read(u32 reg)
{
    return lapic[reg / sizeof(u32)];
}

void lapic_write(u32 reg, u32 value)
{
    lapic[reg / sizeof(u32)] = value;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

void lapic_enable(void)
{
    if (!present)
    {
        return;
    }

    if (!acpi_set_bsp_apic_id(lapic_current_id())) {
        KERNEL_BOOT_DEBUG_LOG(
            "[ACPI] current LAPIC ID %u is absent from usable MADT CPUs\n",
            lapic_current_id());
    } else {
        KERNEL_BOOT_DEBUG_LOG(
            "[ACPI] BSP LAPIC ID %u identified in MADT topology\n",
            lapic_current_id());
    }

    u64 apic_base = rdmsr(0x1B);
    apic_base |= (1ULL << 11);
    wrmsr(0x1B, apic_base);

    u32 svr = lapic_read(LAPIC_SVR);
    svr |= (1 << 8);
    svr = (svr & ~0xFF);
    svr |= LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    lapic_write(LAPIC_TPR, 0);

    /* Clear any previous APIC error state */
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);
    
    /* Mask unused local interrupt sources */
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    
    /* We'll configure these properly later */
    lapic_write(LAPIC_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LINT1, LAPIC_LVT_MASKED);
    
    /* Don't generate APIC error interrupts yet */
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    enabled = true;
}

bool lapic_init_cpu(void)
{
    u64 apic_base;
    u32 svr;

    if (!present || !lapic)
        return false;

    apic_base = rdmsr(0x1B) | (1ULL << 11);
    wrmsr(0x1B, apic_base);

    svr = lapic_read(LAPIC_SVR);
    svr |= (1U << 8);
    svr = (svr & ~0xFFU) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    return true;
}

bool lapic_timer_active(void)
{
    cpu_local_t *cpu = cpu_current();

    return cpu && cpu->scheduler_timer_active;
}

bool lapic_timer_init_cpu(void)
{
    cpu_local_t *cpu = cpu_current();
    timer_monotonic_deadline_t deadline;
    u32 current;
    u32 ticks;

    if (!cpu || !present || !lapic || !enabled ||
        !timer_monotonic_deadline_start(&deadline, LAPIC_TIMER_PERIOD_US))
        return false;

    lapic_write(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFFU);
    while (!timer_monotonic_deadline_expired(&deadline))
        cpu_relax();

    current = lapic_read(LAPIC_TIMER_CURRENT);
    ticks = 0xFFFFFFFFU - current;
    if (ticks < 100U || ticks > 0x7FFFFFFFU)
        return false;

    lapic_write(LAPIC_TIMER_INITIAL, ticks);
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_LAPIC_TIMER | (1U << 17));
    cpu->scheduler_timer_active = true;
    KERNEL_BOOT_DEBUG_LOG("[SMP] CPU %u local scheduler timer=%u/%uus\n",
                          cpu->index, ticks,
                          (u32)LAPIC_TIMER_PERIOD_US);
    return true;
}

bool lapic_ipi_wait_idle(void)
{
    const u32 spin_limit = 10000000U;

    if (!present || !lapic)
        return false;
    for (u32 i = 0; i < spin_limit; i++) {
        if (!(lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS))
            return true;
        cpu_relax();
    }
    return false;
}

static bool lapic_send_ipi(u8 apic_id, u32 command)
{
    if (!lapic_ipi_wait_idle())
        return false;
    lapic_write(LAPIC_ICR_HIGH, (u32)apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, command);
    return lapic_ipi_wait_idle();
}

bool lapic_send_fixed_ipi(u8 apic_id, u8 vector)
{
    if (vector < 16U)
        return false;
    return lapic_send_ipi(apic_id, vector);
}

bool lapic_send_init_ipi(u8 apic_id)
{
    if (!lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_INIT |
                                  LAPIC_ICR_LEVEL_ASSERT |
                                  LAPIC_ICR_TRIGGER_LEVEL))
        return false;

    if (timer_monotonic_ready()) {
        if (!timer_monotonic_delay_us(10000U))
            return false;
    } else {
        for (u32 i = 0; i < 1000000U; i++) cpu_relax();
    }

    if (!lapic_send_ipi(apic_id, LAPIC_ICR_DELIVERY_INIT |
                                  LAPIC_ICR_TRIGGER_LEVEL))
        return false;
    if (timer_monotonic_ready())
        return timer_monotonic_delay_us(200U);
    for (u32 i = 0; i < 20000U; i++) cpu_relax();
    return true;
}

bool lapic_send_startup_ipi(u8 apic_id, u8 vector)
{
    return lapic_send_ipi(apic_id,
                           LAPIC_ICR_DELIVERY_STARTUP | (u32)vector);
}
