#pragma once

#include <types.h>

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;

#define EFI_SUCCESS 0

#ifdef __x86_64__
#define EFIAPI __attribute__((ms_abi))
#else
#error Unsupported architecture
#endif

/* Forward declarations */

typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

/* Entry point */

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_ENTRY_POINT)(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
);
