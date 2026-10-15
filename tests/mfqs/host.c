/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Controlled dependencies for complete production VFS/filesystem units.
 * No registry races, process authorization, scheduler or block-cache coverage. */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <process.h>
#include <storage/gpt.h>
#include <storage/format_internal.h>
#include <timekeeping.h>
#include <entropy.h>
#include "host.h"

#ifdef MFQS_ADMISSION_TEST
#define HEAP_LIMIT (16U * 1024U * 1024U)
#else
#define HEAP_LIMIT (8U * 1024U * 1024U)
#endif
#define HEAP_SLOTS 8192U
#define IMAGE_LIMIT (64U * 1024U * 1024U)
static struct { void *pointer; usize bytes; } heap[HEAP_SLOTS];
static usize heap_bytes;
static int media_fd = -1;
static char media_path[4096];
static block_device_t device;
static mfqs_metrics_t metrics;
#ifdef MFQS_ADMISSION_TEST
static char fault_operation;
static u64 fault_count;
static u32 fault_repetitions;
static bool fault_torn, fault_remove;
static void (*read_hook)(void);
void mfqs_fail_after(char operation, u64 count, bool torn, bool remove)
{ fault_operation = operation; fault_count = count; fault_repetitions = count ? 1U : 0U;
  fault_torn = torn; fault_remove = remove; }
void mfqs_fail_repeated(char operation, u64 count, u32 repetitions)
{ mfqs_fail_after(operation, count, false, false); fault_repetitions = repetitions; }
void mfqs_read_hook(void (*hook)(void)) { read_hook = hook; }
static bool fault(char operation)
{
    if (operation != fault_operation || !fault_count || --fault_count) return false;
    if (fault_remove) device.online = false;
    if (fault_repetitions && --fault_repetitions) fault_count = 1;
    return true;
}
#endif

void mfqs_metrics(mfqs_metrics_t *out)
{ *out = metrics; out->heap_live = heap_bytes; }

void *kmalloc(usize size)
{
    metrics.heap_requests++;
#ifdef MFQS_ADMISSION_TEST
    if (fault('a')) return NULL;
#endif
    if (!size || size > HEAP_LIMIT - heap_bytes) return NULL;
    unsigned slot = 0;
    while (slot < HEAP_SLOTS && heap[slot].pointer) slot++;
    if (slot == HEAP_SLOTS) return NULL;
    void *pointer = malloc((size_t)size);
    if (!pointer) return NULL;
    heap[slot].pointer = pointer;
    heap[slot].bytes = size;
    heap_bytes += size;
    if (heap_bytes > metrics.heap_peak) metrics.heap_peak = heap_bytes;
    return pointer;
}

bool mfqs_heap_contains(const void *pointer)
{
    for (unsigned i = 0; i < HEAP_SLOTS; i++)
        if (heap[i].pointer == pointer && pointer) return true;
    return false;
}

void kfree(void *pointer)
{
    if (!pointer) return;
    unsigned slot = 0;
    while (slot < HEAP_SLOTS && heap[slot].pointer != pointer) slot++;
    assert(slot < HEAP_SLOTS);
    heap_bytes -= heap[slot].bytes;
    heap[slot].pointer = NULL;
    heap[slot].bytes = 0;
    free(pointer);
}

void mfqs_heap_release_nodes(vfs_node_t **nodes, unsigned count)
{
    /* VFS intentionally retains cached nodes after real driver teardown.
     * Reap only these verified inert nodes at host-process exit. Every other
     * allocation must already have been released by production teardown. */
    for (unsigned i = 0; i < count; i++) {
        assert(!vfs_node_is_live(nodes[i]));
        kfree(nodes[i]);
    }
    assert(heap_bytes == 0);
    for (unsigned i = 0; i < HEAP_SLOTS; i++) assert(!heap[i].pointer);
}

struct kernel_thread *thread_current(void) { return (void *)1; }

void spinlock_init(spinlock_t *lock) { lock->value = 0; }
u64 spin_lock_irqsave(spinlock_t *lock)
{ assert(!lock->value); lock->value = 1; return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ assert(lock->value && !flags); lock->value = 0; }
void mutex_init(mutex_t *mutex) { memset(mutex, 0, sizeof(*mutex)); }
bool mutex_try_lock(mutex_t *mutex)
{
    if (mutex->owner) return false;
    mutex->owner = (void *)1;
    return true;
}
bool mutex_lock(mutex_t *mutex)
{ assert(!mutex->owner); return mutex_try_lock(mutex); }
bool mutex_unlock(mutex_t *mutex)
{ assert(mutex->owner); mutex->owner = NULL; return true; }

/* Single-threaded admission dependency; management remains out of scope. */
static bool quiesced;
bool block_io_begin_quiesce(void)
{ if (quiesced) return false; quiesced = true; return true; }
void block_io_end_quiesce(void) { assert(quiesced); quiesced = false; }

/* The foundation runs as a trusted kernel caller. Any credential-dependent
 * route is a scope violation, not a permissive authorization fallback. */
process_t *process_current(void) { return NULL; }
bool process_get_credentials(const process_t *p, process_credentials_t *out)
{ (void)p; (void)out; abort(); }
bool identity_credentials_valid(const process_credentials_t *c)
{ (void)c; abort(); }
bool identity_credentials_is_admin(const process_credentials_t *c)
{ (void)c; abort(); }
bool identity_credentials_has_privilege(const process_credentials_t *c,
                                         identity_privilege_t p)
{ (void)c; (void)p; abort(); }
bool identity_registry_lookup_uid(mg_uid_t uid, user_identity_t *out)
{ (void)uid; (void)out; abort(); }
void kprint_debug(const char *format, ...) { (void)format; }

/* Explicit simulated trusted UTC, not RTC detection or production clock
 * policy. Native time conversion remains in the production translation unit. */
bool timekeeping_realtime_sample(timekeeping_realtime_sample_t *out)
{
    *out = (timekeeping_realtime_sample_t){0}; /* 2000-01-01 00:00:00 UTC */
    out->resolution_ns = MG_TIME_NANOSECONDS_PER_SEC;
    return true;
}
u64 timekeeping_boot_id(void) { abort(); }
u64 timekeeping_monotonic_ms(void) { abort(); }
bool entropy_random_u64(u64 *out) { (void)out; abort(); }

/* External tools construct fixtures. No management or raw formatting path
 * is admitted by this adapter; unexpectedly reaching one is a scope failure. */
bool storage_mutation_begin(void) { abort(); }
void storage_mutation_end(void) { abort(); }
int storage_management_range_preflight(block_device_t *dev, u64 id, u64 lba, u64 n)
{ (void)dev; (void)id; (void)lba; (void)n; abort(); }
int storage_format_execute_for_id(block_device_t *dev, u64 id,
    const char *name, storage_format_writer_t writer)
{ (void)dev; (void)id; (void)name; (void)writer; abort(); }
const block_device_t *storage_format_target(const storage_format_context_t *context)
{ (void)context; abort(); }
bool storage_format_read(storage_format_context_t *context, u64 lba, u32 n, void *out)
{ (void)context; (void)lba; (void)n; (void)out; abort(); }
bool storage_format_write(storage_format_context_t *context, u64 lba, u32 n, const void *in)
{ (void)context; (void)lba; (void)n; (void)in; abort(); }

block_device_t *mfqs_device(void) { return &device; }
bool block_device_id_is_live(u64 id)
{ return media_fd >= 0 && device.online && device.registered && id == device.id; }
bool block_device_is_live(const block_device_t *dev)
{ return dev == &device && block_device_id_is_live(dev->id); }
bool block_device_snapshot(const block_device_t *dev, block_device_t *out)
{ if (!block_device_is_live(dev)) return false; *out = *dev; return true; }
bool gpt_get_partition_info(block_device_t *dev, gpt_partition_info_t *out)
{ (void)dev; (void)out; abort(); } /* No partition descriptors are fabricated. */

bool mfqs_media_open(const char *path)
{
    struct stat status;
    if (media_fd >= 0 || strlen(path) >= sizeof(media_path)) return false;
    int fd = open(path, O_RDWR | O_NOFOLLOW);
    if (fd < 0) return false;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || status.st_size > IMAGE_LIMIT ||
        status.st_size % 4096) { close(fd); return false; }
    if (path != media_path) strcpy(media_path, path);
    media_fd = fd;
    memset(&device, 0, sizeof(device));
    device.id = BLOCK_DEVICE_ID_BASE;
    device.type = BLOCK_DEVICE_RAM;
    device.sector_size = 512;
    device.sector_count = (u64)status.st_size / 512U;
    device.registered = device.online = true;
    return true;
}

void mfqs_media_close(void)
{
    assert(media_fd >= 0);
    assert(fsync(media_fd) == 0);
    assert(close(media_fd) == 0);
    media_fd = -1;
}

bool mfqs_media_reopen(void)
{
    mfqs_media_close();
    return mfqs_media_open(media_path);
}

static bool media_range(block_device_t *dev, u64 id, u64 lba, u32 sectors)
{
    return block_device_is_live(dev) && id == device.id && sectors &&
        lba <= device.sector_count && sectors <= device.sector_count - lba;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba,
                       u32 sectors, void *buffer)
{
    if (!media_range(dev, id, lba, sectors)) return false;
    metrics.reads++;
    metrics.read_sectors += sectors;
#ifdef MFQS_ADMISSION_TEST
    if (read_hook) read_hook();
    if (fault('r')) return false;
#endif
    size_t size = (size_t)sectors * 512U;
    return pread(media_fd, buffer, size, (off_t)(lba * 512U)) == (ssize_t)size;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba,
                        u32 sectors, const void *buffer)
{
    if (!media_range(dev, id, lba, sectors) || device.read_only) return false;
    metrics.writes++;
    metrics.write_sectors += sectors;
    size_t size = (size_t)sectors * 512U;
#ifdef MFQS_ADMISSION_TEST
    if (fault('w')) {
        if (fault_torn) assert(pwrite(media_fd, buffer, size / 2,
                                     (off_t)(lba * 512U)) == (ssize_t)(size / 2));
        return false;
    }
#endif
    return pwrite(media_fd, buffer, size, (off_t)(lba * 512U)) == (ssize_t)size;
}

bool block_read(block_device_t *dev, u64 lba, u32 sectors, void *buffer)
{ return block_read_for_id(dev, dev->id, lba, sectors, buffer); }
bool block_flush_for_id(block_device_t *dev, u64 id)
{
    if (!block_device_is_live(dev) || id != device.id) return false;
    metrics.flushes++;
#ifdef MFQS_ADMISSION_TEST
    if (fault('f')) return false;
#endif
    return fsync(media_fd) == 0;
}
