/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real dispatcher, exFAT formatter, VFS ownership and native syscall case.
 * Controlled registered device, heap, kernel locks, clock and block callbacks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <storage/gpt.h>
#include <mg/storage.h>
#include <mangrove_errors.h>
#include "../../../kernel/src/storage/exfat.c"
#include "../../../kernel/src/storage/format.c"
#include "../../../libc/src/time_convert.c"

static block_device_t device, alias, physical_parent;
static bool alias_registered;
static u64 alias_first;
static u8 (*media)[512];
static bool online, reject_mount_lock, reject_finish, replace_admission;
static bool replace_owner, readonly_owner;
static unsigned writes, reads, flushes, fail_write, fail_read, fail_flush;
static u64 fail_read_lba;
static unsigned remove_write, replace_write, replacement_writes;
static unsigned replace_read;
static bool partial_write, management_blocked, native_swap, system_managed;
static usize live_requested, peak_requested;
static unsigned allocations, fail_allocation;
static FILE *image;
static pthread_mutex_t metadata = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_changed = PTHREAD_COND_INITIALIZER;
static bool pause_writer, writer_paused, writer_resume;

#include "format_vfs.inc"

void *kmalloc(usize size) {
    if (++allocations == fail_allocation) return NULL;
    usize *p = malloc(size + sizeof(usize));
    assert(p); *p = size; live_requested += size;
    if (peak_requested < live_requested) peak_requested = live_requested;
    return p + 1;
}
void kfree(void *ptr) {
    if (!ptr) return;
    usize *p = (usize *)ptr - 1;
    assert(live_requested >= *p); live_requested -= *p; free(p);
}
void mutex_init(mutex_t *lock) { memset(lock, 0, sizeof(*lock)); }
bool mutex_lock(mutex_t *lock) {
    assert(!lock->owner); lock->owner = (void *)1;
    if(replace_owner) { device.id++; replace_owner=false; }
    if(readonly_owner) { device.read_only=true; readonly_owner=false; }
    return true;
}
bool mutex_try_lock(mutex_t *lock) {
    if (lock->owner || (lock == &vfs_mount_lock ? reject_mount_lock : reject_finish)) return false;
    lock->owner = (void *)1; return true;
}
bool mutex_unlock(mutex_t *lock) { assert(lock->owner); lock->owner = NULL; return true; }
u64 spin_lock_irqsave(spinlock_t *lock) { (void)lock; assert(!pthread_mutex_lock(&metadata)); return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) { (void)lock; assert(!flags && !pthread_mutex_unlock(&metadata)); }
bool block_io_begin_quiesce(void) {
    if (management_blocked) return false;
    if (replace_admission) { device.id++; replace_admission = false; }
    return true;
}
void block_io_end_quiesce(void) {}
bool block_device_snapshot(const block_device_t *dev, block_device_t *out) {
    if (!dev || !out || !online || (dev != &device && dev != &alias && dev != &physical_parent)) return false;
    *out = *dev; return true;
}
bool block_device_is_live(const block_device_t *dev) { return online && (dev == &device || dev == &alias); }
bool block_device_id_is_live(u64 id) { return online && (id == device.id || id == alias.id || id == physical_parent.id); }
u32 block_device_count(void) { return 1U + alias_registered + (device.type == BLOCK_DEVICE_PARTITION); }
block_device_t *block_get_device(u32 index) {
    if (!index) return &device;
    if (alias_registered && index == 1U) return &alias;
    return &physical_parent;
}
bool gpt_get_partition_info(block_device_t *dev, gpt_partition_info_t *out) {
    if (dev == &device && device.type == BLOCK_DEVICE_PARTITION) {
        *out = (gpt_partition_info_t){ .parent=&physical_parent, .parent_id=physical_parent.id, .first_lba=100U,
            .last_lba=100U + device.sector_count - 1U };
        return true;
    }
    if (dev != &alias) return false;
    *out = (gpt_partition_info_t){ .parent=device.type == BLOCK_DEVICE_PARTITION ? &physical_parent : &device,
        .parent_id=device.type == BLOCK_DEVICE_PARTITION ? physical_parent.id : device.id,
        .first_lba=alias_first, .last_lba=alias_first + alias.sector_count-1U };
    return true;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba, u32 count, void *out) {
    if (!online || dev != &device || id != device.id || !count || lba >= dev->sector_count || count > dev->sector_count-lba) return false;
    if (++reads == fail_read || (fail_read_lba >= lba && fail_read_lba - lba < count)) return false;
    if(reads==replace_read) { device.id++; return false; }
    if (image) return !fseeko(image, (off_t)(lba*512U), SEEK_SET) && fread(out,512U,count,image)==count;
    memcpy(out, media[lba], (usize)count*512U); return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count, const void *in) {
    if (!online || dev != &device || id != device.id || !count || lba >= dev->sector_count || count > dev->sector_count-lba) return false;
#ifndef FORMAT_BASELINE
    assert(storage_mutation_busy() && recovery_pins && recovery_pin_count == 1U);
    assert(recovery_pins->sb->operation_lock.owner);
#endif
    writes++;
    if (id != 93U) replacement_writes++;
    if (pause_writer && writes == 1U) {
        assert(!pthread_mutex_lock(&pause_lock)); writer_paused=true;
        assert(!pthread_cond_broadcast(&pause_changed));
        while(!writer_resume) assert(!pthread_cond_wait(&pause_changed,&pause_lock));
        assert(!pthread_mutex_unlock(&pause_lock));
    }
    bool failed = writes == fail_write;
    usize bytes = failed ? (partial_write ? (usize)count*256U : 0U) : (usize)count*512U;
    if (image) {
        assert(!fseeko(image,(off_t)(lba*512U),SEEK_SET));
        assert(fwrite(in,1U,bytes,image)==bytes && !fflush(image));
    } else memcpy(media[lba],in,bytes);
    if (writes == remove_write) online=false;
    if (writes == replace_write) device.id++;
    return !failed && online && id == device.id;
}
bool block_flush_for_id(block_device_t *dev, u64 id) {
    if (!online || dev != &device || id != device.id) return false;
    return ++flushes != fail_flush;
}
bool block_read(block_device_t *dev,u64 lba,u32 count,void *out) { return block_read_for_id(dev,dev->id,lba,count,out); }
bool block_write(block_device_t *dev,u64 lba,u32 count,const void *in) { return block_write_for_id(dev,dev->id,lba,count,in); }
bool block_flush(block_device_t *dev) { return block_flush_for_id(dev,dev->id); }
bool vfs_current_uid(u32 *out) { *out=7U; return true; }
bool entropy_random_u64(u64 *out) { *out=123U; return true; }
u64 timekeeping_boot_id(void) { return 1U; }
u64 timekeeping_monotonic_ms(void) { return 0U; }
/* Linked through driver operation tables, outside this formatting fixture. */
bool timekeeping_realtime_sample(timekeeping_realtime_sample_t *out) { (void)out; assert(0); return false; }
bool vfs_check_access(const vfs_node_t *node,u32 permission) { (void)node; (void)permission; assert(0); return false; }
void vfs_node_register(vfs_node_t *node) { (void)node; assert(0); }
void vfs_node_set_security(vfs_node_t *node,u32 owner,u32 permissions) { (void)node; (void)owner; (void)permissions; assert(0); }
void vfs_nodes_update_size(vfs_super_t *sb,u64 inode,u64 size) { (void)sb; (void)inode; (void)size; assert(0); }
int vfs_recovery_pin_begin_handoff(vfs_super_t *sb,vfs_recovery_handoff_t cb,void *ctx,u64 *id) { (void)sb; (void)cb; (void)ctx; (void)id; assert(0); return VFS_ERR_UNSUPPORTED; }

/* FAT32 admission probe is extracted verbatim; exFAT probe is compiled above.
 * MGFS's completion probe is a controlled signature/CRC dependency here. */
#include "format_fat_probe.inc"
static bool fixture_mgfs_probe(block_device_t *dev) {
    u8 header[4096];
    return block_read(dev,0,8,header) && !memcmp(header,"MGFSv1",6) &&
           crc64(header,200,192)==((u64)header[192] | ((u64)header[193]<<8) | ((u64)header[194]<<16) | ((u64)header[195]<<24) | ((u64)header[196]<<32) | ((u64)header[197]<<40) | ((u64)header[198]<<48) | ((u64)header[199]<<56));
}
vfs_fs_type_t *vfs_find_fs(const char *name) {
    static vfs_fs_type_t fat={.name="fat32",.probe=fat32_probe};
    static vfs_fs_type_t ex={.name="exfat",.probe=exfat_probe};
    static vfs_fs_type_t mg={.name="mgfs",.probe=fixture_mgfs_probe};
    return !strcmp(name,"fat32") ? &fat : !strcmp(name,"exfat") ? &ex : &mg;
}
#include "format_probe.inc"
#include "format_native.inc"

static void prepare(u64 sectors) {
    assert(!recovery_pins && !live_requested && !storage_mutation_busy());
    memset(media,0,70000U*512U); memset(&device,0,sizeof(device)); memset(&alias,0,sizeof(alias));
    device.id=93U; device.type=BLOCK_DEVICE_USB; device.sector_size=512U; device.sector_count=sectors;
    device.registered=device.online=online=true;
    alias=device; alias.id=194U; alias.type=BLOCK_DEVICE_PARTITION;
    physical_parent=device; physical_parent.id=700U; physical_parent.sector_count=150000U;
    alias_registered=false; alias_first=0U;
    next_recovery_pin_id=1U; reject_mount_lock=reject_finish=replace_admission=false;
    replace_owner=readonly_owner=false;
    fail_write=fail_read=fail_flush=remove_write=replace_write=0U;
    fail_read_lba=~(u64)0;
    replace_read=0U;
    reads=writes=flushes=replacement_writes=0U;
    allocations=fail_allocation=0U; peak_requested=0U;
    partial_write=management_blocked=native_swap=system_managed=false;
    memset(mount_table,0,sizeof(mount_table));
}
static int fixture_owner_unmount(vfs_super_t *sb) { (void)sb; return VFS_OK; }
static bool fixture_owner_finish(vfs_super_t *sb,void *context) { (void)sb; (void)context; return true; }
static u64 reserve_owner(block_device_t *dev,bool unresolved) {
    static const vfs_super_ops_t ops={.unmount=fixture_owner_unmount};
    vfs_super_t *sb; u64 pin;
    assert(storage_mutation_begin());
    assert(vfs_recovery_private_begin(dev,dev->id,&ops,fixture_owner_finish,NULL,&sb,&pin)==VFS_OK);
    if(unresolved) assert(vfs_recovery_pin_mark_unresolved(pin)==VFS_OK);
    vfs_recovery_private_release(sb); storage_mutation_end(); return pin;
}
/* Fixture-only teardown, never a production retry/recovery mechanism. */
static void discard_owner(void) {
    vfs_recovery_pin_t *p=recovery_pins;
    assert(p && recovery_pin_count==1U); vfs_super_t *sb=p->sb;
    recovery_pins=NULL; recovery_pin_count=0; kfree(p); vfs_super_release(sb);
    assert(!live_requested);
}
static void formatter_layout_regressions(void) {
    prepare(12000U);
    assert(exfat_format_device_for_id(&device, device.id) == VFS_OK);
    unsigned formatted_writes = writes;
    exfat_fs_t *mounted = exfat_new_state(&device);
    assert(mounted && writes == formatted_writes);
    assert(exfat_system_streams_check(mounted) == VFS_OK);
    u32 formatted_root = mounted->root_cluster;
    u32 formatted_heap = mounted->boot.heap_offset;
    u32 formatted_spc = mounted->sectors_per_cluster;
    u32 formatted_fat = mounted->boot.fat_offset;
    /* Independent check: bitmap cluster 2 ends; upcase 3 -> 4 -> EOC. */
    assert(exfat_le32(media[formatted_fat] + 2U * 4U) == 0xffffffffU);
    assert(exfat_le32(media[formatted_fat] + 3U * 4U) == 4U);
    assert(exfat_le32(media[formatted_fat] + 4U * 4U) == 0xffffffffU);
    exfat_free_state(mounted);
    assert(live_requested == 0U);
    exfat_put32(media[formatted_fat] + 3U * 4U, 0xffffffffU);
    assert(!exfat_new_state(&device)); /* legacy missing system FAT link */
    assert(live_requested == 0U && writes == formatted_writes);
    exfat_put32(media[formatted_fat] + 3U * 4U, 4U);
    /* Main flags select policy; backup flags are stale by specification. */
    exfat_put16(media[12] + 106U, 1U);
    mounted = exfat_new_state(&device);
    assert(mounted); exfat_free_state(mounted);
    exfat_put16(media[12] + 106U, 0U);
    for (u16 flag = 2U; flag <= 8U; flag *= 2U) {
        exfat_put16(media[0] + 106U, flag);
        assert(!exfat_new_state(&device));
        assert(live_requested == 0U && writes == formatted_writes);
    }
    exfat_put16(media[0] + 106U, 0U);
    fail_read_lba = exfat_le32(media[0] + 80U);
    assert(!exfat_new_state(&device));
    assert(live_requested == 0U && writes == formatted_writes);
    fail_read_lba = ~(u64)0;
    u64 root_lba = formatted_heap + (formatted_root - 2U) * formatted_spc;
    memcpy(media[root_lba] + 64U, media[root_lba], 32U);
    media[root_lba][65] = 1U; /* independently encoded second bitmap */
    exfat_put32(media[root_lba] + 64U + 20U, 6U);
    exfat_put32(media[formatted_fat] + 6U * 4U, 0xffffffffU);
    media[formatted_heap][0] |= 0x10U; /* distinct inactive bitmap cluster */
    for (u32 i = 0; i < formatted_spc; i++)
        memcpy(media[formatted_heap + 4U * formatted_spc + i],
               media[formatted_heap + i], 512U);
    u32 fat_sectors = exfat_le32(media[0] + 84U);
    for (u32 i = 0; i < fat_sectors; i++)
        memcpy(media[formatted_fat + fat_sectors + i],
               media[formatted_fat + i], 512U);

    for (u32 region = 0; region <= 12U; region += 12U) {
        media[region][110] = 2U;
        u32 sum = 0;
        for (u32 i = 0; i < 11U * 512U; i++) {
            if (i == 106U || i == 107U || i == 112U) continue;
            sum = (sum >> 1U) | (sum << 31U);
            sum += media[region + i / 512U][i % 512U];
        }
        for (u32 i = 0; i < 512U; i += 4U)
            exfat_put32(media[region + 11U] + i, sum);
    }
    for (u16 active = 0; active < 2U; active++) {
        exfat_put16(media[0] + 106U, active);
        exfat_boot_info_t geometry;
        assert(exfat_read_geometry(&device, device.id, &geometry));
        assert(geometry.fat_count == 2U && geometry.volume_flags == active);
        assert(!exfat_new_state(&device));
        assert(live_requested == 0U && writes == formatted_writes);
    }
    assert(!live_requested);
}

#ifndef FORMAT_BASELINE
static void rejected_routes(void) {
    static const char *names[]={"fat32","mgfs","exfat"};
    for(u32 n=0;n<3U;n++) {
        prepare(70000U);
        assert(storage_format_filesystem_for_id(&device,92U,names[n])==VFS_ERR_DEVICE_GONE && !writes && !reads && !live_requested);
        replace_admission=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_DEVICE_GONE && !writes && !reads && !replacement_writes && !live_requested);
        prepare(70000U); replace_owner=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_DEVICE_GONE && !writes && !reads && !live_requested);
        prepare(70000U); readonly_owner=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_ACCESS_DENIED && !writes && !reads && !live_requested);
        prepare(70000U); device.read_only=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_ACCESS_DENIED && !writes && !reads && !live_requested);
        device.read_only=false; management_blocked=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_BUSY && !writes && !live_requested);
        management_blocked=false;
        mount_table[0]=(vfs_mount_t){.active=true,.dev_id=device.id,.dev=&device};
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_BUSY && !writes && !reads && !live_requested);
        mount_table[0]=(vfs_mount_t){.active=true,.dev_id=alias.id,.dev=&alias};
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_BUSY && !writes && !reads && !live_requested);
        memset(mount_table,0,sizeof(mount_table));
        reserve_owner(&alias,false);
        usize retained=live_requested;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_BUSY && !writes && !reads && live_requested==retained);
        discard_owner(); reserve_owner(&device,true); retained=live_requested;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_BUSY && !writes && !reads && live_requested==retained);
        assert(exfat_format_device_for_id(&device,93U) != VFS_OK && !writes && !reads && live_requested==retained);
        discard_owner(); reject_mount_lock=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_BUSY && !writes && !reads && !live_requested);
        reject_mount_lock=false;
        recovery_pin_count=VFS_MAX_RECOVERY_PINS;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_NO_SPACE && !writes && !live_requested);
        recovery_pin_count=0U;
        native_swap=true;
        assert(native_format(n+1U)==MG_ERR_DEVICE_GONE && !writes && !replacement_writes && !live_requested);
    }
    prepare(12000U); device.read_only=true;
    assert(exfat_format_device_for_id(&device,93U) != VFS_OK && !writes && !reads && !live_requested);
    device.read_only=false; replace_admission=true;
    assert(exfat_format_device_for_id(&device,93U) != VFS_OK && !writes && !replacement_writes);
    prepare(12000U); system_managed=true;
    assert(native_format(MG_STORAGE_FILESYSTEM_EXFAT)==MG_ERR_ACCESS_DENIED && !writes && !reads);
    system_managed=false; requester.storage_management_session=false;
    assert(native_format_unauthorized()==MG_ERR_PRIVILEGE_REQUIRED && !writes);
    assert(storage_format_filesystem_for_id(&device,93U,"unsupported")==VFS_ERR_UNSUPPORTED && !writes);
}

static void uncertain_owner(void) {
    assert(recovery_pins && recovery_pin_count==1U && recovery_pins->state==VFS_RECOVERY_PIN_UNRESOLVED);
    storage_format_context_t *c=recovery_pins->sb->private_data;
    assert(c && c->mutated && c->snapshot.id==93U && !c->resolved && c->attempted_sectors>0U);
    assert(!storage_mutation_busy());
    unsigned before=writes;
    if(online && device.id==93U) {
        assert(storage_format_filesystem_for_id(&device,93U,c->filesystem)==VFS_ERR_BUSY);
        assert(exfat_format_device_for_id(&device,93U) != VFS_OK);
    }
    assert(writes==before && !replacement_writes);
    printf("retained formatting evidence: %zu bytes, phase=%u, pending=%u sectors\n",(size_t)live_requested,c->phase,c->attempted_sectors);
    discard_owner();
}
static void successful_and_faults(void) {
    static const char *names[]={"fat32","mgfs","exfat"};
    for(u32 n=0;n<3U;n++) {
        prepare(70000U);
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_OK && writes && flushes && !live_requested && !recovery_pins);
        unsigned total_allocations=allocations,total_reads=reads,total_writes=writes;
        printf("%s format: reads=%u writes=%u flushes=%u allocations=%u peak=%zu bytes\n",names[n],reads,writes,flushes,allocations,(size_t)peak_requested);
        prepare(70000U);
        assert(native_format(n+1U)==MG_OK && !recovery_pins && !live_requested);
        for(unsigned a=1;a<=total_allocations;a++) {
            prepare(70000U); fail_allocation=a;
            int result=storage_format_filesystem_for_id(&device,93U,names[n]);
            assert(result!=VFS_OK);
            if(writes) { assert(result==VFS_ERR_UNCERTAIN); uncertain_owner(); }
            else assert(!live_requested && !recovery_pins);
        }
        unsigned boundaries[]={1U,total_writes/2U,total_writes};
        for(u32 i=0;i<3U;i++) for(u32 partial=0;partial<2U;partial++) {
            prepare(70000U); fail_write=boundaries[i]; partial_write=partial;
            assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN);
            storage_format_context_t *c=recovery_pins->sb->private_data;
            assert(!c->pending_written && c->write_attempts==writes);
            if(partial) assert(!memcmp(media[c->attempted_lba],c->pending,(usize)c->attempted_sectors*256U));
            uncertain_owner();
        }
        prepare(70000U); fail_flush=1U;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN);
        assert(((storage_format_context_t *)recovery_pins->sb->private_data)->phase==FORMAT_FLUSHING);
        uncertain_owner();
        prepare(70000U); fail_read=total_reads;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN);
        uncertain_owner();
        prepare(70000U); remove_write=1U;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN);
        uncertain_owner();
        prepare(70000U); replace_write=1U;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN && !replacement_writes);
        uncertain_owner();
        prepare(70000U); replace_read=total_reads;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN && !replacement_writes);
        uncertain_owner();
        prepare(70000U); reject_finish=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[n])==VFS_ERR_UNCERTAIN && recovery_pins && !replacement_writes);
        reject_finish=false; discard_owner();
    }
    prepare(8U);
    assert(storage_format_filesystem_for_id(&device,93U,"exfat")!=VFS_OK && !writes && !recovery_pins && !live_requested);
}
static void *format_worker(void *unused) {
    (void)unused; return (void *)(usize)storage_format_filesystem_for_id(&device,93U,"exfat");
}
static void registered_ranges(void) {
    static const char *names[]={"fat32","mgfs","exfat"};
    for(u32 i=0;i<3U;i++) {
        prepare(70000U); alias_registered=true;
        assert(storage_format_filesystem_for_id(&device,93U,names[i])==VFS_ERR_BUSY);
        assert(!writes && !reads && !flushes && !allocations && !recovery_pins);
        assert(exfat_format_device_for_id(&device,93U)==VFS_ERR_BUSY && !writes);
        assert(exfat_set_label_for_id(&device,93U,"DENIED")==VFS_ERR_BUSY);
        assert(!writes && !reads && !allocations);
        prepare(70000U); device.type=BLOCK_DEVICE_PARTITION; alias_registered=true;
        alias_first=69999U; alias.sector_count=10U; /* Partial sibling overlap. */
        assert(storage_format_filesystem_for_id(&device,93U,names[i])==VFS_ERR_BUSY);
        assert(!writes && !reads && !allocations && !recovery_pins);
        prepare(70000U); device.type=BLOCK_DEVICE_PARTITION; alias_registered=true;
        alias_first=70100U; alias.sector_count=100U; /* Exact adjacent boundary. */
        assert(storage_format_filesystem_for_id(&device,93U,names[i])==VFS_OK);
        assert(writes && !recovery_pins && !live_requested);
    }
    prepare(70000U); device.type=BLOCK_DEVICE_PARTITION; physical_parent.read_only=true;
    assert(exfat_format_device_for_id(&device,93U)==VFS_ERR_ACCESS_DENIED);
    assert(!writes && !reads && !allocations);
}
static void concurrent_format(void) {
    prepare(12000U); pause_writer=true; writer_paused=writer_resume=false;
    pthread_t worker; assert(!pthread_create(&worker,NULL,format_worker,NULL));
    assert(!pthread_mutex_lock(&pause_lock));
    while(!writer_paused) assert(!pthread_cond_wait(&pause_changed,&pause_lock));
    assert(storage_format_filesystem_for_id(&device,93U,"mgfs")==VFS_ERR_BUSY);
    assert(vfs_device_already_mounted(&device)); /* Private reservation excludes mounting. */
    writer_resume=true; assert(!pthread_cond_broadcast(&pause_changed));
    assert(!pthread_mutex_unlock(&pause_lock));
    void *result; assert(!pthread_join(worker,&result) && (usize)result==VFS_OK);
    pause_writer=false; assert(!live_requested && !recovery_pins);
}
#endif
int main(int argc,char **argv) {
    media=calloc(70000U,512U); assert(media);
#ifdef FORMAT_BASELINE
    (void)argc; (void)argv;
    prepare(12000U); native_swap=true;
    assert(native_format(MG_STORAGE_FILESYSTEM_EXFAT)==MG_OK && device.id==94U && replacement_writes>0U);
    printf("baseline wrong-device format: selected=93 actual=94 writes=%u\n",replacement_writes);
    prepare(12000U); reserve_owner(&device,true);
    assert(exfat_format_device(&device) && writes>0U && recovery_pin_count==1U);
    printf("baseline unprotected direct formatter: unresolved owner present, writes=%u\n",writes);
    discard_owner();
#else
    if(argc==4 && !strcmp(argv[1],"--image")) {
        prepare(70000U); image=fopen(argv[3],"r+b"); assert(image);
        assert(storage_format_filesystem_for_id(&device,93U,argv[2])==VFS_OK && !live_requested);
        assert(!fclose(image)); image=NULL;
    } else {
        formatter_layout_regressions(); rejected_routes(); registered_ranges(); successful_and_faults(); concurrent_format();
    }
    puts("production formatting: native identity, mandatory admission, retained failures and concurrency passed");
#endif
    free(media); return 0;
}
