#include <kmon/block.h>
#include <block.h>
#include <kprint.h>

void kmon_block(void) {
    u32 count = block_device_count();

    if (count == 0) {
        kprint("No block devices registered.\n");
        return;
    }

    kprint("Block devices:\n\n");

    for (u32 i = 0; i < count; i++) {
        block_device_t *device = block_get_device(i);

        if (!device) {
            continue;
        }

        kprint("ID: %u\n", device->id);
        kprint("Type: %u\n", block_type_name(device->type));
        kprint("Sector Size: %u bytes\n", device->sector_size);
        kprint("Sector Count: %u\n", device->sector_count);
        kprint("\n");
    }
}
