#include <block.h>
#include <stddef.h>

static block_device_t devices[BLOCK_MAX_DEVICES];
static u32 device_count = 0;

u32 block_device_count(void)
{
    return device_count;
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
