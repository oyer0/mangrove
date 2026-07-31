#include <acpi.h>
#include <stddef.h>

#define ACPI_MAX_CPUS 256
#define ACPI_MAX_IO_APICS 16
#define ACPI_MAX_ISOS 64

static bool present = false;
static void *rsdp = NULL;
static acpi_madt_t *madt = NULL;
static acpi_cpu_t cpus[ACPI_MAX_CPUS];
static u32 cpu_count = 0;
static acpi_io_apic_t io_apics[ACPI_MAX_IO_APICS];
static u32 io_apic_count = 0;
static acpi_iso_t isos[ACPI_MAX_ISOS];
static u32 iso_count = 0;

bool acpi_present(void)
{
    return present;
}

void *acpi_rsdp(void)
{
    return rsdp;
}

acpi_madt_t *acpi_madt(void)
{
    return madt;
}

u32 acpi_cpu_count(void)
{
    return cpu_count;
}

const acpi_cpu_t *acpi_cpu(u32 index)
{
    if (index >= cpu_count)
    {
        return NULL;
    }

    return &cpus[index];
}

void acpi_init(BOOT_INFO *BootInfo)
{
    present = false;
    rsdp = NULL;
    madt = NULL;
    cpu_count = 0;
    io_apic_count = 0;
    iso_count = 0;

    if (!BootInfo)
    {
        return;
    }
    
    acpi_rsdp_t *header = (acpi_rsdp_t *)BootInfo->Rsdp;
    if (!header) { return; }

    static const char signature[8] = {
        'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '
    };

    for (u32 i = 0; i < 8; i++) {
        if (header->signature[i] != signature[i]) { return; }
    }

    u8 sum = 0;

    u8 *bytes = (u8 *)header;

    for (u32 i = 0; i < 20; i++) {
        sum += bytes[i];
    }

    if (sum != 0) { return; }

    rsdp = header;
    present = true;

    acpi_sdt_header_t *xsdt =
    (acpi_sdt_header_t *)(uintptr_t)header->xsdt_address;

    if (!xsdt)
    {
        present = false;
        rsdp = NULL;
        return;
    }
    
    static const char xsdt_signature[4] = {
        'X', 'S', 'D', 'T'
    };
    
    for (u32 i = 0; i < 4; i++)
    {
        if (xsdt->signature[i] != xsdt_signature[i])
        {
            present = false;
            rsdp = NULL;
            return;
        }
    }

    {
        u8 sum = 0;
        u8 *bytes = (u8 *)xsdt;
    
        for (u32 i = 0; i < xsdt->length; i++)
        {
            sum += bytes[i];
        }
        
        if (sum != 0)
        {
            present = false;
            rsdp = NULL;
            return;
        }
    }

    u32 entry_count =
        (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(u64);

    u64 *entries =
        (u64 *)((u8 *)xsdt + sizeof(acpi_sdt_header_t));

    for (u32 i = 0; i < entry_count; i++) {
        acpi_sdt_header_t *table =
            (acpi_sdt_header_t *)(uintptr_t)entries[i];

        if (!table) { continue; }

        if (table->signature[0] == 'A' &&
            table->signature[1] == 'P' &&
            table->signature[2] == 'I' &&
            table->signature[3] == 'C')
        {
            madt = (acpi_madt_t *)table;
            break;
        }
    }

    if (!madt) {
        present = false;
        rsdp = NULL;
        return;
    }

    acpi_madt_entry_t *entry =
        (acpi_madt_entry_t *)((u8 *)madt + sizeof(acpi_madt_t));

    u8 *end =
        (u8 *)madt + madt->header.length;

    while ((u8 *)entry < end) {
        if (entry->type == 0)
        {
            acpi_madt_local_apic_t *lapic =
                (acpi_madt_local_apic_t *)entry;
        
            if (cpu_count < ACPI_MAX_CPUS) {
                cpus[cpu_count].processor_id = lapic->processor_id;
                cpus[cpu_count].apic_id = lapic->apic_id;
                cpus[cpu_count].flags = lapic->flags;

                cpu_count++;
            }
        }

        if (entry->type == 1)
        {
            acpi_madt_io_apic_t *ioapic =
                (acpi_madt_io_apic_t *)entry;
        
            if (io_apic_count < ACPI_MAX_IO_APICS)
            {
                io_apics[io_apic_count].id = ioapic->io_apic_id;
                io_apics[io_apic_count].address = ioapic->io_apic_address;
                io_apics[io_apic_count].gsi_base =
                    ioapic->global_system_interrupt_base;
        
                io_apic_count++;
            }
        } 

        if (entry->type == 2)
        {
            acpi_madt_iso_t *iso =
                (acpi_madt_iso_t *)entry;
        
            if (iso_count < ACPI_MAX_ISOS)
            {
                isos[iso_count].bus = iso->bus;
                isos[iso_count].source = iso->source;
                isos[iso_count].gsi = iso->gsi;
                isos[iso_count].flags = iso->flags;
        
                iso_count++;
            }
        } 

        entry = (acpi_madt_entry_t *)(
            (u8 *)entry + entry->length
        );
    }
}

u32 acpi_io_apic_count(void)
{
    return io_apic_count;
}

const acpi_io_apic_t *acpi_io_apic(u32 index)
{
    if (index >= io_apic_count)
    {
        return NULL;
    }

    return &io_apics[index];
}

u32 acpi_iso_count(void)
{
    return iso_count;
}

const acpi_iso_t *acpi_iso(u32 index)
{
    if (index >= iso_count)
    {
        return NULL;
    }

    return &isos[index];
}

u32 acpi_irq_to_gsi(u8 irq)
{
    for (u32 i = 0; i < iso_count; i++)
    {
        if (isos[i].source == irq)
        {
            return isos[i].gsi;
        }
    }

    return irq;
}
