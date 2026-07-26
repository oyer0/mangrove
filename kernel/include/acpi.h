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
