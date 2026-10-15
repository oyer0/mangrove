/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production VFS/MGFS; pthread transport models explicit interleavings.
 * No host scheduler, IRQ, DMA or media-equivalence qualification is implied. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mfqs/host.h"
#include "../kernel/src/storage/mgfs.c"

static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t spin = PTHREAD_MUTEX_INITIALIZER;
static struct { mutex_t *key; pthread_mutex_t value; } maps[128];
static _Thread_local char thread_identity;
static _Thread_local bool mutator_thread, cancel_admission, no_thread_identity, counted_wait;
struct kernel_thread *thread_current(void) { return no_thread_identity ? NULL : (void *)&thread_identity; }
static vfs_super_t *super;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool armed, paused, resume_mutator;
static unsigned attempts, finished;
static int mutation_result, expected_error;
static vfs_file_handle_t *reader, *history, *queued_history;
static const char *operation;
static vfs_location_t *moving_cwd;
static void release_cwd(void)
{ if(moving_cwd){vfs_location_cwd_release(moving_cwd);vfs_location_release(moving_cwd);moving_cwd=NULL;} }
static u32 copy_source(void *context, u64 offset, u8 *out, u32 count)
{ (void)context; static const char text[] = "new staged bytes";
  if (offset > sizeof(text)-1 || count > sizeof(text)-1-offset) return 0;
  memcpy(out,text+offset,count);return count; }

static pthread_mutex_t *native(mutex_t *key)
{
    assert(!pthread_mutex_lock(&maps_lock));
    unsigned i;
    for (i = 0; i < 128; i++) if (!maps[i].key || maps[i].key == key) break;
    assert(i < 128);
    if (!maps[i].key) {
        pthread_mutexattr_t attr;
        assert(!pthread_mutexattr_init(&attr));
        assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
        assert(!pthread_mutex_init(&maps[i].value, &attr));
        assert(!pthread_mutexattr_destroy(&attr));
        maps[i].key = key;
    }
    assert(!pthread_mutex_unlock(&maps_lock));
    return &maps[i].value;
}
void mutex_init(mutex_t *m) { memset(m, 0, sizeof(*m)); }
bool mutex_try_lock(mutex_t *m)
{
    if (pthread_mutex_trylock(native(m))) return false;
    m->owner = thread_current(); return true;
}
bool mutex_lock(mutex_t *m)
{
    if (super && m == &super->operation_guard && cancel_admission) return false;
    if (super) {
        assert(!pthread_mutex_lock(&control));
        /* Namespace lookup may now queue outside filesystem admission. Both
         * gates are real production gates held through resolved handoff. */
        if (paused && !mutator_thread && !counted_wait && m->owner &&
            m->owner != thread_current()) {
            attempts++; counted_wait = true; pthread_cond_broadcast(&changed);
        }
        assert(!pthread_mutex_unlock(&control));
    }
    if (pthread_mutex_lock(native(m))) return false;
    m->owner = thread_current(); return true;
}
bool mutex_unlock(mutex_t *m)
{
    if (m->owner != thread_current()) return false;
    m->owner = NULL;
    assert(!pthread_mutex_unlock(native(m)));
    if (super && m == &super->operation_lock && mutator_thread) {
        assert(!pthread_mutex_lock(&control));
        if (armed) {
            armed = false; paused = true;
            pthread_cond_broadcast(&changed);
            while (!resume_mutator) assert(!pthread_cond_wait(&changed, &control));
            paused = false;
        }
        assert(!pthread_mutex_unlock(&control));
    }
    return true;
}
void spinlock_init(spinlock_t *lock) { lock->value = 0; }
u64 spin_lock_irqsave(spinlock_t *lock)
{ assert(!pthread_mutex_lock(&spin)); assert(!lock->value); lock->value = 1; return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ assert(!flags && lock->value); lock->value = 0; assert(!pthread_mutex_unlock(&spin)); }

static void wait_for(bool *condition, unsigned count)
{
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline)); deadline.tv_sec += 10;
    while (condition ? !*condition : attempts < count)
        assert(!pthread_cond_timedwait(&changed, &control, &deadline));
}
static void *mutate(void *unused)
{
    (void)unused;
    mutator_thread = true;
    vfs_node_t *created = NULL;
    mg_file_lines_state_t state;
    if (!strcmp(operation,"create")) mutation_result = vfs_create(vfs_get_root_node(), "handoff-created", &created);
    else if (!strcmp(operation,"rename")) mutation_result=vfs_rename(vfs_get_root_node(),"core",vfs_get_root_node(),"moved");
    else if (!strcmp(operation,"history")) mutation_result = vfs_file_append_lines(history, "new line\n", 9, 64, 0, &state);
    else if (!strcmp(operation,"write")) {
        u64 written=0; mutation_result=vfs_file_write_status(history,9,"overwrite",&written);
        if (!mutation_result) assert(written==9);
    } else if (!strcmp(operation,"truncate")) mutation_result=vfs_truncate_handle(history);
    else if (!strcmp(operation,"replace")) mutation_result=vfs_file_replace(history,true,16,copy_source,NULL);
    else assert(false);
    return NULL;
}
static void *observe(void *argument)
{
    unsigned kind = (unsigned)(uintptr_t)argument;
    int error = VFS_OK;
    mg_file_lines_state_t state;
    if (kind == 0) {
        vfs_node_t *node = NULL;
        if(moving_cwd) {
            VFS_LOCATION_AUTO vfs_location_t *found=NULL;
            error=vfs_location_lookup(moving_cwd,"deviced",NULL,NULL,&found);
            node=vfs_location_node(found);
            if(!error) {
                char path[32];
                assert(vfs_location_path(moving_cwd,path,sizeof(path),NULL)==VFS_OK);
                assert(!strcmp(path,"/moved"));
            }
        } else error = vfs_lookup("/core/deviced", &node);
        if (!error) assert(node && node->size > 0);
    } else if (kind == 1) {
        u8 byte = 0;
        u64 count = vfs_file_read(reader, 1, &byte);
        error = vfs_super_operation_error(super);
        if (!error) assert(count == 1 && byte == 0x7f);
    } else if (kind == 2) {
        vfs_dirent_t entry;
        int result = vfs_readdir_checked(vfs_get_root_node(), 0, &entry);
        error = result < 0 ? result : VFS_OK;
        if (!error) assert(result == 1);
    } else if (kind == 3) {
        vfs_node_metadata_t metadata;
        error = vfs_node_metadata_snapshot(reader->node, &metadata);
        if (!error) assert(metadata.size == reader->node->size);
    } else {
        /* A queued writer must progress after handoff, including history's
         * offset -> admission -> operation locking. */
        error=vfs_file_append_lines(queued_history,"queued line\n",12,64,0,&state);
    }
    assert(!pthread_mutex_lock(&control));
    if (error != expected_error) fprintf(stderr, "reader %u error=%d expected=%d\n", kind, error, expected_error);
    assert(error == expected_error);
    finished++;
    assert(!pthread_mutex_unlock(&control));
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 4 && mfqs_media_open(argv[1]));
    operation=argv[3];
    vfs_init(); assert(mgfs_init() == VFS_OK);
    assert(vfs_mount_root("mgfs", mfqs_device()) == VFS_OK);
    super = vfs_get_root_node()->super;
    assert(super->serialize_handoff);
    assert(vfs_open("/core/deviced", VFS_OPEN_READ, &reader) == VFS_OK);
    assert(vfs_open("/history", VFS_OPEN_RDWR, &history)==VFS_OK);
    assert(vfs_open("/history", VFS_OPEN_RDWR, &queued_history)==VFS_OK);
    char snapshot[256]; mg_file_lines_state_t state;
    assert(vfs_file_snapshot(history,snapshot,sizeof(snapshot),&state)==VFS_OK);
    if(!strcmp(operation,"rename")) {
        assert(vfs_location_lookup(NULL,"/core",NULL,NULL,&moving_cwd)==VFS_OK);
        assert(vfs_location_cwd_retain(moving_cwd));
    }
    u32 baseline = super->reference_count;
    no_thread_identity=true;
    assert(!vfs_super_operation_guard_release(super));
    no_thread_identity=false;
    assert(super->reference_count==baseline);

    /* A recursive logical acquisition and a canceled admission cannot steal
     * or leak the outer guard. No scheduler policy is simulated as success. */
    assert(vfs_super_operation_lock(super));
    assert(!vfs_super_operation_lock(super));
    vfs_super_operation_unlock(super);
    cancel_admission = true; assert(!vfs_super_operation_lock(super)); cancel_admission = false;
    assert(super->reference_count == baseline);
    if (!strcmp(argv[2],"torn") || !strcmp(argv[2],"flush") || !strcmp(argv[2],"exhausted")) {
        u64 held[32]; bool exhausted=!strcmp(argv[2],"exhausted");
        if (exhausted) {
            assert(vfs_super_operation_lock(super));
            for(unsigned i=0;i<32;i++) assert(vfs_recovery_pin_begin(super,&held[i])==VFS_OK);
            vfs_super_operation_unlock(super);
        } else mfqs_fail_after(!strcmp(argv[2],"torn")?'w':'f',!strcmp(argv[2],"torn")?2:1,true,false);
        mutate(NULL); mutator_thread=false;
        if (exhausted) {
            assert(mutation_result==VFS_ERR_NO_SPACE);
            for(unsigned i=0;i<32;i++) assert(vfs_recovery_pin_finish(held[i])==VFS_OK);
            assert(vfs_super_operation_error(super)==VFS_OK);
        } else {
            assert(mutation_result==VFS_ERR_UNCERTAIN);
            assert(vfs_super_operation_error(super)==VFS_ERR_UNCERTAIN);
            assert(vfs_format_preflight(mfqs_device())==VFS_ERR_BUSY);
            assert(vfs_unmount_device(mfqs_device())==VFS_ERR_BUSY);
        }
        vfs_recovery_pin_info_t info; u32 count=0;
        assert(vfs_recovery_pins_snapshot(&info,1,&count)==VFS_OK);
        assert(count==(exhausted?0U:1U));
        if (!exhausted) assert(info.state==VFS_RECOVERY_PIN_UNRESOLVED);
        assert(super->reference_count==baseline+(exhausted?0U:1U));
        assert(!super->operation_guard_owner);
        vfs_close(reader);vfs_close(history);vfs_close(queued_history);
        if(!exhausted) assert(vfs_unmount_device(mfqs_device())==VFS_ERR_BUSY);
        printf("MGFS handoff %s/%s: fault/refusal contained, admission released\n",operation,argv[2]);
        release_cwd();mfqs_media_close(); return 0;
    }
    if (!strcmp(argv[2],"legacy-control")) {
        /* Disable only opt-in admission, retaining actual VFS/MGFS logic.
         * The historical false-absence window must still be observable. */
        /* Data mutation does not hold namespace admission, so disabling
         * filesystem admission still exposes the historical read window. */
        operation="write";
        super->serialize_handoff=false; armed=true;
        pthread_t writer; assert(!pthread_create(&writer,NULL,mutate,NULL));
        assert(!pthread_mutex_lock(&control)); wait_for(&paused,0);
        assert(!pthread_mutex_unlock(&control));
        vfs_node_t *node=NULL; u8 byte=0;
        assert(vfs_lookup("/core/deviced",&node)==VFS_ERR_NOT_FOUND);
        assert(vfs_file_read(reader,1,&byte)==0);
        assert(!pthread_mutex_lock(&control)); resume_mutator=true;
        pthread_cond_broadcast(&changed); assert(!pthread_mutex_unlock(&control));
        assert(!pthread_join(writer,NULL)); assert(mutation_result==VFS_OK);
        assert(vfs_file_read(reader,1,&byte)==1 && byte==0x7f);
        puts("MGFS handoff negative control: absent admission reproduces false NOT_FOUND and EOF");
        release_cwd();mfqs_media_close(); return 0;
    }
    expected_error = !strcmp(argv[2], "failed-handoff") ? VFS_ERR_UNCERTAIN :
                     !strcmp(argv[2], "removal") ? VFS_ERR_DEVICE_GONE : VFS_OK;
    armed = true;
    pthread_t writer, readers[5];
    assert(!pthread_create(&writer, NULL, mutate, NULL));
    assert(!pthread_mutex_lock(&control)); wait_for(&paused, 0);
    assert(finished == 0);
    assert(!vfs_super_operation_guard_release(super)); /* wrong owner */
    vfs_recovery_pin_info_t pin; u32 pins = 0;
    assert(vfs_recovery_pins_snapshot(&pin, 1, &pins) == VFS_OK && pins == 1);
    assert(vfs_recovery_pin_finish_handoff(pin.id) == VFS_ERR_BUSY);
    for (unsigned i = 0; i < 5; i++) assert(!pthread_create(&readers[i], NULL, observe, (void *)(uintptr_t)i));
    wait_for(NULL, 5); assert(finished == 0);
    mgfs_fs_t *fs = super->private_data;
    if (!strcmp(argv[2], "failed-handoff")) {
        if(fs->ordinary_transaction) fs->ordinary_transaction->handoff_pending=false;
        else { assert(fs->stage_transaction); fs->stage_transaction->handoff_pending=false; }
    }
    else if (!strcmp(argv[2], "removal")) {
        mfqs_device()->online = false; vfs_device_removed(mfqs_device());
    } else assert(!strcmp(argv[2], "success"));
    resume_mutator = true; pthread_cond_broadcast(&changed);
    assert(!pthread_mutex_unlock(&control));
    assert(!pthread_join(writer, NULL));
    for (unsigned i = 0; i < 5; i++) assert(!pthread_join(readers[i], NULL));
    assert(finished == 5 && !super->operation_guard_owner);
    assert(vfs_recovery_pins_snapshot(&pin, 1, &pins) == VFS_OK);
    if (!strcmp(argv[2], "failed-handoff")) {
        assert(mutation_result == VFS_ERR_UNCERTAIN && pins == 1 && pin.state == VFS_RECOVERY_PIN_UNRESOLVED);
        assert(vfs_format_preflight(mfqs_device()) == VFS_ERR_BUSY);
        assert(vfs_unmount_device(mfqs_device()) == VFS_ERR_BUSY);
    } else {
        assert(!pins);
        if (!strcmp(argv[2],"success")) assert(mutation_result==VFS_OK);
    }
    assert(super->reference_count==baseline+(!strcmp(argv[2],"failed-handoff")?1U:0U));
    vfs_close(reader); vfs_close(history); vfs_close(queued_history);
    mfqs_metrics_t metrics; mfqs_metrics(&metrics);
    printf("MGFS handoff %s/%s: five real waiters; pins=%u; guard=%zu; super=%zu; reads=%llu writes=%llu flushes=%llu peak=%llu\n",
        operation,argv[2],pins,sizeof(super->operation_guard),sizeof(*super),
        (unsigned long long)metrics.reads,(unsigned long long)metrics.writes,
        (unsigned long long)metrics.flushes,(unsigned long long)metrics.heap_peak);
    release_cwd();mfqs_media_close();
    return 0; /* Unresolved evidence deliberately retained; leak detection is disabled. */
}
