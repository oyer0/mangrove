/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <block.h>
#include <vfs.h>
#include <kprint.h>
#include <ipc.h>
#include <mg/event.h>
#include <scheduler.h>
#include <mutex.h>
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

/* Block callbacks may sleep while a device operation is in flight.  Serialize
 * the cache and callback together with a scheduler-backed gate so a driver
 * with a single command stream (USB BOT in particular) never receives
 * overlapping requests. */
/* The mutex owns admission and FIFO parking. Its short state lock also
 * protects recursion and callback depth. Never hold it across callbacks or
 * scheduler_block(). A callback may recursively access its parent device. */
static mutex_t block_io_gate;
static u32 block_io_depth;
static u32 block_io_callbacks;

static bool block_io_acquire(void)
{
    kernel_thread_t *self = thread_current();
    u64 flags;

    if (!self) return false;
    flags = spin_lock_irqsave(&block_io_gate.state_lock);
    if (block_io_gate.owner == self) {
        if (block_io_depth == (u32)-1) {
            spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
            return false;
        }
        block_io_depth++;
        spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
        return true;
    }
    spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
    if (!mutex_lock(&block_io_gate)) return false;
    flags = spin_lock_irqsave(&block_io_gate.state_lock);
    block_io_depth = 1;
    spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
    return true;
}

static void block_io_release(void)
{
    bool release = false;
    u64 flags = spin_lock_irqsave(&block_io_gate.state_lock);

    if (block_io_gate.owner == thread_current() && block_io_depth) {
        block_io_depth--;
        release = block_io_depth == 0;
    }
    spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
    if (release) (void)mutex_unlock(&block_io_gate);
}

bool block_io_begin_quiesce(void)
{
    kernel_thread_t *self = thread_current();
    bool acquired = false;
    u64 flags;

    if (!self) return false;
    flags = spin_lock_irqsave(&block_io_gate.state_lock);
    /* The service worker must not wait for a callback which needs that same
     * worker to process hardware events. Even recursive ownership does not
     * imply quiescence: teardown invoked inside a callback must be refused. */
    if (!block_io_callbacks && block_io_depth != (u32)-1 &&
        (!block_io_gate.owner || block_io_gate.owner == self)) {
        block_io_gate.owner = self;
        block_io_depth++;
        acquired = true;
    }
    spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
    return acquired;
}

void block_io_end_quiesce(void)
{
    block_io_release();
}

static void block_callback_enter(void)
{
    u64 flags = spin_lock_irqsave(&block_io_gate.state_lock);
    block_io_callbacks++;
    spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
}

static void block_callback_leave(void)
{
    u64 flags = spin_lock_irqsave(&block_io_gate.state_lock);
    block_io_callbacks--;
    spin_unlock_irqrestore(&block_io_gate.state_lock, flags);
}

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
        if (entry->valid && entry->device == device && lba >= entry->lba &&
            lba - entry->lba <= entry->sector_count &&
            sector_count <= entry->sector_count - (lba - entry->lba))
            return entry;
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
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (!devices[slot].registered) continue;
        if (index == 0) return &devices[slot];
        index--;
    }
    return NULL;
}

block_device_t *block_get_device_by_public_id(u64 public_id)
{
    u64 internal_id;

    if ((public_id & 0xf000000000000000ULL) != BLOCK_DEVICE_ID_BASE ||
        public_id < BLOCK_DEVICE_ID_BASE + 1ULL) return NULL;
    internal_id = public_id - BLOCK_DEVICE_ID_BASE - 1ULL;
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (devices[slot].registered && devices[slot].id == internal_id)
            return &devices[slot];
    }
    return NULL;
}

bool block_register(block_device_t *device)
{
    u32 slot;

    if (!device)
    {
        return false;
    }

    if (device_count >= BLOCK_MAX_DEVICES)
    {
        return false;
    }
    
    for (slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (!devices[slot].registered) break;
    }
    if (slot == BLOCK_MAX_DEVICES) return false;

    device->id = next_device_id++;

    devices[slot] = *device;
    devices[slot].registered = true;
    devices[slot].online = true;
    /* Some bounded partition callbacks retain the driver's source descriptor
     * rather than the registry slot.  Give that descriptor the same
     * immutable identity; liveness is checked by ID below, never by address. */
    device->registered = true;
    device->online = true;

    device_count++;

    ipc_publish_event(MG_EVENT_CLASS_DEVICE | MG_EVENT_CLASS_BLOCK,
                      MG_EVENT_BLOCK_ADDED,
                      BLOCK_DEVICE_ID_BASE |
                          (devices[slot].id + 1ULL),
                      block_type_name(devices[slot].type));

    return true;
}

static void block_cache_invalidate_all(void)
{
    memset(block_cache, 0, sizeof(block_cache));
    block_cache_age = 0;
}

static u32 block_find_slot_by_id(u64 id)
{
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (devices[slot].registered && devices[slot].id == id)
            return slot;
    }
    return BLOCK_MAX_DEVICES;
}

bool block_device_id_is_live(u64 id)
{
    u32 slot = block_find_slot_by_id(id);
    return slot < BLOCK_MAX_DEVICES && devices[slot].online;
}

bool block_device_is_live(const block_device_t *device)
{
    if (!device || !device->registered) return false;
    /* Drivers may keep a source descriptor that was copied into the stable
     * registry slot at registration.  The monotonic ID identifies that exact
     * instance and is not reused during a boot, so it is the safe authority
     * for both representations. */
    return block_device_id_is_live(device->id);
}

bool block_device_set_automount_suppressed(u64 id, bool suppressed)
{
    u32 slot = block_find_slot_by_id(id);

    if (slot == BLOCK_MAX_DEVICES || !devices[slot].online) return false;
    devices[slot].automount_suppressed = suppressed;
    return true;
}

bool block_device_automount_suppressed(u64 id)
{
    u32 slot = block_find_slot_by_id(id);

    return slot < BLOCK_MAX_DEVICES && devices[slot].online &&
           devices[slot].automount_suppressed;
}

bool block_device_mark_gone(u64 id)
{
    u32 slot = block_find_slot_by_id(id);

    if (slot == BLOCK_MAX_DEVICES) return false;
    if (devices[slot].online) {
        devices[slot].online = false;
        vfs_device_removed(&devices[slot]);
    }
    return true;
}

bool block_unregister_by_id(u64 id)
{
    u32 slot;
    const char *event_name;

    slot = block_find_slot_by_id(id);
    if (slot == BLOCK_MAX_DEVICES) return false;
    event_name = block_type_name(devices[slot].type);
    (void)block_device_mark_gone(id);
    memset(&devices[slot], 0, sizeof(devices[slot]));
    device_count--;
    block_cache_invalidate_all();
    ipc_publish_event(MG_EVENT_CLASS_DEVICE | MG_EVENT_CLASS_BLOCK,
                      MG_EVENT_BLOCK_REMOVED,
                      BLOCK_DEVICE_ID_BASE | (id + 1ULL), event_name);
    return true;
}

bool block_unregister_by_driver_data(void *driver_data)
{
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (devices[slot].registered && devices[slot].driver_data == driver_data)
            return block_unregister_by_id(devices[slot].id);
    }
    return false;
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

    if (!block_device_is_live(device)) return false;

    if (!block_io_acquire())
        return false;

    if (!block_device_is_live(device)) {
        block_io_release();
        return false;
    }

    block_stats.read_requests++;
    byte_count = 0;
    if (sector_count && device->sector_size &&
        device->sector_size <= BLOCK_CACHE_ENTRY_BYTES &&
        lba <= (u64)-1 - sector_count) {
        u32 maximum = BLOCK_CACHE_ENTRY_BYTES / device->sector_size;
        u32 offset = 0;
        while (offset < sector_count) {
            u32 chunk = sector_count - offset;
            if (chunk > maximum) chunk = maximum;
            entry = block_cache_find(device, lba + offset, chunk);
            if (!entry) break;
            entry->age = ++block_cache_age;
            memcpy((u8 *)buffer + (u64)offset * device->sector_size,
                   entry->data + (lba + offset - entry->lba) * device->sector_size,
                   chunk * device->sector_size);
            offset += chunk;
        }
        if (offset == sector_count) {
            block_stats.cache_hits++;
            block_io_release();
            /* Removal publishes a dead instance before it can wait for the
             * I/O gate.  Do not report cached data as a successful access if
             * that exact instance disappeared while this request was being
             * admitted. */
            return block_device_is_live(device);
        }
        block_stats.cache_misses++;
    }

    block_callback_enter();
    bool read_result = device->read(device, lba, sector_count, buffer);
    block_callback_leave();
    if (!read_result) {
#ifdef NETWORK_BOOT_DIAG
        kprint("[BLOCK-READ] failed dev=%llu type=%u lba=%llu sectors=%u\n",
               device->id, (u32)device->type, lba, sector_count);
#endif
        block_io_release();
        return false;
    }
    /* A removable backend can complete its transport command at the same
     * time that teardown marks the registry instance gone.  The bytes are
     * no longer a valid result for the caller in that case, and must not be
     * retained in the cache for a future instance. */
    if (!block_device_is_live(device)) {
        block_io_release();
        return false;
    }
    block_stats.device_reads++;
    if (sector_count && device->sector_size &&
        device->sector_size <= BLOCK_CACHE_ENTRY_BYTES &&
        lba <= (u64)-1 - sector_count) {
        u32 maximum = BLOCK_CACHE_ENTRY_BYTES / device->sector_size;
        for (u32 offset = 0; offset < sector_count;) {
            u32 chunk = sector_count - offset;
            if (chunk > maximum) chunk = maximum;
            if (!block_cache_request_size(device, chunk, &byte_count)) break;
            entry = block_cache_find(device, lba + offset, chunk);
            if (!entry) entry = block_cache_victim();
            entry->device = device;
            entry->lba = lba + offset;
            entry->sector_count = chunk;
            entry->byte_count = byte_count;
            entry->age = ++block_cache_age;
            entry->valid = true;
            memcpy(entry->data, (const u8 *)buffer +
                               (u64)offset * device->sector_size, byte_count);
            offset += chunk;
        }
    }
    block_io_release();
    return true;
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

    if (!device->write || device->read_only)
    {
        return false;
    }

    if (!block_device_is_live(device)) return false;

    if (!block_io_acquire())
        return false;

    if (!block_device_is_live(device)) {
        block_io_release();
        return false;
    }

    block_stats.write_requests++;
    for (u32 i = 0; i < BLOCK_CACHE_ENTRY_COUNT; i++) {
        if (block_cache_range_overlaps(&block_cache[i], device, lba,
                                       sector_count)) {
            block_cache[i].valid = false;
        }
    }

    block_callback_enter();
    bool result = device->write(device, lba, sector_count, buffer);
    block_callback_leave();
    block_io_release();
    /* Match reads and flushes: a write racing physical removal is never a
     * successful operation, even if the transport finished a command before
     * the removal notification reached the controller worker. */
    return result && block_device_is_live(device);
}

bool block_flush(block_device_t *device)
{
    bool result;

    if (!device || !block_device_is_live(device)) return false;
    if (!block_io_acquire()) return false;
    if (!block_device_is_live(device)) {
        block_io_release();
        return false;
    }
    /* A missing callback is meaningful for backends whose writes complete
     * synchronously and which expose no separate cache-flush command. */
    block_callback_enter();
    result = !device->flush || device->flush(device);
    block_callback_leave();
    block_io_release();
    return result && block_device_is_live(device);
}

bool block_probe_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer)
{
    bool result;

    if (!device || !device->read || !buffer || !sector_count)
        return false;
    if (!block_io_acquire()) return false;
    block_callback_enter();
    result = device->read(device, lba, sector_count, buffer);
    block_callback_leave();
    block_io_release();
    return result;
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
