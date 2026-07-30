#include <kmon/block.h>
#include <block.h>
#include <kprint.h>

void kmon_block(int argc, char **argv) {
    (void)argc; (void)argv;
    u32 count = block_device_count();
    kprint("Registered Block Devices (%u):\n", count);

    for (u32 i = 0; i < count; i++) {
        block_device_t *dev = block_get_device(i);
        if (dev) {
            kprint("  [%u] Type: %s (ID: %llu, Sectors: %llu, Sector Size: %u bytes)\n",
                   i, block_type_name(dev->type), dev->id,
                   dev->sector_count, dev->sector_size);
        }
    }
}
