/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production device.c and mutex.c; discovery, IRQs and scheduler
 * parking use controlled host dependencies. No filesystem mutation is mocked. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <scheduler.h>
#include "../../kernel/src/device.c"

static pthread_mutex_t mapping_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { spinlock_t *key; pthread_mutex_t lock; } mappings[16];
static _Thread_local kernel_thread_t *self;
static _Thread_local bool fail_park, no_identity;
static pthread_cond_t parked=PTHREAD_COND_INITIALIZER;
spinlock_t scheduler_lock;
static unsigned waiting, attempts;
static pthread_mutex_t control=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;

static pthread_mutex_t *native(spinlock_t *key)
{
    assert(!pthread_mutex_lock(&mapping_lock));
    unsigned i;
    for(i=0;i<16;i++) if (!mappings[i].key || mappings[i].key==key) break;
    assert(i<16);
    if (!mappings[i].key) {
        mappings[i].key=key;assert(!pthread_mutex_init(&mappings[i].lock,NULL));
    }
    assert(!pthread_mutex_unlock(&mapping_lock));return &mappings[i].lock;
}
void spin_lock(spinlock_t *key) { assert(!pthread_mutex_lock(native(key))); }
void spin_unlock(spinlock_t *key) { assert(!pthread_mutex_unlock(native(key))); }
void spinlock_init(spinlock_t *key) { key->value=0;(void)native(key); }
u64 spin_lock_irqsave(spinlock_t *key) { spin_lock(key);return 0; }
void spin_unlock_irqrestore(spinlock_t *key,u64 flags) { (void)flags;spin_unlock(key); }
kernel_thread_t *thread_current(void) { return no_identity?NULL:self; }
static bool scheduler_unblock_locked(kernel_thread_t *thread)
{
    if (thread->state!=THREAD_STATE_BLOCKED) return false;
    thread->state=THREAD_STATE_READY;pthread_cond_broadcast(&parked);return true;
}
bool scheduler_block(void)
{
    if (fail_park) return false;
    spin_lock(&scheduler_lock);
    if (__atomic_exchange_n(&self->mutex_wake_pending,false,__ATOMIC_ACQ_REL)) {
        spin_unlock(&scheduler_lock);return true;
    }
    self->state=THREAD_STATE_BLOCKED;
    assert(!pthread_mutex_lock(&control));waiting++;attempts++;
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&control);
    while(self->state==THREAD_STATE_BLOCKED)
        assert(!pthread_cond_wait(&parked,native(&scheduler_lock)));
    self->state=THREAD_STATE_RUNNING;
    assert(!pthread_mutex_lock(&control));waiting--;pthread_mutex_unlock(&control);
    spin_unlock(&scheduler_lock);return true;
}
/* scheduler_mutex_wake() is compiled from the production scheduler by the runner. */

static block_device_t disks[2];
static unsigned disk_count;
static bool pause_probe, entered, release_probe, nested, bad_probe, duplicate_capture;
static unsigned probes;
static gpt_partition_info_t partition;
u32 pci_get_device_count(void) { return 0; }
const pci_device_t *pci_get_device(u32 index) { (void)index;return NULL; }
u32 xhci_usb_device_snapshot(xhci_usb_device_info_t *out,u32 capacity)
{ (void)out;(void)capacity;return 0; }
bool net_fill_interface(void *out,usize capacity,usize *count)
{ (void)out;(void)capacity;*count=0;return false; }
u32 block_device_count(void) { return duplicate_capture?2:disk_count; }
block_device_t *block_get_device(u32 index)
{ return duplicate_capture ? &disks[0] : index<disk_count ? &disks[index]:NULL; }
bool block_device_snapshot(const block_device_t *device,block_device_t *out)
{
    assert(!pthread_mutex_lock(&control));
    bool found=device && device->registered;
    if (found) *out=*device;
    pthread_mutex_unlock(&control);return found;
}
bool block_device_id_is_live(u64 id)
{
    assert(!pthread_mutex_lock(&control));bool live=false;
    for(unsigned i=0;i<disk_count;i++) if (disks[i].id==id && disks[i].registered && disks[i].online) live=true;
    pthread_mutex_unlock(&control);return live;
}
bool block_device_automount_suppressed(u64 id) { (void)id;return false; }
const char *block_type_name(block_device_type_t type) { (void)type;return "usb"; }
bool gpt_get_partition_info(block_device_t *dev,gpt_partition_info_t *out)
{
    assert(!pthread_mutex_lock(&control));
    bool found=dev->type==BLOCK_DEVICE_PARTITION && disks[0].id==partition.parent_id && disks[0].online;
    if(found) *out=partition;
    pthread_mutex_unlock(&control);return found;
}
bool gpt_partition_is_esp(const gpt_partition_info_t *p) { (void)p;return false; }
const char *vfs_probe_filesystem_for_id(block_device_t *device,u64 id)
{
    assert(!pthread_mutex_lock(&control));probes++;
    bool recurse=nested;nested=false;
    if(pause_probe) {
        pause_probe=false;entered=true;pthread_cond_broadcast(&changed);
        while(!release_probe) assert(!pthread_cond_wait(&changed,&control));
    }
    bool valid=device->id==id && device->online && !bad_probe;
    pthread_mutex_unlock(&control);
    if(recurse) {
        mg_device_info_t out={.id=123};u32 total=123;u64 generation=123;
        assert(device_snapshot_read(MG_DEVICE_FILTER_BLOCKS,0,0,0,&out,1,&total,&generation)==MG_ERR_BUSY);
        assert(out.id==123 && total==123 && generation==123);
    }
    return valid?"exfat":NULL;
}
const char *vfs_probe_filesystem(block_device_t *device)
{ return vfs_probe_filesystem_for_id(device,device->id); }
bool vfs_filesystem_label(block_device_t *device,const char *fs,char *out,usize capacity)
{ (void)device;(void)fs;assert(capacity>10);strcpy(out,"QUAL_EXFAT");return true; }
bool vfs_mount_point_for_device(block_device_t *device,char *out,usize capacity,vfs_mount_role_t *role)
{ (void)device;(void)capacity;(void)role;strcpy(out,"/vol/QUAL_EXFAT");return true; }

typedef struct {
    kernel_thread_t thread;
    u64 requested,generation;
    mg_device_info_t out[4];u32 total;
    mg_result_t result;
} call_t;
static void *reader(void *pointer)
{
    call_t *call=pointer;self=&call->thread;
    call->result=device_snapshot_read(MG_DEVICE_FILTER_BLOCKS,0,0,call->requested,call->out,4,&call->total,&call->generation);
    return NULL;
}
static call_t fresh(u64 requested)
{
    call_t call={0};call.thread.state=THREAD_STATE_RUNNING;call.requested=requested;
    call.total=123;call.generation=123;call.out[0].id=123;return call;
}
static call_t read_now(u64 requested)
{
    kernel_thread_t *saved=self;call_t call=fresh(requested);reader(&call);self=saved;return call;
}
static void set_disk(u64 id)
{
    disks[0]=(block_device_t){.id=id,.type=BLOCK_DEVICE_USB,.sector_size=512,
        .sector_count=131072,.registered=true,.online=true};disk_count=1;
}
static void wait_for_probe(void)
{ pthread_mutex_lock(&control);while(!entered)pthread_cond_wait(&changed,&control);pthread_mutex_unlock(&control); }
static void wait_for_waiters(unsigned count)
{ pthread_mutex_lock(&control);while(waiting<count)pthread_cond_wait(&changed,&control);pthread_mutex_unlock(&control); }
static void unpause(void)
{ pthread_mutex_lock(&control);release_probe=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&control); }
static pthread_t start_paused(call_t *call)
{
    pthread_t thread;entered=release_probe=false;pause_probe=true;
    assert(!pthread_create(&thread,NULL,reader,call));wait_for_probe();return thread;
}
static void *stress(void *unused)
{
    (void)unused;call_t call=fresh(0);self=&call.thread;u64 previous=0;
    for(unsigned i=0;i<128;i++) {
        assert(device_snapshot_read(MG_DEVICE_FILTER_BLOCKS,0,0,0,call.out,4,&call.total,&call.generation)==1);
        assert(call.total==1 && call.out[0].disk_number==1 && call.generation>previous);
        previous=call.generation;
    }
    return NULL;
}
int main(void)
{
    kernel_thread_t main_thread={.state=THREAD_STATE_RUNNING};self=&main_thread;
    set_disk(10);nested=true;call_t base=read_now(0);
    assert(base.result==1 && base.total==1 && base.out[0].disk_number==1);
    puts("preserved nested-refresh reproducer: recursion rejected, unique ID, original snapshot intact");
    call_t a=fresh(0),b=fresh(0),c=fresh(base.generation);
    pthread_t ta=start_paused(&a),tb,tc;
    assert(!pthread_create(&tb,NULL,reader,&b));wait_for_waiters(1);
    assert(!pthread_create(&tc,NULL,reader,&c));wait_for_waiters(2);
    /* Interrupted parking cannot leak admission or change the old generation. */
    fail_park=true;call_t cancelled=read_now(0);fail_park=false;
    assert(cancelled.result==MG_ERR_BUSY && cancelled.total==123 && cancelled.out[0].id==123);
    unpause();pthread_join(ta,NULL);pthread_join(tb,NULL);pthread_join(tc,NULL);
    assert(a.result==1 && b.result==1 && a.total==1 && b.total==1);
    assert(a.out[0].id==b.out[0].id && a.out[0].disk_number==b.out[0].disk_number);
    assert(a.generation==base.generation+1 && b.generation==a.generation+1);
    assert(c.result==MG_ERR_RETRY && c.generation==123 && c.out[0].id==123);
    call_t same=read_now(b.generation);assert(same.result==1 && same.generation==b.generation);
    puts("concurrent refresh/copy: FIFO progress, stale pagination RETRY, cancellation leaves outputs unchanged");
    /* Failed identification is a present device with unknown filesystem. */
    bad_probe=true;call_t bad=read_now(0);assert(bad.result==1 && !bad.out[0].filesystem[0]);
    bad_probe=false;assert(read_now(0).out[0].filesystem[0]);
    no_identity=true;call_t missing=read_now(0);no_identity=false;
    assert(missing.result==MG_ERR_BUSY && missing.total==123);
    duplicate_capture=true;u64 before=snapshot_generation;call_t dup=read_now(0);duplicate_capture=false;
    assert(dup.result==MG_ERR_RETRY && dup.total==123 && snapshot_generation==before);
    assert(read_now(before).result==1);
    /* Retire or replace the registration while the filesystem probe sleeps. */
    for(unsigned replace=0;replace<2;replace++) {
        set_disk(10);a=fresh(0);ta=start_paused(&a);
        pthread_mutex_lock(&control);disks[0].online=false;
        if(replace) {disks[0].id=11;disks[0].online=true;}
        pthread_mutex_unlock(&control);unpause();pthread_join(ta,NULL);
        assert(a.result==0 && a.total==0);
        if(replace) {same=read_now(0);assert(same.result==1 && same.out[0].id==block_id(&disks[0]) && same.out[0].disk_number==2);}
    }
    set_disk(10);disk_count=2;disks[1]=disks[0];disks[1].id=12;
    mg_device_info_t page[1];u32 total=0;u64 token=0;
    assert(device_snapshot_read(MG_DEVICE_FILTER_BLOCKS,0,0,0,page,1,&total,&token)==1 && total==2);
    u64 first=page[0].id;
    assert(device_snapshot_read(MG_DEVICE_FILTER_BLOCKS,0,1,token,page,1,&total,NULL)==1 && page[0].id!=first);
    assert(read_now(0).result==2);
    assert(device_snapshot_read(MG_DEVICE_FILTER_BLOCKS,0,1,token,page,1,&total,NULL)==MG_ERR_RETRY);
    /* A parent that disappears mid-probe cannot survive as a valid GPT relation. */
    set_disk(20);disk_count=2;disks[1]=disks[0];disks[1].id=21;disks[1].type=BLOCK_DEVICE_PARTITION;
    partition=(gpt_partition_info_t){.parent=&disks[0],.parent_id=20,.number=1,.first_lba=1,.last_lba=100};
    a=fresh(0);ta=start_paused(&a);pthread_mutex_lock(&control);disks[0].online=false;
    pthread_mutex_unlock(&control);unpause();pthread_join(ta,NULL);
    for(unsigned i=0;i<a.total;i++) assert(a.out[i].category!=MG_DEVICE_CATEGORY_PARTITION);
    set_disk(10);same=read_now(0);assert(same.out[0].disk_number==1);
    /* Exhaustion never reuses another registration's presentation number. */
    disk_display_count=DEVICE_DISK_DISPLAY_MAX;set_disk(999);same=read_now(0);
    assert(same.result==1 && same.out[0].disk_number==0);
    set_disk(10);same=read_now(0);assert(same.out[0].disk_number==1);
    puts("probe failure, offline/replaced registration, parent liveness and presentation-ID exhaustion passed");
    before=snapshot_generation;pthread_t threads[4];
    for(unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,stress,NULL));
    for(unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(snapshot_generation==before+512);
    puts("512 contended refreshes: unique entries, stable presentation IDs, monotonic completed generations");
#ifndef SNAPSHOT_LEGACY_CONTROL
    assert(!snapshot_lock.owner && !snapshot_lock.wait_head && !snapshot_lock.wait_tail && !waiting);
#endif
    puts("device snapshots: complete production discovery and FIFO mutex; ASan/UBSan; no probe or gate allocations");
    return 0;
}
