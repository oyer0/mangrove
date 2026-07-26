#include <acpi.h>
#include <stddef.h>

static bool present = false;
static void *rsdp = NULL;

bool acpi_present(void)
{
    return present;
}

void *acpi_rsdp(void)
{
    return rsdp;
}

void acpi_init(BOOT_INFO *BootInfo)
{
    present = false;
    rsdp = NULL;

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
}
