/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production FAT32, storage dispatch, native case and private recovery registry. */
#include <storage/gpt.h>
#include <mg/storage.h>
#include <mangrove_errors.h>
#define FAT32_REAL_RECOVERY
#define main unused_identity_main
#include "fat32_mount_identity_test.c"
#undef main
#include "vfs_fat32_label_owner.inc"

static bool management_held, reject_finish, reject_admission;
static block_device_t alias;
static unsigned failing_read, replace_read, remove_write, read_requests;
static FILE *original_image;
typedef struct { u64 lba; u8 bytes[512]; } overlay_sector_t;
static overlay_sector_t overlay[256];
static unsigned overlay_count;

bool storage_mutation_begin(void) { if (management_held) return false; management_held = true; return true; }
void storage_mutation_end(void) { assert(management_held); management_held = false; }
bool storage_mutation_busy(void) { return management_held; }
void mutex_init(mutex_t *lock) { memset(lock, 0, sizeof(*lock)); }
bool mutex_lock(mutex_t *lock) { assert(!lock->owner); lock->owner = (void *)1; return true; }
bool mutex_try_lock(mutex_t *lock) { if ((lock==&vfs_mount_lock ? reject_admission : reject_finish) || lock->owner) return false; lock->owner = (void *)1; return true; }
bool mutex_unlock(mutex_t *lock) { assert(lock->owner); lock->owner = NULL; return true; }
u64 spin_lock_irqsave(spinlock_t *lock) { assert(!lock->value); lock->value = 1; return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) { assert(lock->value && !flags); lock->value = 0; }
bool block_device_is_live(const block_device_t *dev) { return online && (dev==&device || dev==&alias); }
bool block_device_snapshot(const block_device_t *dev, block_device_t *out) {
    if (!dev || !out || !online || (dev != &device && dev != &alias)) return false;
    *out = *dev; return true;
}
bool gpt_get_partition_info(block_device_t *dev, gpt_partition_info_t *out) {
    if (dev != &alias) return false;
    *out = (gpt_partition_info_t){.parent = &device, .first_lba = 0,
        .last_lba = device.sector_count - 1U}; return true;
}
static bool overlay_read(block_device_t *dev, u64 id, u64 lba, u32 count, void *out) {
    if (dev != &device || !online || id != device.id || lba > device.sector_count ||
        count > device.sector_count - lba || ++read_requests == failing_read) return false;
    if (fseeko(original_image, (off_t)(lba * 512U), SEEK_SET) ||
        fread(out, 512U, count, original_image) != count) return false;
    for (u32 i = 0; i < count; i++)
        for (unsigned j = 0; j < overlay_count; j++)
            if (overlay[j].lba == lba+i) memcpy((u8 *)out+i*512U, overlay[j].bytes, 512U);
    reads += count;
    if (read_requests == replace_read) device.id++;
    return id == device.id;
}
static bool overlay_write(block_device_t *dev, u64 id, u64 lba, u32 count, const void *data) {
    assert(management_held && recovery_pin_count == 1U && recovery_pins);
    assert(recovery_pins->sb->operation_lock.owner);
    if (dev != &device || !online || id != device.id || lba > device.sector_count ||
        count > device.sector_count-lba) return false;
    writes++;
    bool failed = writes == fail_write_at;
    usize copied = failed ? (fail_write_definite ? 0U : fail_write_partial_bytes) : (usize)count*512U;
    for (u32 i = 0; i < count && copied; i++) {
        unsigned j;
        for (j = 0; j < overlay_count && overlay[j].lba != lba+i; j++) {}
        if (j == overlay_count) {
            assert(overlay_count < 256U);
            overlay[j].lba = lba+i;
            assert(!fseeko(original_image, (off_t)((lba+i)*512U), SEEK_SET) &&
                fread(overlay[j].bytes, 512U, 1U, original_image) == 1U);
            overlay_count++;
        }
        usize amount = copied < 512U ? copied : 512U;
        memcpy(overlay[j].bytes, (const u8 *)data+i*512U, amount); copied -= amount;
    }
    if (writes == remove_write) online = false;
    return !failed && online && id == device.id;
}
static bool label_flush(block_device_t *dev, u64 id) {
    assert(management_held && recovery_pins && recovery_pins->sb->operation_lock.owner);
    flushes++;
    return dev == &device && id == device.id && online && flushes != fail_flush_at;
}
/* Production storage dispatch delegates FAT32 before any generic post-owner I/O. */
static int storage_set_filesystem_label_guarded(block_device_t *dev, const char *fs_name, const char *label) {
    (void)dev; (void)label; assert(!strcmp(fs_name,"mgfs")); return VFS_ERR_UNSUPPORTED;
}
int exfat_set_label_for_id(block_device_t *dev, u64 expected, const char *label) { (void)dev; (void)expected; (void)label; return VFS_ERR_UNSUPPORTED; }
#include "fat32_label_dispatch.inc"

static i64 native_label(const char *text) {
    mg_storage_label_request_t request = {.instance_id=BLOCK_DEVICE_ID_BASE | (device.id+1U)};
    strcpy(request.filesystem,"fat32"); strcpy(request.label,text);
    requester.storage_management_session = true;
    requester.storage_authorized_operation = MG_STORAGE_OP_LABEL;
    label_frame_t frame = {.rax=SYSCALL_STORAGE_LABEL,.rdi=(u64)(usize)&request};
    label_syscall(&frame);
    assert(!requester.storage_authorized_operation);
    return (i64)frame.rax;
}
static void prepare(void) {
    assert(!recovery_pins && !live_requested); setup();
    assert(!fseeko(original_image, 0, SEEK_END));
    device.sector_count = (u64)ftello(original_image)/512U; device.sector_size = 512U;
    device.type = BLOCK_DEVICE_USB; device.read_only = false;
    alias = device; alias.id = 999U; alias.type = BLOCK_DEVICE_PARTITION;
    recovery_pin_count = 0; next_recovery_pin_id = 1U;
    reject_finish = false; overlay_count = 0; failing_read = replace_read = remove_write = read_requests = 0;
    scaling_read = overlay_read; scaling_write = overlay_write; flush_hook = label_flush;
}
static void discard_pin(void) {
    assert(recovery_pins && recovery_pin_count == 1U);
    vfs_recovery_pin_t *pin = recovery_pins; vfs_super_t *sb = pin->sb;
    assert(sb->reference_count == 1U && sb->state == VFS_SUPER_DETACHING);
    fat32_label_transaction_t *label = pin->handoff_context;
    kfree(label->fsinfo); fat32_replace_free(&label->transaction);
    if (sb->private_data) ((fat32_fs_t *)sb->private_data)->replace_transaction = NULL;
    recovery_pins = NULL; recovery_pin_count = 0; kfree(pin); vfs_super_release(sb);
    assert(!live_requested);
}
static void contained(void) {
    assert(recovery_pins && recovery_pins->state == VFS_RECOVERY_PIN_UNRESOLVED);
    fat32_label_transaction_t *label = recovery_pins->handoff_context;
    usize bound = sizeof(*label)+sizeof(fat32_directory_growth_t)+sizeof(fat32_label_fsinfo_t)+
        sizeof(vfs_super_t)+sizeof(vfs_recovery_pin_t)+sizeof(fat32_fs_t)+sizeof(vfs_node_t)+sizeof(fat32_node_state_t);
    assert(!label->transaction.data_buffer && live_requested <= bound);
    unsigned before = writes;
    assert(vfs_format_preflight(&device) == VFS_ERR_BUSY && vfs_format_preflight(&alias) == VFS_ERR_BUSY);
    assert(fat32_set_label(&device,"retry") == VFS_ERR_BUSY && writes == before);
    assert(vfs_recovery_pin_finish_handoff(recovery_pins->id) == VFS_ERR_UNCERTAIN);
}
static void save_image(const char *path) {
    FILE *out = fopen(path,"wb"); assert(out); u8 buffer[4096];
    rewind(original_image); size_t amount;
    while ((amount=fread(buffer,1U,sizeof(buffer),original_image))) assert(fwrite(buffer,1U,amount,out)==amount);
    for (unsigned i=0; i<overlay_count; i++) {
        assert(!fseeko(out,(off_t)(overlay[i].lba*512U),SEEK_SET));
        assert(fwrite(overlay[i].bytes,512U,1U,out)==1U);
    }
    assert(!fclose(out));
}
static void success(const char *label) {
    assert(storage_set_filesystem_label(&device,"fat32",label) == VFS_OK && !recovery_pins && !live_requested);
    char observed[64]; bool found = fat32_read_label(&device, observed, sizeof(observed));
    assert(*label ? found && !strcmp(observed,label) : !found && !*observed);
}
int main(int argc, char **argv) {
    assert(argc==3 || argc==4);
    if (argc==4) {
        assert(!strcmp(argv[1],"--reject")); original_image=fopen(argv[2],"rb"); assert(original_image);
        prepare(); int result=storage_set_filesystem_label(&device,"fat32","NEWLABEL");
        assert(result==atoi(argv[3]) && !writes && !recovery_pins && !live_requested);
        assert(!fclose(original_image)); return 0;
    }
    original_image=fopen(argv[1],"rb"); assert(original_image);
    prepare(); clock_t start=clock();
    assert(storage_set_filesystem_label(&device,"fat32","NEWLABEL") == VFS_OK && !recovery_pins && !live_requested);
    unsigned wc=writes, fc=flushes, ac=allocations, rc=read_requests;
    printf("FAT32 label: peak=%zu reads=%u writes=%u flushes=%u allocations=%u cpu_ms=%.3f\n",
        (size_t)peak_requested,reads,wc,fc,ac,1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
    save_image(argv[2]); success("REPLACED"); success(""); success(""); success("Case label"); success("ABCDEFGHIJK"); success("AGAIN");
    for (unsigned op=0; op<2; op++) {
        const char *label=op ? "" : "NEWLABEL";
        for (unsigned mode=0; mode<6; mode++)
            for (unsigned at=1; at <= (mode==3 ? fc : mode==4 ? ac : mode==5 ? rc : wc); at++) {
                prepare();
                if (mode<3) { fail_write_at=at; fail_write_definite=mode==0; fail_write_partial_bytes=mode==1 ? 128U : 512U; }
                if (mode==3) fail_flush_at=at;
                if (mode==4) fail_allocation=at;
                if (mode==5) failing_read=at;
                int result=storage_set_filesystem_label(&device,"fat32",label);
                if (result==VFS_OK) {
                    /* Clear of an already unlabeled root can have fewer writes. */
                    assert((mode<3 && writes<at) || (mode==3 && flushes<at) ||
                        (mode==4 && allocations<at) || (mode==5 && read_requests<at));
                } else if (result==VFS_ERR_UNCERTAIN) { contained(); discard_pin(); }
                else {
                    assert(!recovery_pins && !live_requested);
                    /* A pre-link read error may cancel confirmed fresh allocation. */
                    if (writes) {
                        assert(mode==5 && (result==VFS_ERR_IO || result==VFS_ERR_NO_SPACE));
                        unsigned cleanup_write=writes, cleanup_flush=flushes;
                        /* Repeat the definite pre-link failure, then fail the
                         * cancellation itself instead of resolving its pin. */
                        for (unsigned fault=0; fault<2U; fault++) {
                            prepare(); failing_read=at;
                            if (!fault) { fail_write_at=cleanup_write; fail_write_partial_bytes=128U; }
                            else fail_flush_at=cleanup_flush;
                            assert(storage_set_filesystem_label(&device,"fat32",label)==VFS_ERR_UNCERTAIN);
                            contained(); discard_pin();
                        }
                    }
                }
            }
    }
    const char *invalid[] = {" ","X ","NO NAME","no name"};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); i++) {
        prepare(); assert(native_label(invalid[i]) == MG_ERR_BAD_ARGUMENT &&
            !reads && !writes && !allocations && !recovery_pins);
    }
    prepare(); requester.storage_management_session=false;
    requester.storage_authorized_operation=MG_STORAGE_OP_LABEL;
    mg_storage_label_request_t unauthorized={.instance_id=BLOCK_DEVICE_ID_BASE | (device.id+1U)};
    strcpy(unauthorized.filesystem,"fat32"); strcpy(unauthorized.label,"DENIED");
    label_frame_t denied={.rax=SYSCALL_STORAGE_LABEL,.rdi=(u64)(usize)&unauthorized};
    label_syscall(&denied);
    assert((i64)denied.rax==MG_ERR_PRIVILEGE_REQUIRED && !writes && !reads && !recovery_pins);
    prepare(); replace_before_dispatch=true;
    assert(native_label("NATIVE") == MG_ERR_DEVICE_GONE && !reads && !writes && !recovery_pins && !live_requested);
    replace_before_dispatch=false;
    prepare(); assert(native_label("NATIVE") == MG_OK && !recovery_pins && !live_requested);
    prepare(); fail_write_at=1U; fail_write_definite=true;
    assert(native_label("") == MG_ERR_UNCERTAIN); contained();
    char failed_clear[1024]; assert(strlen(argv[2])+14U < sizeof(failed_clear));
    snprintf(failed_clear,sizeof(failed_clear),"%s.failed-clear",argv[2]); save_image(failed_clear);
    discard_pin();
    prepare(); assert(native_label("too-long-label") == MG_ERR_BAD_ARGUMENT && !writes && !recovery_pins);
    prepare(); reject_finish=true;
    assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_UNCERTAIN); contained(); discard_pin();
    for (unsigned at=1; at<=wc; at++) {
        prepare(); remove_write=at;
        assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_UNCERTAIN);
        online=true; device.id++; unsigned before=writes;
        assert(vfs_recovery_pin_finish_handoff(recovery_pins->id)==VFS_ERR_UNCERTAIN && writes==before);
        discard_pin();
    }
    prepare(); replace_read=1U;
    assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_DEVICE_GONE && !writes && !recovery_pins && !live_requested);
    prepare(); reject_admission=true;
    assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_BUSY && !reads && !writes && !recovery_pins && !live_requested);
    reject_admission=false;
    prepare(); recovery_pin_count=VFS_MAX_RECOVERY_PINS;
    assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_NO_SPACE &&
        !reads && !writes && !recovery_pins && !live_requested);
    recovery_pin_count=0;
    prepare(); device.read_only=true;
    assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_ACCESS_DENIED && !writes && !live_requested);
    device.read_only=false; mount_table[0]=(vfs_mount_t){.active=true,.dev_id=device.id,.dev=&device};
    assert(fat32_set_label(&device,"NEWLABEL")==VFS_ERR_BUSY && !writes && !live_requested);
    memset(mount_table,0,sizeof(mount_table));
    printf("Label retained bounds: label=%zu growth=%zu owner=%zu pin=%zu FS=%zu root=%zu state=%zu aggregate32=%zu\n",
        sizeof(fat32_label_transaction_t),sizeof(fat32_directory_growth_t),sizeof(vfs_super_t),sizeof(vfs_recovery_pin_t),
        sizeof(fat32_fs_t),sizeof(vfs_node_t),sizeof(fat32_node_state_t),
        32U*(sizeof(fat32_label_transaction_t)+sizeof(fat32_directory_growth_t)+sizeof(fat32_label_fsinfo_t)+sizeof(vfs_super_t)+sizeof(vfs_recovery_pin_t)+sizeof(fat32_fs_t)+sizeof(vfs_node_t)+sizeof(fat32_node_state_t)));
    assert(!fclose(original_image)); puts("Production FAT32 label dispatch, private registry and failure containment passed"); return 0;
}
