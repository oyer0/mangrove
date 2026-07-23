#pragma once

enum{
    EFI_RESERVED_MEMORY_TYPE         = 0,
    EFI_LOADER_CODE                  = 1,
    EFI_LOADER_DATA                  = 2,
    EFI_BOOT_SERVICES_CODE           = 3,
    EFI_BOOT_SERVICES_DATA           = 4,
    EFI_RUNTIME_SERVICES_CODE        = 5,
    EFI_RUNTIME_SERVICES_DATA        = 6,
    EFI_CONVENTIONAL_MEMORY          = 7,
    EFI_UNUSABLE_MEMORY              = 8,
    EFI_ACPI_RECLAIM_MEMORY          = 9,
    EFI_ACPI_MEMORY_NVS              = 10,
    EFI_MEMORY_MAPPED_IO             = 11,
    EFI_MEMORY_MAPPED_IO_PORT_SPACE  = 12,
    EFI_PAL_CODE                     = 13,
    EFI_PERSISTENT_MEMORY            = 14
};
