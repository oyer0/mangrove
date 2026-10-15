/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production block/VFS/MGFS/GPT and label dispatcher. Host transport,
 * thread context, parking, IRQs, heap and syscall authorization are controlled. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <scheduler.h>
#include <ipc.h>
#include <mg/storage.h>
#include <mangrove_errors.h>
#include "../kernel/src/vfs.c"
#include "../kernel/src/storage/format.c"

extern int mgfs_init(void);
static _Thread_local kernel_thread_t caller;
static bool reject_finish;
static bool remove_finish;
static bool remove_finish_parent;
static bool cancel_lock;
kernel_thread_t *thread_current(void) { return &caller; }
void spinlock_init(spinlock_t *lock) { lock->value = 0; }
u64 spin_lock_irqsave(spinlock_t *lock)
{ while (__atomic_exchange_n(&lock->value, 1, __ATOMIC_ACQUIRE)) sched_yield(); return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ assert(!flags); __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE); }
void mutex_init(mutex_t *lock) { memset(lock, 0, sizeof(*lock)); }
bool mutex_try_lock(mutex_t *lock)
{
    if (remove_finish && recovery_pins &&
        lock == &recovery_pins->sb->operation_lock &&
        ((mgfs_label_transaction_t *)recovery_pins->sb->private_data)->resolved) {
        remove_finish = false;
        assert(!block_unregister_by_id(remove_finish_parent ? recovery_pins->storage_id : recovery_pins->device_id));
    }
    if (reject_finish && recovery_pins &&
        lock == &recovery_pins->sb->operation_lock &&
        ((mgfs_label_transaction_t *)recovery_pins->sb->private_data)->resolved) return false;
    u64 flags = spin_lock_irqsave(&lock->state_lock);
    bool acquired = !lock->owner;
    if (acquired) lock->owner = thread_current();
    spin_unlock_irqrestore(&lock->state_lock, flags);
    return acquired;
}
bool mutex_lock(mutex_t *lock)
{
    if (cancel_lock && recovery_pins && lock == &recovery_pins->sb->operation_lock) {
        cancel_lock = false; return false;
    }
    while (!mutex_try_lock(lock)) sched_yield();
    return true;
}
bool mutex_unlock(mutex_t *lock)
{
    u64 flags = spin_lock_irqsave(&lock->state_lock);
    assert(lock->owner == thread_current()); lock->owner = NULL;
    spin_unlock_irqrestore(&lock->state_lock, flags); return true;
}
static usize live_heap, peak_heap;
static unsigned allocations, fail_allocation;
static bool pause_pin;
static void pause_callback(void);
void *kmalloc(usize size)
{
    if (++allocations == fail_allocation) return NULL;
    if (pause_pin && size == sizeof(vfs_recovery_pin_t)) pause_callback();
    usize *memory = calloc(1, sizeof(usize) + size); assert(memory);
    *memory = size; live_heap += size;
    if (live_heap > peak_heap) peak_heap = live_heap;
    return memory + 1;
}
void kfree(void *pointer)
{ if (pointer) { usize *memory = (usize *)pointer - 1; live_heap -= *memory; free(memory); } }
void kprint(const char *format, ...) { (void)format; }
void kprint_debug(const char *format, ...) { (void)format; }
void ipc_publish_event(u32 c, u16 t, u64 r, const char *n) { (void)c; (void)t; (void)r; (void)n; }
bool entropy_random_u64(u64 *out) { *out = 123456789; return true; }
u64 timekeeping_boot_id(void) { return 1; }
u64 timekeeping_monotonic_ms(void) { return 1; }
process_t *process_current(void) { return NULL; }
bool process_get_credentials(const process_t *p,process_credentials_t *c) { (void)p; (void)c; abort(); }
bool identity_credentials_valid(const process_credentials_t *c) { (void)c; abort(); }
bool process_any_cwd_under_path(const char *path) { (void)path; return false; }
int fat32_set_label_for_id(block_device_t *d,u64 id,const char *s)
{ (void)d; (void)id; (void)s; abort(); }
int exfat_set_label_for_id(block_device_t *d,u64 id,const char *s)
{ (void)d; (void)id; (void)s; abort(); }
int exfat_format_device_for_id(block_device_t *d,u64 id)
{ (void)d; (void)id; abort(); }

static u8 *media, *before;
static usize media_bytes;
static block_device_t source, *device;
static u64 volume_lba;
static unsigned reads, writes, flushes, fail_read, fail_write, fail_flush;
static usize partial_bytes;
static bool drop_write, corrupt_readback, remove_write;
static bool ordinary_mode;
static bool replace_dispatch;
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_condition = PTHREAD_COND_INITIALIZER;
static bool pause_write, pause_read, paused, resume_writer;
static void pause_callback(void)
{
    assert(!pthread_mutex_lock(&pause_lock)); paused = true;
    assert(!pthread_cond_broadcast(&pause_condition));
    while (!resume_writer) assert(!pthread_cond_wait(&pause_condition, &pause_lock));
    assert(!pthread_mutex_unlock(&pause_lock));
}
static bool read_media(block_device_t *dev,u64 lba,u32 count,void *out)
{
    (void)dev; reads++;
    if (pause_read) pause_callback();
    assert(lba * 512U <= media_bytes && count * 512ULL <= media_bytes - lba * 512U);
    if (reads == fail_read) return false;
    memcpy(out, media + lba * 512U, count * 512U);
    if (corrupt_readback && writes && lba == 0) ((u8 *)out)[216] ^= 1;
    return true;
}
static bool write_media(block_device_t *dev,u64 lba,u32 count,const void *in)
{
    writes++;
    assert((ordinary_mode || storage_mutation_busy()) && recovery_pins && recovery_pins->sb->operation_lock.owner);
    assert(count == 8 && (ordinary_mode || lba == volume_lba));
    if (pause_write) pause_callback();
    usize length = writes == fail_write ? partial_bytes : count * 512U;
    assert(length <= count * 512U);
    if (!drop_write) memcpy(media + lba * 512U, in, length);
    if (remove_write) assert(block_device_mark_gone(dev->id));
    return writes != fail_write;
}
static bool flush_media(block_device_t *dev)
{ (void)dev; flushes++; return flushes != fail_flush; }

#include "mgfs_label_native.inc"

static u32 pin_count(void)
{
    u32 count;
    int result = vfs_recovery_pins_snapshot(NULL,0,&count);
    assert(result == VFS_OK || result == VFS_ERR_BUFFER_TOO_SMALL);
    return count;
}
static bool finish_allowed = true;
static bool pause_handoff;
static bool handoff(vfs_super_t *sb, void *context)
{ (void)sb; (void)context; if (pause_handoff) pause_callback(); return finish_allowed; }
static int dispose(vfs_super_t *sb) { (void)sb; return VFS_OK; }
static const vfs_super_ops_t owner_ops = {.unmount = dispose};

static void retained(void)
{
    assert(pin_count() == 1 && recovery_pins->state == VFS_RECOVERY_PIN_UNRESOLVED);
    mgfs_label_transaction_t *tx = recovery_pins->sb->private_data;
    assert(tx->original_read && tx->proposed_ready && tx->write_attempted);
    const u8 *original = before + volume_lba * 512;
    assert(!memcmp(tx->original,original,4096));
    assert(!memcmp(tx->proposed,original,200) && !memcmp(tx->proposed + 288,original + 288,4096 - 288));
    assert(tx->pin == recovery_pins->id && recovery_pins->sb->reference_count == 1);
    assert(recovery_pins->sb->state == VFS_SUPER_DETACHING);
    assert(live_heap == sizeof(*tx) + sizeof(vfs_super_t) + sizeof(vfs_recovery_pin_t));
    u64 saved = device->id;
    void *backing = device->driver_data;
    if (device->online) {
        assert(vfs_format_preflight(device) == VFS_ERR_BUSY);
        assert(gpt_initialize_device_for_id(device,saved) == VFS_ERR_BUSY);
        assert(storage_format_filesystem_for_id(device,saved,"mgfs") == VFS_ERR_BUSY);
        assert(storage_set_filesystem_label_for_id(device,saved,"mgfs","RETRY") == VFS_ERR_BUSY);
        assert(vfs_mount_root("mgfs",device) == VFS_ERR_INVALID_PARAM);
    }
    u64 pin = recovery_pins->id;
    assert(!block_unregister_by_id(saved));
    assert(device->registered && !device->online && device->id == saved && device->driver_data == backing);
    block_device_t alias = source; alias.registered = false;
    assert(!block_register(&alias));
    assert(!block_unregister_by_id(saved));
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_UNCERTAIN);
    assert(pin_count() == 1 && !memcmp(tx->original,original,4096));
}

static void partition_lifetime(bool failed, bool parent_finish)
{
    block_device_t *parent = device;
    assert(gpt_scan_device(parent));
    device = block_get_device(1);
    block_device_t *sibling = block_get_device(2);
    assert(device && sibling && device->type == BLOCK_DEVICE_PARTITION);
    gpt_partition_info_t info, other;
    assert(gpt_get_partition_info(device,&info) && gpt_get_partition_info(sibling,&other));
    volume_lba = info.first_lba;
    u8 cached[4096];
    assert(block_read_for_id(parent,parent->id,volume_lba,8,cached)); /* Prime physical alias cache. */
    if (parent_finish) remove_finish = remove_finish_parent = true;
    else if (failed) { fail_write = 1; partial_bytes = 217; }
    int result = native_label("NEW");
    if (parent_finish) {
        assert(result == MG_ERR_UNCERTAIN && pin_count() == 1);
        mgfs_label_transaction_t *tx = recovery_pins->sb->private_data;
        assert(tx->write_confirmed && tx->flush_confirmed && tx->readback_confirmed);
        assert(tx->handoff_attempted && tx->handoff_result == VFS_ERR_UNCERTAIN);
        assert(parent->registered && !parent->online && device->online);
        assert(!block_unregister_by_id(device->id) && device->registered);
        block_device_t duplicate = source; duplicate.registered = false;
        assert(!block_register(&duplicate));
        puts("PARTITION parent removal during final handoff retains confirmed images and both registrations");
        return;
    }
    if (failed) {
        assert(result == MG_ERR_UNCERTAIN && pin_count() == 1);
        assert(vfs_format_preflight(parent) == VFS_ERR_BUSY);
        assert(vfs_format_preflight(sibling) == VFS_OK);
        assert(gpt_initialize_device_for_id(parent,parent->id) == VFS_ERR_BUSY);
        assert(storage_set_filesystem_label_for_id(parent,parent->id,"mgfs","RACE") == VFS_ERR_BUSY);
        /* Read-only alias inspection remains valid; cached old bytes must be gone. */
        assert(block_read_for_id(parent,parent->id,volume_lba,8,cached));
        assert(!memcmp(cached,media + volume_lba * 512,4096) && cached[216] == 'N');
        volume_lba = other.first_lba; fail_write = 0;
        assert(storage_set_filesystem_label_for_id(sibling,sibling->id,"mgfs","SIBLING") == VFS_OK);
        assert(pin_count() == 1);
        volume_lba = info.first_lba;
        assert(!block_unregister_by_id(parent->id));
        assert(parent->registered && !parent->online && parent->driver_data == media);
        assert(!block_unregister_by_id(device->id));
        assert(device->registered && !device->online);
        block_device_t duplicate = source; duplicate.registered = false;
        assert(!block_register(&duplicate));
        assert(recovery_pins->storage_id == source.id && recovery_pins->device_id == device->id);
        assert(!block_read_for_id(parent,parent->id,info.first_lba,8,cached));
        puts("PARTITION uncertain child retains logical and physical registrations; parent alias blocked, disjoint sibling permitted, cached old bytes invalidated");
    } else {
        assert(result == MG_OK && !pin_count());
        assert(vfs_mount_root("mgfs",device) == VFS_OK);
        vfs_node_t *root = vfs_get_root_node();
        assert(vfs_unmount_device(device) == VFS_OK); kfree(root);
        assert(gpt_remove_partitions_for_parent(parent));
        assert(block_unregister_by_id(source.id) && !live_heap);
        puts("PARTITION confirmed label, production GPT mapping, remount and complete teardown");
    }
}

static void *worker(void *opaque)
{ int *result = opaque; *result = storage_set_filesystem_label_for_id(device,source.id,"mgfs","NEW"); return NULL; }
static vfs_super_t *pin_super;
static void *pin_worker(void *opaque)
{
    u64 pin = 0;
    assert(vfs_super_operation_lock(pin_super));
    *(int *)opaque = vfs_recovery_pin_begin(pin_super,&pin);
    vfs_super_operation_unlock(pin_super);
    assert(!pin);
    return NULL;
}
static void *mount_worker(void *opaque)
{ *(int *)opaque = vfs_mount_root("mgfs",device); return NULL; }
typedef struct { u64 pin; int result; } finish_argument_t;
static void *finish_worker(void *opaque)
{ finish_argument_t *arg = opaque; arg->result = vfs_recovery_pin_finish_handoff(arg->pin); return NULL; }
static void wait_paused(void)
{
    assert(!pthread_mutex_lock(&pause_lock));
    while (!paused) assert(!pthread_cond_wait(&pause_condition,&pause_lock));
    assert(!pthread_mutex_unlock(&pause_lock));
}
static void resume_paused(pthread_t thread)
{
    assert(!pthread_mutex_lock(&pause_lock)); resume_writer = true;
    assert(!pthread_cond_broadcast(&pause_condition)); assert(!pthread_mutex_unlock(&pause_lock));
    assert(!pthread_join(thread,NULL));
}

static void registration_lifetime(void)
{
    u64 pin; vfs_super_t *owner;
    assert(storage_mutation_begin());
    assert(vfs_recovery_private_begin(device,device->id,&owner_ops,handoff,NULL,&owner,&pin) == VFS_OK);
    storage_mutation_end();
    u64 id = device->id;
    assert(!block_unregister_by_id(id));
    assert(device->registered && !device->online && vfs_registration_retained(id));
    block_device_t duplicate = source; duplicate.registered = false;
    assert(!block_register(&duplicate));
    /* A confirmed owner can finish offline. Final teardown becomes legal only
     * after its registry handoff; unresolved owners can never use this path. */
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    vfs_recovery_private_release(owner);
    assert(!vfs_registration_retained(id) && !pin_count());
    assert(block_unregister_by_id(id) && block_register(&duplicate));
    assert(duplicate.id != id && block_get_device(0) == device);
    assert(!block_read_for_id(device,id,0,1,before));
    assert(block_unregister_by_id(duplicate.id));
    assert(!live_heap);
}

int main(int argc,char **argv)
{
    assert(argc == 6);
    FILE *file = fopen(argv[1],"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); media_bytes = (usize)ftell(file); rewind(file);
    media = malloc(media_bytes); before = malloc(media_bytes); assert(media && before);
    assert(fread(media,1,media_bytes,file) == media_bytes); fclose(file);
    memcpy(before,media,media_bytes);
    vfs_init(); assert(mgfs_init() == VFS_OK);
    source = (block_device_t){.type=BLOCK_DEVICE_USB,.sector_size=512,.sector_count=media_bytes / 512,
        .driver_data=media,.read=read_media,.write=write_media,.flush=flush_media};
    assert(block_register(&source)); device = block_get_device(0);
    const char *mode = argv[3]; unsigned ordinal = (unsigned)strtoul(argv[4],NULL,10);
    partial_bytes = (usize)strtoul(argv[5],NULL,10);
    if (!strcmp(mode,"partition") || !strcmp(mode,"partition-fail") || !strcmp(mode,"partition-finish")) {
        partition_lifetime(!strcmp(mode,"partition-fail"),!strcmp(mode,"partition-finish")); goto done;
    }
    if (!strcmp(mode,"parent-retire")) {
        block_device_t *parent = device;
        assert(gpt_scan_device(parent)); device = block_get_device(1);
        assert(vfs_mount_root("mgfs",device) == VFS_OK);
        vfs_node_t *root = vfs_get_root_node(); pin_super = root->super;
        assert(vfs_super_retain(pin_super));
        pause_pin = true;
        pthread_t thread; int result;
        assert(!pthread_create(&thread,NULL,pin_worker,&result)); wait_paused();
        assert(block_unregister_by_id(source.id));
        resume_paused(thread); pause_pin = false;
        assert(result == VFS_ERR_DEVICE_GONE && !pin_count());
        assert(block_unregister_by_id(device->id));
        vfs_super_release(pin_super); kfree(root); assert(!live_heap);
        puts("PARENT RETIRE pin admission rechecks physical-parent liveness after allocation; no late owner can outlive final teardown");
        goto done;
    }
    if (!strcmp(mode,"pending")) {
        pause_read = true;
        pthread_t thread; int result;
        assert(!pthread_create(&thread,NULL,mount_worker,&result)); wait_paused();
        assert(vfs_registration_retained(source.id) && pending_mount.active);
        assert(!block_unregister_by_id(source.id));
        resume_paused(thread); pause_read = false;
        assert(result != VFS_OK && !pending_mount.active && !pin_count());
        assert(!vfs_registration_retained(source.id) && block_unregister_by_id(source.id) && !live_heap);
        puts("PENDING mount reservation retains identity until failed validation releases it"); goto done;
    }
    if (!strcmp(mode,"slots")) {
        static u8 backing[BLOCK_MAX_DEVICES];
        u64 ids[BLOCK_MAX_DEVICES]; ids[0] = source.id;
        for (unsigned i=0;i<BLOCK_MAX_DEVICES;i++) {
            block_device_t candidate = source; candidate.registered = false;
            if (i) { candidate.driver_data = &backing[i]; assert(block_register(&candidate)); ids[i] = candidate.id; }
            block_device_t *target = block_get_device(i);
            vfs_super_t *owner; u64 pin;
            assert(storage_mutation_begin());
            assert(vfs_recovery_private_begin(target,ids[i],&owner_ops,handoff,NULL,&owner,&pin) == VFS_OK);
            assert(vfs_recovery_pin_mark_unresolved(pin) == VFS_OK);
            vfs_recovery_private_release(owner); storage_mutation_end();
            assert(!block_unregister_by_id(ids[i]) && target->registered && !target->online);
        }
        assert(pin_count() == VFS_MAX_RECOVERY_PINS && block_device_count() == BLOCK_MAX_DEVICES);
        block_device_t extra = source; extra.registered = false; extra.driver_data = &backing[0];
        assert(!block_register(&extra));
        puts("SLOTS 32 retained offline owners exhaust slots safely; no protected identity recycled"); goto done;
    }
    if (!strcmp(mode,"lifetime")) { registration_lifetime(); goto done; }
    if (!strcmp(mode,"reservation")) {
        assert(mutex_lock(&vfs_mount_lock));
        assert(vfs_mount_reserve(device,source.id) == VFS_OK);
        assert(!block_unregister_by_id(source.id) && device->registered && !device->online);
        vfs_mount_release_reservation(); assert(mutex_unlock(&vfs_mount_lock));
        assert(block_unregister_by_id(source.id) && !live_heap);
        puts("RESERVATION final unregister refused between validation reads until reservation release"); goto done;
    }
    if (!strcmp(mode,"finish-race")) {
        vfs_super_t *owner; finish_argument_t arg;
        assert(storage_mutation_begin());
        assert(vfs_recovery_private_begin(device,source.id,&owner_ops,handoff,NULL,&owner,&arg.pin) == VFS_OK);
        storage_mutation_end(); pause_handoff = true;
        pthread_t thread; assert(!pthread_create(&thread,NULL,finish_worker,&arg)); wait_paused();
        assert(!block_unregister_by_id(source.id) && vfs_registration_retained(source.id));
        resume_paused(thread); assert(arg.result == VFS_OK && !pin_count());
        vfs_recovery_private_release(owner);
        assert(block_unregister_by_id(source.id) && !live_heap);
        puts("FINISH-RACE registration retained during callback; final teardown admitted only after resolved post-unlock finish"); goto done;
    }
    if (!strcmp(mode,"cancel-finish")) {
        fail_read = 1; reject_finish = true;
        assert(native_label("NEW") == MG_ERR_UNCERTAIN && pin_count() == 1);
        mgfs_label_transaction_t *tx = recovery_pins->sb->private_data;
        assert(!tx->write_attempted && tx->handoff_result == VFS_ERR_BUSY);
        assert(!writes && !memcmp(media,before,media_bytes));
        assert(!block_unregister_by_id(source.id) && device->registered);
        puts("CANCEL-FINISH no-write failure retained after rejected handoff"); goto done;
    }
    if (!strcmp(mode,"ordinary") || !strcmp(mode,"history")) {
        ordinary_mode = true;
        assert(vfs_mount_root("mgfs",device) == VFS_OK);
        vfs_node_t *node; vfs_file_handle_t *handle;
        assert(vfs_create(vfs_get_root_node(),"test",&node) == VFS_OK);
        assert(vfs_open_node(node,VFS_OPEN_RDWR,&handle) == VFS_OK);
        fail_write = writes + 2; partial_bytes = 256;
        int result;
        if (!strcmp(mode,"history")) {
            mg_file_lines_state_t state;
            result = vfs_file_append_lines(handle,"fresh\n",6,10,0,&state);
        } else {
            u8 bytes[8192]; memset(bytes,'X',sizeof(bytes)); u64 written = 99;
            result = vfs_file_write_status(handle,sizeof(bytes),bytes,&written);
            assert(!written);
        }
        assert(result == VFS_ERR_UNCERTAIN && pin_count() == 1 && recovery_pins->state == VFS_RECOVERY_PIN_UNRESOLVED);
        assert(vfs_close(handle) == VFS_OK);
        assert(!block_unregister_by_id(source.id) && device->registered && !device->online);
        block_device_t duplicate = source; duplicate.registered = false;
        assert(!block_register(&duplicate) && pin_count() == 1);
        printf("MUTATION %s uncertainty retained offline registration and original pin\n",mode);
        goto done;
    }
    if (!strcmp(mode,"repeat")) {
        const char *labels[] = {"FIRST", "SECOND", "", "", "ORIGINAL"};
        for (unsigned i=0;i<5;i++) {
            assert(native_label(labels[i]) == MG_OK && !pin_count() && !live_heap);
            if (!*labels[i]) assert(!memcmp(media+200,(u8[88]){0},88));
            else assert(!memcmp(media+216,labels[i],strlen(labels[i])));
        }
        assert(!memcmp(media,before,media_bytes) && writes == 4 && flushes == 4);
        assert(block_unregister_by_id(source.id));
        puts("REPEAT set/replace/clear/clear-no-op/restore: exact superblock, no pins or heap retained"); goto done;
    }
    if (!strcmp(mode,"concurrent")) {
        pause_write = true;
        pthread_t thread; int result;
        assert(!pthread_create(&thread,NULL,worker,&result));
        assert(!pthread_mutex_lock(&pause_lock));
        while (!paused) assert(!pthread_cond_wait(&pause_condition,&pause_lock));
        assert(!pthread_mutex_unlock(&pause_lock));
        assert(pin_count() == 1 && vfs_format_preflight(device) == VFS_ERR_BUSY);
        assert(storage_set_filesystem_label_for_id(device,source.id,"mgfs","RACE") == VFS_ERR_BUSY);
        assert(storage_format_filesystem_for_id(device,source.id,"mgfs") == VFS_ERR_BUSY);
        assert(gpt_initialize_device_for_id(device,source.id) == VFS_ERR_BUSY);
        assert(vfs_mount_root("mgfs",device) == VFS_ERR_INVALID_PARAM);
        assert(!block_unregister_by_id(source.id)); /* Offline immediately; active callback still drains. */
        block_device_t duplicate = source; duplicate.registered = false;
        assert(!block_register(&duplicate));
        assert(!pthread_mutex_lock(&pause_lock)); resume_writer = true;
        assert(!pthread_cond_broadcast(&pause_condition)); assert(!pthread_mutex_unlock(&pause_lock));
        assert(!pthread_join(thread,NULL) && result == VFS_ERR_UNCERTAIN);
        retained(); puts("CONCURRENT active owner retained through removal and competing management"); goto done;
    }
    if (!strcmp(mode,"exhaust")) {
        assert(vfs_mount_root("mgfs",device) == VFS_OK);
        vfs_node_t *root = vfs_get_root_node(); vfs_super_t *sb = root->super;
        u64 pins[VFS_MAX_RECOVERY_PINS];
        assert(vfs_super_operation_lock(sb));
        for (unsigned i=0;i<VFS_MAX_RECOVERY_PINS;i++) assert(vfs_recovery_pin_begin(sb,&pins[i]) == VFS_OK);
        vfs_super_operation_unlock(sb);
        block_device_t spare = source; spare.driver_data = before; spare.registered = false;
        assert(block_register(&spare)); block_device_t *other = block_get_device(1);
        unsigned old_reads=reads;
        assert(storage_set_filesystem_label_for_id(other,other->id,"mgfs","NO") == VFS_ERR_NO_SPACE);
        assert(reads==old_reads && !writes && pin_count() == VFS_MAX_RECOVERY_PINS);
        for (unsigned i=0;i<VFS_MAX_RECOVERY_PINS;i++) assert(vfs_recovery_pin_finish(pins[i]) == VFS_OK);
        assert(vfs_unmount_device(device) == VFS_OK); kfree(root);
        assert(block_unregister_by_id(other->id));
        assert(block_unregister_by_id(source.id)); assert(!live_heap);
        puts("EXHAUST no I/O before bounded registry admission"); goto done;
    }
    fail_write = (!strcmp(mode,"write") || !strcmp(mode,"clear-write")) ? ordinal : 0;
    fail_read = !strcmp(mode,"read") ? ordinal : 0;
    fail_flush = (!strcmp(mode,"flush") || !strcmp(mode,"clear-flush")) ? ordinal : 0;
    fail_allocation = !strcmp(mode,"alloc") ? ordinal : 0;
    remove_write = !strcmp(mode,"remove");
    drop_write = !strcmp(mode,"drop") || !strcmp(mode,"clear-drop"); corrupt_readback = !strcmp(mode,"corrupt");
    reject_finish = !strcmp(mode,"finish");
    remove_finish = !strcmp(mode,"finish-remove");
    replace_dispatch = !strcmp(mode,"stale");
    cancel_lock = !strcmp(mode,"cancel");
    const char *label = !strncmp(mode,"clear",5) ? "" : !strcmp(mode,"noop") ? "ORIGINAL" : "NEW";
    if (!strcmp(mode,"invalid")) label = " invalid ";
    if (!strcmp(mode,"malformed")) label = "\xc0\xaf";
    if (!strcmp(mode,"unicode")) label = "N\xc3\xa1zev";
    if (!strcmp(mode,"max")) label = "123456789012345678901234567890123456789012345678901234567890123";
    if (!strcmp(mode,"readonly")) device->read_only = true;
    int result = native_label(label);
    unsigned operation_reads = reads, operation_allocations = allocations;
    usize operation_peak = peak_heap;
    fail_read = fail_allocation = fail_write = fail_flush = 0;
    if (result == MG_ERR_UNCERTAIN) retained();
    else {
        assert(!pin_count() && !live_heap);
        if (result == MG_OK) {
            assert(!strcmp(mode,"ok") || !strcmp(mode,"clear") || !strcmp(mode,"noop") ||
                   !strcmp(mode,"alloc") || !strcmp(mode,"read") || !strcmp(mode,"unicode") || !strcmp(mode,"max"));
            assert(!memcmp(media,before,200) && !memcmp(media+288,before+288,media_bytes-288));
            if (!strcmp(mode,"noop")) assert(!writes && !flushes && !memcmp(media,before,media_bytes));
            else assert(writes == 1 && flushes == 1);
            assert(vfs_mount_root("mgfs",device) == VFS_OK);
            vfs_node_t *root = vfs_get_root_node();
            assert(vfs_unmount_device(device) == VFS_OK); kfree(root);
            assert(!live_heap);
        } else assert(!writes && !memcmp(media,before,media_bytes));
        assert(!storage_mutation_busy() && block_unregister_by_id(source.id));
    }
    printf("RESULT mode=%s status=%d reads=%u writes=%u flushes=%u allocations=%u peak=%zu retained=%zu tx=%zu pin=%zu\n",
        mode,result,operation_reads,writes,flushes,operation_allocations,(size_t)operation_peak,(size_t)live_heap,sizeof(mgfs_label_transaction_t),sizeof(vfs_recovery_pin_t));
done:
    file = fopen(argv[2],"wb"); assert(file);
    assert(fwrite(media,1,media_bytes,file)==media_bytes && !fclose(file));
    free(before); free(media); return 0;
}
