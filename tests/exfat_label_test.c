/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual driver and private VFS registry; controlled block/heap/lock services. */
#include <storage/gpt.h>
#define EXFAT_REAL_RECOVERY
#define main unused_identity_main
#include "exfat_mount_identity_test.c"
#undef main
#include "vfs_label_owner.inc"

static u8 original[sizeof(media)];
static bool reject_finish;
static bool management_held;
bool storage_mutation_begin(void) { if (management_held) return false; management_held=true; return true; }
void storage_mutation_end(void) { assert(management_held); management_held=false; }
bool storage_mutation_busy(void) { return management_held; }
static unsigned disposed;
void mutex_init(mutex_t *lock) { memset(lock, 0, sizeof(*lock)); }
bool mutex_lock(mutex_t *lock) {
    assert(!lock->owner); lock->owner = (void *)1; return true;
}
bool mutex_try_lock(mutex_t *lock) {
    if (reject_finish || lock->owner) return false;
    lock->owner = (void *)1; return true;
}
bool mutex_unlock(mutex_t *lock) {
    assert(lock->owner); lock->owner = NULL; return true;
}
u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->value); lock->value = 1; return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    assert(lock->value && !flags); lock->value = 0;
}
bool block_device_snapshot(const block_device_t *dev, block_device_t *out) {
    if (!dev || !out || !online || dev != &device) return false;
    *out = *dev; return true;
}
bool gpt_get_partition_info(block_device_t *dev, gpt_partition_info_t *out) {
    (void)dev; (void)out; return false;
}
static void prepare(void) {
    device.read_only = false;
    assert(!recovery_pins && !live_requested);
    setup(); device.sector_count = 12000; memcpy(media, original, sizeof(media));
    fs.boot.heap_offset = exfat_le32(media[0] + 88);
    fs.device_id = device.id;
    recovery_pin_count = 0; next_recovery_pin_id = 1;
    reject_finish = false;
}
static void discard_private_pin(void) {
    assert(recovery_pins && recovery_pin_count == 1);
    vfs_recovery_pin_t *pin = recovery_pins;
    vfs_super_t *owner = pin->sb;
    assert(owner->state == VFS_SUPER_DETACHING && owner->reference_count == 1);
    exfat_fs_t *private_fs = owner->private_data;
    exfat_replace_free(pin->handoff_context);
    if (private_fs) private_fs->replace_transaction = NULL;
    recovery_pins = NULL; recovery_pin_count = 0;
    kfree(pin); vfs_super_release(owner);
    assert(!live_requested);
    disposed++;
}
static void assert_uncertain(void) {
    assert(recovery_pin_count == 1 && recovery_pins &&
        recovery_pins->state == VFS_RECOVERY_PIN_UNRESOLVED);
    vfs_super_t *owner = recovery_pins->sb;
    exfat_fs_t *private_fs = owner->private_data;
    assert(private_fs && private_fs->replace_transaction && !private_fs->version_confident);
    unsigned before = writes;
    assert(vfs_format_preflight(&device) == VFS_ERR_BUSY &&
        vfs_device_already_mounted(&device));
    assert(exfat_set_label(&device, "retry") == VFS_ERR_BUSY && writes == before);
    assert(vfs_recovery_pin_finish_handoff(recovery_pins->id) == VFS_ERR_UNCERTAIN);
}
int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *image = fopen(argv[1], "r+b");
    assert(image && fread(original, sizeof(original), 1, image) == 1);
    prepare();
    clock_t started = clock();
    int initial_result = exfat_set_label(&device, "R\xc3\xa9sum\xc3\xa9");
    if (initial_result) fprintf(stderr, "label initial result=%d layout=%s writes=%u reads=%u\n", initial_result, argv[2], writes, reads);
    assert(initial_result == VFS_OK && !recovery_pins && !live_requested);
    unsigned total_writes = writes, total_flushes = flushes, total_allocs = allocations;
    printf("exFAT private label layout=%s: peak=%zu reads=%u writes=%u flushes=%u allocations=%u cpu_ms=%.3f\n",
        argv[2], (size_t)peak_requested, reads, writes, flushes, allocations,
        1000.0*(double)(clock()-started)/CLOCKS_PER_SEC);
    assert(!(exfat_le16(media[0]+106)&2) && !(exfat_le16(media[12]+106)&2));
    rewind(image); assert(fwrite(media, sizeof(media), 1, image) == 1 && !fclose(image));
    char label[64];
    assert(exfat_read_label_internal(&device, label, sizeof(label)) && !strcmp(label, "R\xc3\xa9sum\xc3\xa9") && !live_requested);
    assert(exfat_set_label(&device, "next") == VFS_OK && !live_requested);
    assert(exfat_set_label(&device, "") == VFS_OK && !live_requested);
    /* Reject mounted/read-only owners without even loading driver state. */
    prepare(); mount_table[0] = (vfs_mount_t){.active=true, .dev_id=device.id, .dev=&device};
    assert(exfat_set_label(&device, "no") == VFS_ERR_BUSY && !writes && !live_requested);
    memset(mount_table, 0, sizeof(mount_table));
    device.read_only = true;
    assert(exfat_set_label(&device, "no") == VFS_ERR_ACCESS_DENIED && !writes && !live_requested);
    device.read_only = false;
    /* All allocation, sector-write and flush boundaries. A callback failure
     * can already have copied bytes; uncertainty must keep its exact owner. */
    for (unsigned fail = 1; fail <= total_allocs; fail++) {
        prepare(); fail_allocation = fail;
        int result = exfat_set_label(&device, "R\xc3\xa9sum\xc3\xa9");
        assert(result != VFS_OK && !writes && !recovery_pins && !live_requested);
        assert(!memcmp(media, original, sizeof(media)));
    }
    for (unsigned fail = 1; fail <= total_writes; fail++) {
        prepare(); fail_write_number = fail; fail_write_copy_bytes = 128;
        assert(exfat_set_label(&device, "R\xc3\xa9sum\xc3\xa9") == VFS_ERR_UNCERTAIN);
        assert_uncertain();
        printf("label write fault=%u retained=%zu\n", fail, (size_t)live_requested);
        discard_private_pin();
    }
    for (unsigned fail = 1; fail <= total_flushes; fail++) {
        prepare(); fail_flush_number = fail;
        assert(exfat_set_label(&device, "R\xc3\xa9sum\xc3\xa9") == VFS_ERR_UNCERTAIN);
        assert_uncertain(); discard_private_pin();
    }
    for (unsigned fail = 1; fail <= total_writes; fail++) {
        prepare(); remove_after_write_number = fail;
        assert(exfat_set_label(&device, "R\xc3\xa9sum\xc3\xa9") == VFS_ERR_UNCERTAIN);
        assert(recovery_pins && recovery_pins->state == VFS_RECOVERY_PIN_UNRESOLVED);
        u64 previous_id = device.id;
        online = true; device.id++;
        assert(!block_device_id_is_live(previous_id));
        unsigned previous_writes = writes;
        assert(vfs_recovery_pin_finish_handoff(recovery_pins->id) == VFS_ERR_UNCERTAIN && writes == previous_writes);
        discard_private_pin();
    }
    prepare(); reject_finish = true;
    assert(exfat_set_label(&device, "R\xc3\xa9sum\xc3\xa9") == VFS_ERR_UNCERTAIN);
    assert_uncertain(); discard_private_pin();
    prepare(); replace_after_reads = 1;
    assert(exfat_set_label(&device, "no") != VFS_OK && !writes && !recovery_pins && !live_requested);
    printf("private owner: %u retained fault transactions verified and explicitly discarded by fixture\n", disposed);
    size_t retained = sizeof(exfat_replace_transaction_t) + sizeof(exfat_directory_growth_t) +
        sizeof(exfat_boot_cleanup_t) + sizeof(exfat_percent_transaction_t) + sizeof(vfs_recovery_pin_t) + sizeof(vfs_super_t) +
        sizeof(vfs_node_t) + sizeof(exfat_node_t) + sizeof(exfat_fs_t) + EXFAT_MAX_UPCASE_BYTES;
    printf("label bounds: tx=%zu boot_cleanup=%zu registry_pin=%zu owner=%zu max_retained=%zu aggregate32=%zu (requested bytes, excludes allocator overhead)\n",
        sizeof(exfat_replace_transaction_t), sizeof(exfat_boot_cleanup_t), sizeof(vfs_recovery_pin_t),
        sizeof(vfs_super_t), retained, retained*32);
    return 0;
}
