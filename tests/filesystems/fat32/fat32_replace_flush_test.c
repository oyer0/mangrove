/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production replacement/sync; controlled block callbacks, heap and pin owner. */
#define main fat32_identity_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

typedef struct {
    fat32_replace_phase_t phase;
    u32 writes, reclamation, fsinfo, old_link, directory_first;
    u8 attempted, confirmed;
} checkpoint_t;
static checkpoint_t checkpoints[4];
static unsigned remove_at_flush, replace_at_flush;

static bool observe_flush(block_device_t *dev, u64 id)
{
    assert(dev == &device && id == fs.device_id);
    unsigned at = flushes++;
    assert(at < 4U);
    fat32_replace_transaction_t *tx = fs.replace_transaction;
    if (tx) {
        checkpoints[at] = (checkpoint_t){tx->phase, writes,
            tx->reclamation_cursor, tx->fsinfo_cursor, ((u32 *)media[1])[3],
            fat32_entry_first((fat32_on_disk_entry_t *)media[10]),
            tx->replacement_flush_attempted, tx->replacement_flush_confirmed};
        unsigned calls = flushes;
        assert(fat32_sync(&super) == VFS_ERR_UNCERTAIN && flushes == calls);
    }
    if (flushes == remove_at_flush) online = false;
    if (flushes == replace_at_flush) device.id++;
    return online && id == device.id && (!fail_flush_at || flushes != fail_flush_at);
}
static vfs_node_t *prepare(unsigned copies)
{
    setup();
    memset(checkpoints, 0, sizeof(checkpoints)); remove_at_flush = replace_at_flush = 0;
    flush_hook = observe_flush;
    entry(0, "TARGET  TXT", 3U, 700U);
    ((u32 *)media[1])[3] = 7U; ((u32 *)media[1])[7] = FAT32_EOC_MIN;
    ((u32 *)media[1])[5] = FAT32_EOC_MIN; /* Fragment fresh data: 4 -> 6. */
    memcpy(media[11], "old", 3U); memcpy(media[15], "tail", 4U);
    fs.fat_count = copies;
    for (unsigned copy = 1U; copy < copies; copy++) memcpy(media[1U+copy], media[1], 512U);
    fs.reserved_sector_count = 8U; fs.fs_info_sector = 5U; fs.backup_boot_sector = 1U;
    for (unsigned sector = 5U; sector <= 6U; sector++) {
        fat32_store_le32(media[sector], 0x41615252U);
        fat32_store_le32(media[sector]+484U, 0x61417272U);
        fat32_store_le32(media[sector]+488U, 58U);
        fat32_store_le32(media[sector]+492U, 4U);
        fat32_store_le32(media[sector]+508U, 0xaa550000U);
    }
    vfs_node_t *file = fat32_finddir(&root, "target.txt"); assert(file);
    return file;
}
static int replace(vfs_node_t *file, u64 length, u64 short_at, u64 *pin)
{
    replace_source_state_t source = {.short_at = short_at};
    return fat32_replace_locked(&super, file->inode, 1U, length,
        length ? replace_source : NULL, &source, pin);
}
static void assert_contained(vfs_node_t *file)
{
    fat32_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx && active_pin && pin_unresolved && !fs.version_confident && !tx->data_buffer);
    if (!(tx->replacement_flush_confirmed & FAT32_REPLACE_FLUSH_PUBLISHED)) {
        for (unsigned copy = 0; copy < fs.fat_count; copy++)
            assert(((u32 *)media[1U+copy])[3] == 7U &&
                   ((u32 *)media[1U+copy])[7] == FAT32_EOC_MIN);
        assert(!tx->reclamation_cursor);
    }
    unsigned issued = writes, barriers = flushes; u64 pin;
    assert(replace(file, 3U, ~(u64)0, &pin) == VFS_ERR_UNCERTAIN && !pin &&
        writes == issued && flushes == barriers);
    assert(fat32_sync(&super) == VFS_ERR_UNCERTAIN && flushes == barriers);
    assert(vfs_recovery_pin_finish_handoff(active_pin) == VFS_ERR_UNCERTAIN && fs.replace_transaction == tx);
}
static void boundary_tests(void)
{
    unsigned injected = 0U;
    for (unsigned copies = 1U; copies <= 4U; copies++) {
        vfs_node_t *file = prepare(copies); usize baseline = live_requested;
        peak_requested = baseline; clock_t start = clock(); u64 pin;
        assert(replace(file, 700U, ~(u64)0, &pin) == VFS_OK && pin && flushes == 3U);
        fat32_replace_transaction_t *tx = fs.replace_transaction;
        assert(tx->conditional_replacement && tx->publication_confirmed &&
            tx->replacement_flush_attempted == 7U && tx->replacement_flush_confirmed == 7U);
        assert(checkpoints[0].phase == FAT32_REPLACE_STAGING_FLUSHING &&
            checkpoints[0].directory_first == 3U && checkpoints[0].old_link == 7U && !checkpoints[0].reclamation);
        assert(checkpoints[1].phase == FAT32_REPLACE_PUBLICATION_FLUSHING &&
            checkpoints[1].directory_first == 4U && checkpoints[1].old_link == 7U && !checkpoints[1].reclamation);
        assert(checkpoints[2].phase == FAT32_REPLACE_COMPLETION_FLUSHING &&
            !checkpoints[2].old_link && checkpoints[2].reclamation == 2U && checkpoints[2].fsinfo == 2U);
        unsigned total_writes = writes;
        printf("FAT32 replacement copies=%u: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
            copies, (size_t)(peak_requested-baseline), (size_t)(live_requested-baseline),
            reads, writes, flushes, 1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && !active_pin && fs.version_confident && flushes == 3U);
        u8 data[700]; assert(fat32_read(file, 0U, sizeof(data), data) == sizeof(data));
        for (u32 i = 0; i < sizeof(data); i++) assert(data[i] == (u8)i);
        assert(live_requested == baseline); cleanup();
        for (unsigned at = 1; at <= total_writes; at++)
            for (unsigned torn = 0; torn < 3U; torn++) {
                file = prepare(copies); fail_write_at = at;
                fail_write_definite = torn == 0U; fail_write_partial_bytes = torn == 1U ? 128U : 512U;
                assert(replace(file, 700U, ~(u64)0, &pin) == VFS_ERR_UNCERTAIN && !pin && writes == at);
                assert_contained(file); cleanup(); assert(!live_requested); injected++;
            }
        for (unsigned at = 1; at <= 3U; at++)
            for (unsigned fault = 0; fault < 3U; fault++) {
                file = prepare(copies);
                if (!fault) fail_flush_at = at;
                if (fault == 1U) remove_at_flush = at;
                if (fault == 2U) replace_at_flush = at;
                assert(replace(file, 700U, ~(u64)0, &pin) == VFS_ERR_UNCERTAIN && !pin && flushes == at);
                assert(fs.replace_transaction->replacement_flush_attempted == (1U << at)-1U &&
                    fs.replace_transaction->replacement_flush_confirmed == (1U << (at-1U))-1U);
                assert_contained(file); cleanup(); assert(!live_requested); injected++;
            }
        for (unsigned at = 1; at <= total_writes; at++) {
            file = prepare(copies); remove_after_writes = at;
            assert(replace(file, 700U, ~(u64)0, &pin) == VFS_ERR_UNCERTAIN && !pin);
            assert_contained(file); cleanup(); assert(!live_requested); injected++;
        }
    }
    printf("FAT32 replacement write/flush/removal boundaries: %u verified\n", injected);
}
static void cancellation_and_completion(void)
{
    for (unsigned fault = 0; fault < 4U; fault++) {
        vfs_node_t *file = prepare(2U); u64 pin;
        if (fault == 1U) fail_flush_at = 1U;
        if (fault == 2U) remove_at_flush = 1U;
        if (fault == 3U) replace_at_flush = 1U;
        int result = replace(file, 700U, 512U, &pin);
        assert(flushes == 1U && checkpoints[0].phase == FAT32_REPLACE_CANCELLATION_FLUSHING &&
            checkpoints[0].directory_first == 3U && checkpoints[0].old_link == 7U && file->size == 700U);
        assert(!((u32 *)media[1])[4] && !((u32 *)media[1])[6]);
        if (!fault) {
            assert(result == VFS_ERR_IO && pin && !pin_unresolved);
            assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && fs.version_confident);
        } else {
            assert(result == VFS_ERR_UNCERTAIN && !pin &&
                fs.replace_transaction->cancellation_cursor == 2U);
            assert_contained(file);
        }
        cleanup(); assert(!live_requested);
    }
    vfs_node_t *file = prepare(1U); u64 pin;
    assert(fat32_replace_locked(&super, file->inode, 2U, 1U, replace_source,
        &(replace_source_state_t){~(u64)0}, &pin) == VFS_ERR_STALE_GENERATION && !pin && !writes && !flushes);
    assert(replace(file, 0U, ~(u64)0, &pin) == VFS_OK && flushes == 2U &&
        fs.replace_transaction->replacement_flush_attempted == 6U &&
        fs.replace_transaction->replacement_flush_confirmed == 7U);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    unsigned previous = flushes; fs.fs_info_sector = fs.backup_boot_sector = 0U;
    assert(replace(file, 0U, ~(u64)0, &pin) == VFS_ERR_STALE_GENERATION && flushes == previous);
    assert(fat32_replace_locked(&super, file->inode, 2U, 0U, NULL, NULL, &pin) == VFS_OK && flushes == previous+1U);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK); cleanup();
    file = prepare(1U);
    assert(replace(file, 700U, ~(u64)0, &pin) == VFS_OK);
    /* Handoff cannot release incomplete checkpoints, even with a published phase. */
    fs.replace_transaction->replacement_flush_confirmed &= ~FAT32_REPLACE_FLUSH_COMPLETED;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_UNCERTAIN && fs.replace_transaction);
    cleanup();
    file = prepare(1U);
    assert(replace(file, 700U, ~(u64)0, &pin) == VFS_OK);
    online = false; device.id++; /* Removal after all I/O was confirmed. */
    unsigned previous_writes = writes; previous = flushes;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && !fs.replace_transaction &&
        writes == previous_writes && flushes == previous);
    cleanup();
    file = prepare(2U);
    for (unsigned copy = 0; copy < 2U; copy++)
        for (u32 cluster = 4U; cluster < fs.total_clusters; cluster++)
            ((u32 *)media[1U+copy])[cluster] = FAT32_EOC_MIN;
    assert(replace(file, 3U, ~(u64)0, &pin) == VFS_ERR_NO_SPACE && pin &&
        !writes && !flushes && !fs.replace_transaction->fresh_count &&
        fs.replace_transaction->replacement_flush_attempted == 0U &&
        fs.replace_transaction->replacement_flush_confirmed == FAT32_REPLACE_FLUSH_CANCELLED);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && fs.version_confident);
    cleanup();
}
static void sync_cases(void)
{
    prepare(1U); super.read_only = true;
    usize heap = live_requested; unsigned allocated = allocations;
    assert(fat32_super_ops.sync == fat32_sync && fat32_sync(&super) == VFS_OK &&
        flushes == 1U && !writes && live_requested == heap && allocations == allocated);
    fail_flush_at = 2U;
    assert(fat32_sync(&super) == VFS_ERR_IO && !fs.version_confident && !fs.ownership_proven);
    assert(fat32_sync(&super) == VFS_ERR_UNCERTAIN && flushes == 2U); cleanup();
    prepare(1U); device.id++;
    assert(fat32_sync(&super) == VFS_ERR_DEVICE_GONE && !flushes && !writes); cleanup();
    prepare(1U); replace_at_flush = 1U;
    assert(fat32_sync(&super) == VFS_ERR_DEVICE_GONE && !fs.version_confident && !writes); cleanup();
    assert(fat32_sync(NULL) == VFS_ERR_DEVICE_GONE);
}
static void maximum_inventory(void)
{
    setup(); fs.total_clusters = 8300U; fs.fat_size_sectors = 65U; fs.data_start_lba = 80U;
    fat32_on_disk_entry_t *disk = (fat32_on_disk_entry_t *)media[80];
    memcpy(disk->name, "TARGET  TXT", 11U); disk->attr = 0x20U;
    disk->first_cluster_low = 3U; disk->file_size = FAT32_REPLACE_MAX_BYTES;
    for (u32 cluster = 3U; cluster <= 8193U; cluster += 2U)
        ((u32 *)media[1U+cluster/128U])[cluster%128U] =
            cluster == 8193U ? FAT32_EOC_MIN : cluster+2U;
    memcpy(media[81], "old", 3U);
    vfs_node_t *file = fat32_finddir(&root, "TARGET.TXT"); assert(file);
    usize baseline = live_requested; peak_requested = baseline; clock_t start = clock();
    u64 pin; assert(replace(file, FAT32_REPLACE_MAX_BYTES, ~(u64)0, &pin) == VFS_OK && flushes == 3U);
    assert(fs.replace_transaction->old_count == 4096U && fs.replace_transaction->fresh_count == 4096U);
    assert(live_requested-baseline == sizeof(fat32_replace_transaction_t)+8192U*sizeof(u32));
    printf("FAT32 fragmented 2 MiB old/new: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
        (size_t)(peak_requested-baseline), (size_t)(live_requested-baseline), reads, writes, flushes,
        1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && !memcmp(media[81], "old", 3U));
    u8 byte; assert(fat32_read(file, FAT32_REPLACE_MAX_BYTES-1U, 1U, &byte) == 1U && byte == 255U);
    for (u32 cluster = 3U; cluster <= 8193U; cluster += 2U)
        assert(!((u32 *)media[1U+cluster/128U])[cluster%128U]);
    cleanup(); assert(!live_requested);
}
static void image_failure(const char *path, unsigned boundary)
{
    setup(); flush_hook = NULL;
    image_file = fopen(path, "r+b"); assert(image_file);
    assert(fseeko(image_file, 0, SEEK_END) == 0);
    image_sectors = (u64)ftello(image_file)/512U; image_writable = true;
    device.id = 902U; device.sector_size = 512U; device.sector_count = image_sectors;
    vfs_super_t *mounted; assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    vfs_node_t *file = fat32_finddir(mounted->root_node, "TARGET.TXT"); assert(file);
    replace_source_state_t source = {.short_at = boundary == 4U ? 512U : ~(u64)0};
    fail_flush_at = boundary == 4U ? 1U : boundary;
    u64 pin;
    assert(fat32_replace_locked(mounted, file->inode, 1U, 512U*1024U+1U,
        replace_source, &source, &pin) == VFS_ERR_UNCERTAIN && !pin && pin_unresolved);
    fat32_fs_t *state = mounted->private_data;
    assert(state->replace_transaction && !state->version_confident && state->replace_transaction->pin_id == active_pin);
    printf("Disposable FAT32 failed flush=%u: retained=%zu writes=%u flushes=%u phase=%u; runtime outcome uncertain\n",
        boundary, (size_t)(sizeof(fat32_replace_transaction_t) +
        (state->replace_transaction->fresh_capacity + state->replace_transaction->old_count)*sizeof(u32)),
        writes, flushes, state->replace_transaction->phase);
    /* Explicit fixture teardown discards evidence; no production recovery is claimed. */
    fat32_replace_free(state->replace_transaction); state->replace_transaction = NULL;
    active_pin = 0;
    vfs_node_t *nodes[2]; unsigned count = 0;
    for (fat32_node_state_t *node = state->nodes; node; node = node->next) { assert(count < 2U); nodes[count++] = node->node; }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (unsigned i = 0; i < count; i++) kfree(nodes[i]);
    kfree(mounted); assert(!live_requested); assert(!fclose(image_file)); image_file = NULL;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--image")) { setup(); replacement_image_case(argv[2]); return 0; }
    if (argc == 4 && !strcmp(argv[1], "--image-fail")) { image_failure(argv[3], (unsigned)atoi(argv[2])); return 0; }
    assert(argc == 1); boundary_tests(); cancellation_and_completion(); sync_cases();
    replacement_large_case(false, FAT32_REPLACE_MAX_BYTES);
    replacement_large_case(true, FAT32_REPLACE_MAX_BYTES);
    maximum_inventory();
    size_t maximum = sizeof(fat32_replace_transaction_t) + 8192U*sizeof(u32);
    printf("FAT32 replacement metadata: fixed=%zu max_retained=%zu aggregate32=%zu; scratch peak separate\n",
        sizeof(fat32_replace_transaction_t), maximum, 32U*maximum);
    puts("FAT32 conditional replacement flush ordering, cancellation, handoff and sync passed");
}
