#include <kmon/block.h>
#include <block.h>
#include <kprint.h>
#include <heap.h>
#include <string.h>

void kmon_block(void)
{
    u32 count = block_device_count();

    if (count == 0)
    {
        kprint("No block devices registered.\n");
        return;
    }

    kprint("Block devices:\n\n");

    for (u32 i = 0; i < count; i++)
    {
        block_device_t *device = block_get_device(i);

        if (!device)
        {
            continue;
        }

        kprint("ID: %u\n", device->id);
        kprint("Type: %s\n", block_type_name(device->type));
        kprint("Sector Size: %u bytes\n", device->sector_size);
        kprint("Sector Count: %u\n", device->sector_count);
        kprint("\n");
    }

    block_device_t *device = block_get_device(1);

    if (!device)
    {
        kprint("No test disk.\n");
        return;
    }

    u8 *buffer = kmalloc(512);

    if (!buffer)
    {
        kprint("Allocation failed.\n");
        return;
    }

    memset(buffer, 0, 512);
    memcpy(buffer, "MANGROVE", 8);

    if (!block_write(device, 0, 1, buffer))
    {
        kprint("Write failed.\n");
        kfree(buffer);
        return;
    }

    memset(buffer, 0, 512);

    if (!block_read(device, 0, 1, buffer))
    {
        kprint("Read failed.\n");
        kfree(buffer);
        return;
    }

    kprint("Write + read successful.\n");

    for (u32 i = 0; i < 64; i++)
    {
        kprint("%x ", buffer[i]);

        if ((i + 1) % 16 == 0)
        {
            kprint("\n");
        }
    }

    kfree(buffer);
}
