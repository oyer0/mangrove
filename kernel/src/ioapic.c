#include <ioapic.h>
#include <acpi.h>
#include <irq.h>
#include <stddef.h>
#include <vmm.h>

static volatile u32 *ioapic = NULL;
static bool present = false;
static u32 gsi_base = 0;
static u32 gsi_limit = 0;

bool ioapic_present(void)
{
    return present;
}

void ioapic_init(void)
{
    present = false;
    ioapic = NULL;
    gsi_base = 0;
    gsi_limit = 0;

    const acpi_io_apic_t *apic = acpi_io_apic(0);

    if (!apic)
    {
        return;
    }

    ioapic = (volatile u32 *)vmm_map_mmio(
        (phys_addr_t)apic->address, 0x1000);
    if (!ioapic) return;

    u32 version = ioapic_read(IOAPIC_VERSION);
    gsi_base = apic->gsi_base;
    gsi_limit = gsi_base + ((version >> 16) & 0xFFU);
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

bool ioapic_route_gsi(u32 gsi, u8 vector, u8 apic_id, u16 acpi_flags)
{
    u64 entry = 0;

    if (!present || vector < IRQ_VECTOR_FIRST ||
        vector > IRQ_VECTOR_LAST || gsi < gsi_base || gsi > gsi_limit)
        return false;

    /* Vector (bits 0-7) */
    entry |= (u64)vector;

    /* Delivery Mode = Fixed (000) */
    /* Destination Mode = Physical (0) */
    /* MADT flags use 3 for active-low and level-triggered.  Zero means
     * ISA-conformant defaults here: active-high and edge-triggered. */
    if ((acpi_flags & 0x3U) == 0x3U)
        entry |= (1ULL << 13);
    if (((acpi_flags >> 2) & 0x3U) == 0x3U)
        entry |= (1ULL << 15);
    /* Mask = 0 (enabled) */

    /* Destination APIC ID (bits 56-63) */
    entry |= ((u64)apic_id << 56);

    ioapic_write_redirection((u8)(gsi - gsi_base), entry);

    u64 verify = ioapic_read_redirection((u8)(gsi - gsi_base));
    return (verify & 0xFFU) == vector &&
           ((verify >> 56) & 0xFFU) == apic_id;
}
