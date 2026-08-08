#include <block.h>
#include <stddef.h>

static block_device_t devices[BLOCK_MAX_DEVICES];
static u32 device_count = 0;
static u64 next_device_id = 0;

u32 block_device_count(void)
{
    return device_count;
}

const char *block_type_name(block_device_type_t type)
{
    switch (type)
    {
        case BLOCK_DEVICE_SATA:
            return "SATA";

        case BLOCK_DEVICE_NVME:
            return "NVMe";

        case BLOCK_DEVICE_USB:
            return "USB";

        case BLOCK_DEVICE_RAM:
            return "RAM";

        case BLOCK_DEVICE_PARTITION:
            return "partition";

        default:
            return "Unknown";
    }
}

block_device_t *block_get_device(u32 index)
{
    if (index >= device_count)
    {
        return NULL;
    }

    return &devices[index];
}

bool block_register(block_device_t *device)
{
    if (!device)
    {
        return false;
    }

    if (device_count >= BLOCK_MAX_DEVICES)
    {
        return false;
    }
    
    device->id = next_device_id++;

    devices[device_count] = *device;

    device_count++;

    return true;
}

bool block_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer)
{
    if (!device)
    {
        return false;
    }

    if (!buffer)
    {
        return false;
    }

    if (!device->read)
    {
        return false;
    }

    return device->read(device, lba, sector_count, buffer);
}

bool block_write(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer)
{
    if (!device)
    {
        return false;
    }

    if (!buffer)
    {
        return false;
    }

    if (!device->write)
    {
        return false;
    }

    return device->write(device, lba, sector_count, buffer);
}
