#pragma once

#include <types.h>

/* Basic types */

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u16 CHAR16;

#define EFI_SUCCESS 0

#ifdef __x86_64__
#define EFIAPI __attribute__((ms_abi))
#else
#error Unsupported architecture
#endif

/* Forward declarations */

typedef struct EFI_TABLE_HEADER EFI_TABLE_HEADER;

typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct EFI_RUNTIME_SERVICES
    EFI_RUNTIME_SERVICES;

typedef struct EFI_BOOT_SERVICES
    EFI_BOOT_SERVICES;

typedef struct EFI_CONFIGURATION_TABLE
    EFI_CONFIGURATION_TABLE;

/* Function pointers */

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_ENTRY_POINT)(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    const CHAR16 *String
);

typedef void *EFI_TEXT_TEST_STRING;
typedef void *EFI_TEXT_QUERY_MODE;
typedef void *EFI_TEXT_SET_MODE;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    usize Attribute
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION) (
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    usize Column,
    usize Row
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR) (
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    u8 Visible
);

/* Structures */

struct EFI_TABLE_HEADER
{
    u64 Signature;
    u32 Revision;
    u32 HeaderSize;
    u32 CRC32;
    u32 Reserved;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
{
    void *Reset;
    EFI_TEXT_STRING OutputString;
    EFI_TEXT_TEST_STRING TestString;
    EFI_TEXT_QUERY_MODE QueryMode;
    EFI_TEXT_SET_MODE SetMode;
    EFI_TEXT_SET_ATTRIBUTE SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR EnableCursor;

    void *Mode;
};

struct EFI_SYSTEM_TABLE
{
    EFI_TABLE_HEADER Hdr;

    CHAR16 *FirmwareVendor;
    u32 FirmwareRevision;

    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;

    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;

    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;

    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;

    usize NumberOfTableEntries;

    EFI_CONFIGURATION_TABLE *ConfigurationTable;
};
