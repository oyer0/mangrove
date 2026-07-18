#include <filesystem.h>

static EFI_BOOT_SERVICES *BootServices;

EFI_STATUS filesystem_init(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    BootServices = SystemTable->BootServices;

    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;

    EFI_STATUS Status = BootServices->HandleProtocol(
        ImageHandle,
        &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&LoadedImage
    );

    Status = BootServices->HandleProtocol(
        LoadedImage->DeviceHandle,
        &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
        (void **)&FileSystem
    );

    EFI_FILE_PROTOCOL *Root;

    Status = FileSystem->OpenVolume(
        FileSystem,
        &Root
    );

    return Status;
}
