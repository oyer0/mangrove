#include <storage/gpt.h>
#include <kprint.h>
#include <stddef.h>

typedef struct {
    block_device_t *parent;
    u64 first_lba;
    u64 last_lba;
} gpt_partition_data_t;

static gpt_partition_data_t partitions[16];
static u32 partition_count;

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 le64(const u8 *p)
{
    u64 v = 0;
    for (u32 i = 0; i < 8; i++) v |= (u64)p[i] << (i * 8);
    return v;
}

static bool partition_read(block_device_t *device, u64 lba, u32 count, void *buffer)
{
    gpt_partition_data_t *part = (gpt_partition_data_t *)device->driver_data;
    if (!part || lba > part->last_lba - part->first_lba + 1ULL ||
        count > part->last_lba - part->first_lba + 1ULL - lba) return false;
    return block_read(part->parent, part->first_lba + lba, count, buffer);
}

static bool partition_write(block_device_t *device, u64 lba, u32 count,
                            const void *buffer)
{
    gpt_partition_data_t *part = (gpt_partition_data_t *)device->driver_data;
    if (!part || lba > part->last_lba - part->first_lba + 1ULL ||
        count > part->last_lba - part->first_lba + 1ULL - lba) return false;
    return block_write(part->parent, part->first_lba + lba, count, buffer);
}

bool gpt_scan_device(block_device_t *device)
{
    u8 header[512];
    u64 entries_lba, entry_count;
    u32 entry_size;
    bool found = false;
    if (!device || device->sector_size != 512 || device->sector_count < 2 ||
        !block_read(device, 1, 1, header)) {
        return false;
    }
    if (header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T') {
        return false;
    }
    entries_lba = le64(header + 72);
    entry_count = le32(header + 80);
    entry_size = le32(header + 84);
    if (!entry_count || entry_size < 128 || entry_size > 512 || entries_lba >= device->sector_count)
        return false;
    if (entry_count > 128) entry_count = 128;

    for (u64 index = 0; index < entry_count && partition_count < 16; index++) {
        u8 sector[512];
        u64 lba = entries_lba + (index * entry_size) / 512ULL;
        u32 offset = (u32)((index * entry_size) % 512ULL);
        if (!block_read(device, lba, 1, sector)) break;
        if (offset + entry_size > 512) continue;
        u64 first = le64(sector + offset + 32);
        u64 last = le64(sector + offset + 40);
        bool type_nonzero = false;
        for (u32 i = 0; i < 16; i++) if (sector[offset + i] != 0) type_nonzero = true;
        if (!type_nonzero || first > last || last >= device->sector_count) continue;

        gpt_partition_data_t *part = &partitions[partition_count];
        block_device_t child;
        part->parent = device;
        part->first_lba = first;
        part->last_lba = last;
        child.type = BLOCK_DEVICE_PARTITION;
        child.sector_size = device->sector_size;
        child.sector_count = last - first + 1ULL;
        child.read = partition_read;
        child.write = partition_write;
        child.driver_data = part;
        if (block_register(&child)) {
            partition_count++;
            found = true;
        }
    }
    if (found) KERNEL_BOOT_DEBUG_LOG("[GPT] partition table discovered\n");
    return found;
}
