#pragma once

#include <types.h>
#include <bootinfo.h>

bool acpi_present(void);
void *acpi_rsdp(void);

void acpi_init(BOOT_INFO *BootInfo);

typedef struct PACKED
{
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;

    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
} acpi_rsdp_t;

typedef struct PACKED
{
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} acpi_sdt_header_t;

typedef struct PACKED
{
    acpi_sdt_header_t header;

    u32 local_apic_address;
    u32 flags;
} acpi_madt_t;

acpi_madt_t *acpi_madt(void);

typedef struct PACKED
{
    u8 type;
    u8 length;
} acpi_madt_entry_t;

typedef struct PACKED
{
    acpi_madt_entry_t header;

    u8 processor_id;
    u8 apic_id;
    u32 flags;
} acpi_madt_local_apic_t;

typedef struct PACKED
{
    acpi_madt_entry_t header;

    u8 io_apic_id;
    u8 reserved;
    u32 io_apic_address;
    u32 global_system_interrupt_base;
} acpi_madt_io_apic_t;

typedef struct PACKED
{
    acpi_madt_entry_t header;

    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
} acpi_madt_iso_t;

typedef struct
{
    u8 processor_id;
    u8 apic_id;
    u32 flags;
} acpi_cpu_t;

typedef struct
{
    u8 id;
    u32 address;
    u32 gsi_base;
} acpi_io_apic_t;

typedef struct
{
    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
} acpi_iso_t;

u32 acpi_io_apic_count(void);
const acpi_io_apic_t *acpi_io_apic(u32 index);

u32 acpi_iso_count(void);
const acpi_iso_t *acpi_iso(u32 index);

u32 acpi_cpu_count(void);
const acpi_cpu_t *acpi_cpu(u32 index);

u32 acpi_irq_to_gsi(u8 irq);
