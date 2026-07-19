#pragma once

#include <types.h>

/* Basic types */

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u16 CHAR16;
typedef u64 EFI_PHYSICAL_ADDRESS;

typedef enum
{
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress
} EFI_ALLOCATE_TYPE;

typedef enum
{
    EFI_RESERVED_MEMORY_TYPE,
    EFI_LOADER_CODE,
    EFI_LOADER_DATA,
    EFI_BOOT_SERVICES_CODE,
    EFI_BOOT_SERVICES_DATA,
    EFI_RUNTIME_SERVICES_CODE,
    EFI_RUNTIME_SERVICES_DATA,
    EFI_CONVENTIONAL_MEMORY,
    EFI_UNUSABLE_MEMORY,
    EFI_ACPI_RECLAIM_MEMORY,
    EFI_ACPI_MEMORY_NVS,
    EFI_MEMORY_MAPPED_IO,
    EFI_MEMORY_MAPPED_IO_PORT_SPACE,
    EFI_PAL_CODE,
    EFI_PERSISTENT_MEMORY,
    EFI_UNACCEPTED_MEMORY,
    EFI_MAX_MEMORY_TYPE
} EFI_MEMORY_TYPE;

#define EFI_SUCCESS 0

#define EFIERR(x) ((1ULL << 63) | (x))
#define EFI_ERROR(Status) ((Status) & (1ULL << 63))

#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_BUFFER_TOO_SMALL  EFIERR(5)
#define EFI_OUT_OF_RESOURCES  EFIERR(9)

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_UNSUPPORTED 0x8000000000000003ULL

#define NULL ((void *)0)
#define EFI_PAGE_SIZE 4096

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

typedef struct EFI_MEMORY_DESCRIPTOR
    EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_GUID EFI_GUID;

typedef struct EFI_LOADED_IMAGE_PROTOCOL
    EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL
    EFI_FILE_PROTOCOL;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL
    EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

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

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    usize *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    usize *MapKey,
    usize *DescriptorSize,
    u32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES) (
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    usize Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL) (
    EFI_MEMORY_TYPE PoolType,
    usize Size,
    void **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL) (
    void *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL) (
    EFI_HANDLE Handle,
    EFI_GUID *Protocol,
    void **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_OPEN_VOLUME) (
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL **Root
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN) (
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    u64 OpenMode,
    u64 Attributes
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ) (
    EFI_FILE_PROTOCOL *This,
    usize *BufferSize,
    void *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION) (
    EFI_FILE_PROTOCOL *This,
    u64 Position
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES) (
    EFI_HANDLE ImageHandle,
    usize MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    void *Registration,
    void **Interface
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

struct EFI_MEMORY_DESCRIPTOR
{
    u32 Type;
    u32 Pad;

    u64 PhysicalStart;
    u64 VirtualStart;

    u64 NumberOfPages;

    u64 Attribute;
};

struct EFI_BOOT_SERVICES
{
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_ALLOCATE_PAGES AllocatePages;
    void *FreePages;

    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;

    /* Event & Timer Services */

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    /* Protocol Handler Services */

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;

    EFI_HANDLE_PROTOCOL HandleProtocol;

    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    
    /* Image Services */
    
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    
    /* Miscellaneous Services */
    
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    
    /* Driver Support Services */
    
    void *ConnectController;
    void *DisconnectController;
    
    /* Open and Close Protocol Services */
    
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    
    /* Library Services */
    
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;

    EFI_LOCATE_PROTOCOL LocateProtocol;

    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    
    /* CRC Services */
    
    void *CalculateCrc32;
    
    /* Miscellaneous Services */
    
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

struct EFI_GUID
{
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8 Data4[8];
};

struct EFI_LOADED_IMAGE_PROTOCOL
{
    u32 Revision;

    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemHandle;

    EFI_HANDLE DeviceHandle;
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    u64 Revision;

    EFI_OPEN_VOLUME OpenVolume;
};

struct EFI_FILE_PROTOCOL
{
    u64 Revision;

    EFI_FILE_OPEN Open;
    void *Close;
    void *Delete;
    EFI_FILE_READ Read;
    void *Write;

    void *GetPosition;

    EFI_FILE_SET_POSITION SetPosition;
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    u32 Version;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    u32 PixelFormat;

    u32 PixelInformation[4];

    u32 PixelsPerScanLine;

};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE
{
    u32 MaxMode;
    u32 Mode;

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    usize SizeOfInfo;

    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    usize FrameBufferSize;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL
{
    void *QueryMode;
    void *SetMode;
    void *Blt;

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* Protocol GUIDs */

static EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID =
{
    0x5B1B31A1,
    0x9562,
    0x11d2,
    { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

static EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID =
{
    0x964e5b22,
    0x6459,
    0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

static EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID =
{
    0x9042a9de,
    0x23dc,
    0x4a38,
    { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a }
};
