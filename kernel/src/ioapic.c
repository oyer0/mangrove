#include <ioapic.h>
#include <acpi.h>
#include <stddef.h>
#include <vmm.h>

static volatile u32 *ioapic = NULL;
static bool present = false;

bool ioapic_present(void)
{
    return present;
}

void ioapic_init(void)
{
    present = false;
    ioapic = NULL;

    const acpi_io_apic_t *apic = acpi_io_apic(0);

    if (!apic)
    {
        return;
    }

    ioapic = (volatile u32 *)vmm_map_mmio(
        (phys_addr_t)apic->address, 0x1000);
    if (!ioapic) return;

    present = true;
}

u32 ioapic_read(u8 reg)
{
    ioapic[0] = reg;
    return ioapic[4];
}

void ioapic_write(u8 reg, u32 value)
{
    ioapic[0] = reg;
    ioapic[4] = value;
}

u64 ioapic_read_redirection(u8 irq)
{
    u32 high = ioapic_read(IOAPIC_REDTBL + irq * 2 + 1);
    u32 low = ioapic_read(IOAPIC_REDTBL + irq * 2);

    return ((u64)high << 32) | low;
}

void ioapic_write_redirection(u8 irq, u64 value)
{
    ioapic_write(
        IOAPIC_REDTBL + irq * 2 + 1,
        (u32)(value >> 32)
    );

    ioapic_write(
        IOAPIC_REDTBL + irq * 2,
        (u32)value
    );
}

void ioapic_route_irq(u8 irq, u8 vector, u8 apic_id)
{
    u64 entry = 0;

    /* Vector (bits 0-7) */
    entry |= (u64)vector;

    /* Delivery Mode = Fixed (000) */
    /* Destination Mode = Physical (0) */
    /* Polarity = High (0) */
    /* Trigger Mode = Edge (0) */
    /* Mask = 0 (enabled) */

    /* Destination APIC ID (bits 56-63) */
    entry |= ((u64)apic_id << 56);

    ioapic_write_redirection(irq, entry);

    u64 verify = ioapic_read_redirection(irq);
}
