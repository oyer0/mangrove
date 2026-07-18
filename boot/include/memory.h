#pragma once

#include <uefi.h>

void memory_init(EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS memory_map_get(void);
