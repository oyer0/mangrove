#include <filesystem.h>

static EFI_BOOT_SERVICES *BootServices;
static EFI_FILE_PROTOCOL *Root;

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
    
    Status = FileSystem->OpenVolume(
        FileSystem,
        &Root
    );
    
    return Status;

}

EFI_STATUS filesystem_open(
    CHAR16 *Path,
    EFI_FILE_PROTOCOL **File)
{
    return Root->Open(
        Root,
        File,
        Path,
        EFI_FILE_MODE_READ,
        0
    );
}

EFI_STATUS filesystem_read(
    EFI_FILE_PROTOCOL *File,
    void *Buffer,
    usize *BufferSize)
{
    return File->Read(
        File,
        BufferSize,
        Buffer
    );
}

EFI_STATUS filesystem_seek(
    EFI_FILE_PROTOCOL *File,
    u64 Position)
{
    return File->SetPosition(
        File,
        Position
    );
}
