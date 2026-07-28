#include <lapic.h>
#include <acpi.h>
#include <stddef.h>
#include <msr.h>

static volatile u32 *lapic = NULL;
static bool present = false;

bool lapic_present(void)
{
    return present;
}

void lapic_init(void)
{
    present = false;
    lapic = NULL;

    acpi_madt_t *madt = acpi_madt();

    if (!madt)
    {
        return;
    }

    lapic = (volatile u32 *)(uintptr_t)madt->local_apic_address;

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

    u64 apic_base = rdmsr(0x1B);
    apic_base |= (1ULL << 11);
    wrmsr(0x1B, apic_base);
    u32 svr = lapic_read(LAPIC_SVR);
    svr |= (1 << 8);
    svr = (svr & ~0xFF);
    svr |= 0xFF;
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
}
