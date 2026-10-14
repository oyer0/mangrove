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
#include <storage/gpt.h>

static block_device_t devices[BLOCK_MAX_DEVICES];
static u32 device_count = 0;
static u64 next_device_id = 0;
/* Registry lock never spans callbacks, VFS notifications or gate admission. */
static spinlock_t block_registry_lock;
static bool retirement_notifying[BLOCK_MAX_DEVICES];

typedef struct {
    u64 storage_id;
    u64 lba;
    u32 sector_size;
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
    const block_physical_range_t *range,
    u64 lba,
    u32 sector_count)
{
    u64 entry_end;
    u64 write_end;

    if (!entry->valid || entry->storage_id != range->storage_id) return false;
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

static block_cache_entry_t *block_cache_find(const block_physical_range_t *range,
                                             u64 lba, u32 sector_count)
{
    for (u32 i = 0; i < BLOCK_CACHE_ENTRY_COUNT; i++) {
        block_cache_entry_t *entry = &block_cache[i];
        if (entry->valid && entry->storage_id == range->storage_id &&
            entry->sector_size == range->sector_size && lba >= entry->lba &&
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
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    u32 count = device_count;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return count;
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
    block_device_t *result = NULL;
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (!devices[slot].registered) continue;
        if (index == 0) { result = &devices[slot]; break; }
        index--;
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return result;
}


block_device_t *block_get_device_by_public_id(u64 public_id)
{
    block_device_t *result = NULL;
    u64 flags;
    if ((public_id & 0xf000000000000000ULL) != BLOCK_DEVICE_ID_BASE ||
        public_id < BLOCK_DEVICE_ID_BASE + 1ULL) return NULL;
    flags = spin_lock_irqsave(&block_registry_lock);
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (devices[slot].registered &&
            devices[slot].id == public_id - BLOCK_DEVICE_ID_BASE - 1ULL) {
            result = &devices[slot];
            break;
        }
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return result;
}


bool block_register(block_device_t *device)
{
    u32 slot;
    u64 id, flags;
    block_device_type_t type;

    if (!device || !block_io_begin_quiesce()) return false;
    flags = spin_lock_irqsave(&block_registry_lock);
    /* One physical backend has one root registration. Driver-owned copies
     * cannot create a second cache identity for that same backend. Retiring
     * registrations continue to exclude re-registration until removed. */
    for (u32 i = 0; i < BLOCK_MAX_DEVICES; i++) {
        if (!devices[i].registered) continue;
        if ((device->registered && devices[i].id == device->id) ||
            (device->type != BLOCK_DEVICE_PARTITION &&
             devices[i].type != BLOCK_DEVICE_PARTITION && device->driver_data &&
             device->driver_data == devices[i].driver_data)) {
            spin_unlock_irqrestore(&block_registry_lock, flags);
            block_io_end_quiesce();
            return false;
        }
    }
    for (slot = 0; slot < BLOCK_MAX_DEVICES; slot++)
        if (!devices[slot].registered) break;
    if (slot == BLOCK_MAX_DEVICES || next_device_id >= 0x0ffffffffffffffeULL) {
        spin_unlock_irqrestore(&block_registry_lock, flags);
        block_io_end_quiesce();
        return false;
    }
    device->id = id = next_device_id++;
    device->registered = device->online = true;
    devices[slot] = *device;
    type = device->type;
    device_count++;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    if (type != BLOCK_DEVICE_PARTITION) gpt_bind_partition_parent(device);
    block_io_end_quiesce();
    ipc_publish_event(MG_EVENT_CLASS_DEVICE | MG_EVENT_CLASS_BLOCK,
                      MG_EVENT_BLOCK_ADDED, BLOCK_DEVICE_ID_BASE | (id + 1ULL),
                      block_type_name(type));
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
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    u32 slot = block_find_slot_by_id(id);
    bool live = slot < BLOCK_MAX_DEVICES && devices[slot].online;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return live;
}


bool block_device_is_live(const block_device_t *device)
{
    u64 flags;
    bool live = false;
    if (!device) return false;
    flags = spin_lock_irqsave(&block_registry_lock);
    if (device->registered) {
        u32 slot = block_find_slot_by_id(device->id);
        live = slot < BLOCK_MAX_DEVICES && devices[slot].online;
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return live;
}

/* Capture an instance before an operation parks on the I/O gate. Revalidate
 * after admission so a queued request cannot follow reuse of a registry slot.
 * Driver-owned descriptors require their owner's lifetime, as before. */
static bool block_capture_live_id(const block_device_t *device, u64 *id)
{
    bool live = false;
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    if (device && device->registered) {
        u32 slot = block_find_slot_by_id(device->id);
        live = slot < BLOCK_MAX_DEVICES && devices[slot].online;
        if (live) *id = device->id;
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return live;
}

/* A mounted filesystem must use the exact registry object it mounted, even
 * if that slot has since been retired and reused. */
static bool block_device_matches_live_id(const block_device_t *device, u64 id)
{
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    u32 slot = block_find_slot_by_id(id);
    bool match = slot < BLOCK_MAX_DEVICES && devices[slot].online &&
                 device == &devices[slot];
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return match;
}

bool block_device_snapshot(const block_device_t *device, block_device_t *out)
{
    u64 flags;
    bool found = false;
    if (!device || !out) return false;
    flags = spin_lock_irqsave(&block_registry_lock);
    if (device->registered) {
        u32 slot = block_find_slot_by_id(device->id);
        if (slot < BLOCK_MAX_DEVICES) { *out = devices[slot]; found = true; }
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return found;
}

bool block_device_physical_range(block_device_t *device, u64 expected_id,
                                  block_physical_range_t *out)
{
    block_device_t snapshot, parent;
    gpt_partition_info_t part;
    if (!out || !block_device_snapshot(device, &snapshot) ||
        snapshot.id != expected_id || !block_device_id_is_live(expected_id) ||
        !snapshot.sector_size || !snapshot.sector_count) return false;
    *out = (block_physical_range_t){expected_id, 0,
        snapshot.sector_count - 1U, snapshot.sector_size, snapshot.read_only};
    if (snapshot.type != BLOCK_DEVICE_PARTITION) return true;
    if (!gpt_get_partition_info(device, &part) ||
        !block_device_snapshot(part.parent, &parent) ||
        parent.id != part.parent_id ||
        parent.type == BLOCK_DEVICE_PARTITION ||
        !block_device_id_is_live(parent.id) ||
        parent.sector_size != snapshot.sector_size ||
        part.first_lba > part.last_lba || part.last_lba >= parent.sector_count ||
        part.last_lba - part.first_lba + 1U != snapshot.sector_count)
        return false;
    out->storage_id = parent.id;
    out->first_lba = part.first_lba;
    out->last_lba = part.last_lba;
    out->read_only |= parent.read_only;
    return true;
}


bool block_device_set_automount_suppressed(u64 id, bool suppressed)
{
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    u32 slot = block_find_slot_by_id(id);
    bool result = slot < BLOCK_MAX_DEVICES && devices[slot].online;
    if (result) devices[slot].automount_suppressed = suppressed;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return result;
}


bool block_device_automount_suppressed(u64 id)
{
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    u32 slot = block_find_slot_by_id(id);
    bool result = slot < BLOCK_MAX_DEVICES && devices[slot].online &&
                  devices[slot].automount_suppressed;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return result;
}


bool block_device_mark_gone(u64 id)
{
    u64 flags = spin_lock_irqsave(&block_registry_lock);
    u32 slot = block_find_slot_by_id(id);
    bool notify = false;
    if (slot == BLOCK_MAX_DEVICES) {
        spin_unlock_irqrestore(&block_registry_lock, flags);
        return false;
    }
    if (devices[slot].online) {
        devices[slot].online = false; /* registered -> retiring */
        retirement_notifying[slot] = notify = true;
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    /* Keep the registry slot pinned while VFS consumes its identity. Never
     * nest registry_lock with VFS locks or dispose filesystem objects in it. */
    if (notify) {
        vfs_device_removed(&devices[slot]);
        flags = spin_lock_irqsave(&block_registry_lock);
        retirement_notifying[slot] = false;
        spin_unlock_irqrestore(&block_registry_lock, flags);
    }
    return true;
}


bool block_unregister_by_id(u64 id)
{
    u32 slot;
    u64 flags;
    const char *event_name;

    if (!block_device_mark_gone(id)) return false;
    /* Retirement is permanent even if quiescence is unavailable. Callers
     * must retain backing state and retry teardown, never clear it on false. */
    if (!block_io_begin_quiesce()) return false;
    flags = spin_lock_irqsave(&block_registry_lock);
    slot = block_find_slot_by_id(id);
    if (slot == BLOCK_MAX_DEVICES || retirement_notifying[slot]) {
        spin_unlock_irqrestore(&block_registry_lock, flags);
        block_io_end_quiesce();
        return false;
    }
    event_name = block_type_name(devices[slot].type);
    memset(&devices[slot], 0, sizeof(devices[slot])); /* quiescent -> removed */
    device_count--;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    block_cache_invalidate_all();
    block_io_end_quiesce();
    ipc_publish_event(MG_EVENT_CLASS_DEVICE | MG_EVENT_CLASS_BLOCK,
                      MG_EVENT_BLOCK_REMOVED,
                      BLOCK_DEVICE_ID_BASE | (id + 1ULL), event_name);
    return true;
}


bool block_unregister_by_driver_data(void *driver_data)
{
    u64 id = 0, flags = spin_lock_irqsave(&block_registry_lock);
    bool found = false;
    for (u32 slot = 0; slot < BLOCK_MAX_DEVICES; slot++) {
        if (devices[slot].registered && devices[slot].driver_data == driver_data) {
            id = devices[slot].id; found = true; break;
        }
    }
    spin_unlock_irqrestore(&block_registry_lock, flags);
    return found && block_unregister_by_id(id);
}


static bool block_read_internal(
    block_device_t *device,
    u64 expected_id,
    bool bind_id,
    u64 lba,
    u32 sector_count,
    void *buffer)
{
    u32 byte_count;
    block_cache_entry_t *entry;
    block_physical_range_t range;

    if (!device)
    {
        return false;
    }

    if (!buffer)
    {
        return false;
    }

    if (!block_io_acquire())
        return false;

    if (!(bind_id ? block_device_matches_live_id(device, expected_id) :
          (device->id == expected_id && block_device_id_is_live(expected_id))) ||
        !device->read || !sector_count ||
        !block_device_physical_range(device, expected_id, &range) ||
        device->sector_size != range.sector_size ||
        lba > range.last_lba - range.first_lba ||
        (u64)sector_count - 1U > range.last_lba - range.first_lba - lba) {
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
            entry = block_cache_find(&range, range.first_lba + lba + offset, chunk);
            if (!entry) break;
            entry->age = ++block_cache_age;
            memcpy((u8 *)buffer + (u64)offset * device->sector_size,
                   entry->data + (range.first_lba + lba + offset - entry->lba) * device->sector_size,
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
            return block_device_id_is_live(expected_id) &&
                   block_device_id_is_live(range.storage_id);
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
    if (!block_device_is_live(device) ||
        !block_device_id_is_live(range.storage_id)) {
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
            entry = block_cache_find(&range, range.first_lba + lba + offset, chunk);
            if (!entry) entry = block_cache_victim();
            /* Parent recursion may already have cached a wider physical
             * subrange. Insertion replaces its coordinates with these bytes. */
            entry->storage_id = range.storage_id;
            entry->lba = range.first_lba + lba + offset;
            entry->sector_size = range.sector_size;
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
    return block_device_id_is_live(expected_id) &&
           block_device_id_is_live(range.storage_id);
}

bool block_read(block_device_t *device, u64 lba, u32 sector_count,
                void *buffer)
{
    u64 id;
    if (!block_capture_live_id(device, &id)) return false;
    return block_read_internal(device, id, false, lba, sector_count, buffer);
}

bool block_read_for_id(block_device_t *device, u64 expected_id, u64 lba,
                       u32 sector_count, void *buffer)
{
    if (!block_device_matches_live_id(device, expected_id)) return false;
    return block_read_internal(device, expected_id, true, lba, sector_count,
                               buffer);
}

static bool block_write_internal(
    block_device_t *device,
    u64 expected_id,
    bool bind_id,
    u64 lba,
    u32 sector_count,
    const void *buffer)
{
    block_physical_range_t range;
    if (!device)
    {
        return false;
    }

    if (!buffer)
    {
        return false;
    }

    if (!block_io_acquire())
        return false;

    if (!(bind_id ? block_device_matches_live_id(device, expected_id) :
          (device->id == expected_id && block_device_id_is_live(expected_id))) ||
        !device->write || device->read_only || !sector_count ||
        !block_device_physical_range(device, expected_id, &range) ||
        range.read_only ||
        device->sector_size != range.sector_size ||
        lba > range.last_lba - range.first_lba ||
        (u64)sector_count - 1U > range.last_lba - range.first_lba - lba) {
        block_io_release();
        return false;
    }

    block_stats.write_requests++;
    for (u32 i = 0; i < BLOCK_CACHE_ENTRY_COUNT; i++) {
        if (block_cache_range_overlaps(&block_cache[i], &range, range.first_lba + lba,
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
    return result && block_device_id_is_live(expected_id) &&
           block_device_id_is_live(range.storage_id);
}

bool block_write(block_device_t *device, u64 lba, u32 sector_count,
                 const void *buffer)
{
    u64 id;
    if (!block_capture_live_id(device, &id)) return false;
    return block_write_internal(device, id, false, lba, sector_count, buffer);
}

bool block_write_for_id(block_device_t *device, u64 expected_id, u64 lba,
                        u32 sector_count, const void *buffer)
{
    if (!block_device_matches_live_id(device, expected_id)) return false;
    return block_write_internal(device, expected_id, true, lba, sector_count,
                                buffer);
}

static bool block_flush_internal(block_device_t *device, u64 expected_id,
                                 bool bind_id)
{
    bool result;

    if (!block_io_acquire()) return false;
    if (!(bind_id ? block_device_matches_live_id(device, expected_id) :
          (device->id == expected_id && block_device_id_is_live(expected_id)))) {
        block_io_release();
        return false;
    }
    /* A missing callback is meaningful for backends whose writes complete
     * synchronously and which expose no separate cache-flush command. */
    block_callback_enter();
    result = !device->flush || device->flush(device);
    block_callback_leave();
    block_io_release();
    return result && block_device_id_is_live(expected_id);
}

bool block_flush(block_device_t *device)
{
    u64 expected_id;
    if (!block_capture_live_id(device, &expected_id)) return false;
    return block_flush_internal(device, expected_id, false);
}

bool block_flush_for_id(block_device_t *device, u64 expected_id)
{
    if (!block_device_matches_live_id(device, expected_id)) return false;
    return block_flush_internal(device, expected_id, true);
}

bool block_probe_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer)
{
    bool result, was_registered;
    u64 expected_id, flags;

    if (!device || !buffer || !sector_count) return false;
    flags = spin_lock_irqsave(&block_registry_lock);
    was_registered = device->registered;
    expected_id = device->id;
    spin_unlock_irqrestore(&block_registry_lock, flags);
    if (!block_io_acquire()) return false;
    if (!device->read || device->id != expected_id ||
        (was_registered && !block_device_id_is_live(expected_id))) {
        block_io_release();
        return false;
    }
    block_callback_enter();
    result = device->read(device, lba, sector_count, buffer);
    block_callback_leave();
    if (was_registered) result = result && block_device_id_is_live(expected_id);
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
