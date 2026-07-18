#include <memory.h>

static EFI_BOOT_SERVICES *BootServices;

void memory_init(EFI_SYSTEM_TABLE *SystemTable)
{
    BootServices = SystemTable->BootServices;
}
