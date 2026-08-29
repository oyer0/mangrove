#include <block.h>
#include <stddef.h>
#include <string.h>

static block_device_t devices[BLOCK_MAX_DEVICES];
static u32 device_count = 0;
static u64 next_device_id = 0;

typedef struct {
    block_device_t *device;
    u64 lba;
    u32 sector_count;
    u32 byte_count;
    u64 age;
    bool valid;
    u8 data[BLOCK_CACHE_ENTRY_BYTES];
} block_cache_entry_t;

static block_cache_entry_t block_cache[BLOCK_CACHE_ENTRY_COUNT];
static u64 block_cache_age;
static block_io_stats_t block_stats;

static bool block_cache_range_overlaps(
    const block_cache_entry_t *entry,
    block_device_t *device,
    u64 lba,
    u32 sector_count)
{
    u64 entry_end;
    u64 write_end;

    if (!entry->valid || entry->device != device) return false;
    if (entry->lba > (u64)-1 - entry->sector_count ||
        lba > (u64)-1 - sector_count) return true;
    entry_end = entry->lba + entry->sector_count;
    write_end = lba + sector_count;
    return entry->lba < write_end && lba < entry_end;
}

static bool block_cache_request_size(const block_device_t *device,
                                     u32 sector_count, u32 *out_bytes)
{
    u64 bytes;

    if (!device || !out_bytes || sector_count == 0 ||
        device->sector_size == 0) return false;
    bytes = (u64)sector_count * device->sector_size;
    if (bytes > BLOCK_CACHE_ENTRY_BYTES) return false;
    *out_bytes = (u32)bytes;
    return true;
}

static block_cache_entry_t *block_cache_find(block_device_t *device,
                                             u64 lba, u32 sector_count)
{
    for (u32 i = 0; i < BLOCK_CACHE_ENTRY_COUNT; i++) {
        block_cache_entry_t *entry = &block_cache[i];
        if (entry->valid && entry->device == device && entry->lba == lba &&
            entry->sector_count == sector_count) return entry;
    }
    return NULL;
}

static block_cache_entry_t *block_cache_victim(void)
{
    block_cache_entry_t *victim = &block_cache[0];

    for (u32 i = 0; i < BLOCK_CACHE_ENTRY_COUNT; i++) {
        block_cache_entry_t *entry = &block_cache[i];
        if (!entry->valid) return entry;
        if (entry->age < victim->age) victim = entry;
    }
    return victim;
}

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
    u32 byte_count;
    block_cache_entry_t *entry;

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

    block_stats.read_requests++;
    if (block_cache_request_size(device, sector_count, &byte_count)) {
        entry = block_cache_find(device, lba, sector_count);
        if (entry) {
            entry->age = ++block_cache_age;
            memcpy(buffer, entry->data, byte_count);
            block_stats.cache_hits++;
            return true;
        }
        block_stats.cache_misses++;
    } else {
        entry = NULL;
    }

    if (!device->read(device, lba, sector_count, buffer)) return false;
    block_stats.device_reads++;
    if (entry || block_cache_request_size(device, sector_count, &byte_count)) {
        if (!entry) entry = block_cache_victim();
        entry->device = device;
        entry->lba = lba;
        entry->sector_count = sector_count;
        entry->byte_count = byte_count;
        entry->age = ++block_cache_age;
        entry->valid = true;
        memcpy(entry->data, buffer, byte_count);
    }
    return true;
}

bool block_write(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer)
{
    block_stats.write_requests++;

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

    for (u32 i = 0; i < BLOCK_CACHE_ENTRY_COUNT; i++) {
        if (block_cache_range_overlaps(&block_cache[i], device, lba,
                                       sector_count)) {
            block_cache[i].valid = false;
        }
    }

    return device->write(device, lba, sector_count, buffer);
}

void block_io_stats_reset(void)
{
    memset(&block_stats, 0, sizeof(block_stats));
}

void block_io_stats_get(block_io_stats_t *out_stats)
{
    if (!out_stats) return;
    *out_stats = block_stats;
}
