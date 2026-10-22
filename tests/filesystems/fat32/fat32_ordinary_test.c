/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production ordinary FAT32 operations with controlled device/pin dependencies. */
#define main fat32_identity_fixture_main
#include "fat32_mount_identity_test.c"
#undef main
#include <time.h>

static int write_done(vfs_node_t *node, u64 at, u64 size, const void *input,
                      u64 *bytes)
{
    u64 pin = 0U;
    int result = fat32_write_pinned(node, at, size, input, bytes, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK) {
        *bytes = 0U; return VFS_ERR_UNCERTAIN;
    }
    return result;
}

static int resize_done(vfs_node_t *node, u64 size)
{
    u64 pin = 0U;
    int result = fat32_resize_pinned(node, size, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK)
        return VFS_ERR_UNCERTAIN;
    return result;
}

static vfs_node_t *ordinary_file(bool empty)
{
    entry(0U, "TARGET  BIN", empty ? 0U : 3U, empty ? 0U : 3U);
    memcpy(media[11], "old", 3U);
    vfs_node_t *file = fat32_finddir(&root, "TARGET.BIN"); assert(file);
    return file;
}

static void successes(void)
{
    u8 input[1301], output[4096];
    for (usize i = 0U; i < sizeof(input); i++) input[i] = (u8)(i * 17U);
    setup(); vfs_node_t *file = ordinary_file(true);
    u64 bytes = 0U, id = file->inode;
    assert(fat32_node_ops.write(file, 0U, 1U, "x") == 0U &&
           fat32_node_ops.truncate(file) == VFS_ERR_UNSUPPORTED && !writes);
    assert(write_done(file, 17U, sizeof(input), input, &bytes) == VFS_OK &&
           bytes == sizeof(input) && file->size == 1318U && file->inode == id);
    memset(output, 0xff, sizeof(output));
    assert(fat32_read(file, 0U, 1318U, output) == 1318U);
    for (usize i = 0U; i < 17U; i++) assert(!output[i]);
    assert(!memcmp(output + 17U, input, sizeof(input)) && fs.ownership_proven &&
           fat32_state(file)->allocation_count == 3U);
    u64 conflict_pin = 0U; unsigned before_writes = writes;
    assert(fat32_replace_locked(&super, id, 1U, 0U, NULL, NULL, &conflict_pin) ==
           VFS_ERR_STALE_GENERATION && !conflict_pin && writes == before_writes);
    assert(write_done(file, file->size, 3U, "end", &bytes) == VFS_OK && bytes == 3U);
    assert(resize_done(file, 514U) == VFS_OK && file->size == 514U &&
           fat32_state(file)->allocation_count == 2U);
    assert(resize_done(file, 2050U) == VFS_OK && file->size == 2050U &&
           fat32_state(file)->allocation_count == 5U);
    memset(output, 0xff, sizeof(output));
    assert(fat32_read(file, 514U, 1536U, output) == 1536U);
    for (usize i = 0U; i < 1536U; i++) assert(!output[i]);
    u64 generation = fat32_state(file)->generation;
    assert(resize_done(file, 2050U) == VFS_OK &&
           fat32_state(file)->generation == generation);
    assert(resize_done(file, 0U) == VFS_OK && !file->size &&
           !fat32_state(file)->first_cluster && !fat32_state(file)->allocation_count);
    assert(write_done(file, 0U, 3U, "new", &bytes) == VFS_OK && bytes == 3U &&
           fat32_read(file, 0U, 3U, output) == 3U && !memcmp(output, "new", 3U));
    assert(unlink_finished(&root, "TARGET.BIN") == VFS_OK);
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_NOT_FOUND && !bytes);
    cleanup(); assert(!live_requested);

    setup(); file = ordinary_file(true); id = file->inode;
    assert(rename_finished(&root, "TARGET.BIN", &root, "MOVED.BIN") == VFS_OK &&
           fat32_finddir(&root, "MOVED.BIN") == file);
    assert(write_done(file, 0U, 3U, "new", &bytes) == VFS_OK && bytes == 3U &&
           file->inode == id && fat32_read(file, 0U, 3U, output) == 3U &&
           !memcmp(output, "new", 3U));
    cleanup(); assert(!live_requested);

    setup(); file = ordinary_file(false);
    ((u32 *)media[1])[3] = 7U; ((u32 *)media[1])[7] = 5U;
    ((u32 *)media[1])[5] = FAT32_EOC_MIN;
    /* Inventory and trim allocation beyond logical EOF as well. */
    assert(resize_done(file, 3U) == VFS_OK && ((u32 *)media[1])[3] == FAT32_EOC_MIN &&
           !((u32 *)media[1])[7] && !((u32 *)media[1])[5]);
    cleanup(); assert(!live_requested);
}

static void geometries(void)
{
    const u32 sector_counts[] = {1U, 8U, 128U};
    for (unsigned geometry = 0U; geometry < 3U; geometry++) {
        for (unsigned mode = 0U; mode < 3U; mode++) {
            setup();
            fs.sectors_per_cluster = sector_counts[geometry];
            fs.cluster_size_bytes = fs.sectors_per_cluster * 512U;
            fs.fat_count = mode == 2U ? 2U : mode + 1U;
            for (u32 copy = 1U; copy < fs.fat_count; copy++)
                memcpy(media[1U + copy], media[1], 512U);
            if (mode == 2U) {
                fs.bpb.ext_flags = 0x81U;
                memset(media[1], 0x77, 512U); /* Inactive FAT is not authoritative. */
            }
            for (u32 cluster = 5U; cluster < fs.total_clusters; cluster += 2U)
                for (u32 copy = 0U; copy < fs.fat_count; copy++)
                    if (mode != 2U || copy == 1U)
                        ((u32 *)media[copy + 1U])[cluster] = FAT32_EOC_MIN;
            u8 inactive[512]; memcpy(inactive, media[1], 512U);
            entry(0U, "TARGET  BIN", 0U, 0U);
            vfs_node_t *file = fat32_finddir(&root, "TARGET.BIN"); assert(file);
            usize length = 3U * fs.cluster_size_bytes + 17U;
            u8 *input = malloc(length), *output = malloc(length); assert(input && output);
            for (usize i = 0U; i < length; i++) input[i] = (u8)(i * 11U);
            usize baseline = live_requested; peak_requested = baseline; u64 bytes;
            assert(write_done(file, 13U, length, input, &bytes) == VFS_OK && bytes == length);
            assert(fat32_read(file, 13U, length, output) == length && !memcmp(input, output, length));
            assert(fat32_state(file)->allocation_count == 4U);
            assert(resize_done(file, fs.cluster_size_bytes + 1U) == VFS_OK &&
                   fat32_state(file)->allocation_count == 2U);
            assert(resize_done(file, 0U) == VFS_OK);
            if (mode == 2U) assert(!memcmp(inactive, media[1], 512U));
            if (geometry == 2U && mode == 1U)
                printf("FAT32 64 KiB-cluster ordinary lifecycle: peak=%zu requested bytes\n",
                       (size_t)(peak_requested - baseline));
            free(input); free(output); cleanup(); assert(!live_requested);
        }
    }
}

static void partial_and_cancellation(void)
{
    u8 input[1536]; memset(input, 'p', sizeof(input)); u64 bytes;
    setup(); vfs_node_t *file = ordinary_file(false);
    ((u32 *)media[1])[3] = 7U; ((u32 *)media[1])[7] = 5U;
    ((u32 *)media[1])[5] = FAT32_EOC_MIN;
    file->size = ((fat32_on_disk_entry_t *)media[10])[0].file_size = 1536U;
    fail_read_lba = 15U; /* Read failure on the second data cluster, not mutation. */
    assert(write_done(file, 0U, sizeof(input), input, &bytes) == VFS_ERR_IO &&
           bytes == 512U && fs.version_confident && !active_pin &&
           fat32_state(file)->generation == 2U && !memcmp(media[11], input, 512U));
    cleanup(); assert(!live_requested);

    setup(); file = ordinary_file(true);
    for (u32 i = 5U; i < fs.total_clusters; i++) ((u32 *)media[1])[i] = FAT32_EOC_MIN;
    assert(write_done(file, 0U, sizeof(input), input, &bytes) == VFS_ERR_NO_SPACE &&
           bytes == 512U && file->size == 512U && fs.version_confident &&
           !active_pin && fat32_state(file)->first_cluster == 4U);
    cleanup(); assert(!live_requested);

    setup(); file = ordinary_file(false);
    for (u32 i = 5U; i < fs.total_clusters; i++) ((u32 *)media[1])[i] = FAT32_EOC_MIN;
    assert(resize_done(file, 1536U) == VFS_ERR_NO_SPACE && file->size == 3U &&
           fat32_state(file)->generation == 1U && !((u32 *)media[1])[4] &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN && fs.version_confident &&
           !active_pin && fs.ownership_proven && !memcmp(media[11], "old", 3U));
    cleanup(); assert(!live_requested);
}

static unsigned ordinary_replace_after_write;
static bool replacing_write(block_device_t *dev, u64 id, u64 lba, u32 count,
                            const void *input)
{
    scaling_write = NULL;
    bool result = block_write_for_id(dev, id, lba, count, input);
    scaling_write = replacing_write;
    if (writes == ordinary_replace_after_write) device.id++;
    return result;
}

static vfs_node_t *boundary_setup(unsigned kind)
{
    setup(); vfs_node_t *file = ordinary_file(kind == 1U);
    fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
    fs.fs_info_sector = 3U; fs.backup_boot_sector = 1U; fs.reserved_sector_count = 6U;
    for (u32 lba = 3U; lba <= 4U; lba++) {
        fat32_store_le32(media[lba], 0x41615252U);
        fat32_store_le32(media[lba] + 484U, 0x61417272U);
        fat32_store_le32(media[lba] + 508U, 0xaa550000U);
    }
    if (kind == 3U) {
        for (unsigned copy = 1U; copy <= 2U; copy++) {
            ((u32 *)media[copy])[3] = 7U; ((u32 *)media[copy])[7] = 5U;
            ((u32 *)media[copy])[5] = FAT32_EOC_MIN;
        }
        file->size = ((fat32_on_disk_entry_t *)media[10])[0].file_size = 1536U;
    }
    return file;
}

static int boundary_operation(vfs_node_t *file, unsigned kind, u64 *bytes, u64 *pin)
{
    static u8 input[1201]; memset(input, 'v', sizeof(input));
    if (kind >= 3U) return fat32_resize_pinned(file, kind == 3U ? 513U : 1201U, pin);
    return fat32_write_pinned(file, kind == 2U ? 700U : 0U,
                              kind == 0U ? 1U : sizeof(input), input, bytes, pin);
}

static void every_mutation_boundary(void)
{
    for (unsigned kind = 0U; kind < 5U; kind++) {
        vfs_node_t *file = boundary_setup(kind); u64 bytes = 0U, pin = 0U;
        assert(boundary_operation(file, kind, &bytes, &pin) == VFS_OK && pin);
        unsigned total_writes = writes, total_flushes = flushes;
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        cleanup(); assert(!live_requested);
        for (unsigned mode = 0U; mode < 6U; mode++) {
            unsigned total = mode == 3U ? total_flushes : total_writes;
            for (unsigned boundary = 1U; boundary <= total; boundary++) {
                file = boundary_setup(kind); bytes = pin = 0U;
                if (mode == 0U) { fail_write_at = boundary; fail_write_definite = true; }
                if (mode == 1U) { fail_write_at = boundary; fail_write_partial_bytes = 17U; }
                if (mode == 2U) { fail_write_at = boundary; fail_write_partial_bytes = 512U; }
                if (mode == 3U) fail_flush_at = boundary;
                if (mode == 4U) remove_after_writes = boundary;
                if (mode == 5U) {
                    ordinary_replace_after_write = boundary;
                    scaling_write = replacing_write;
                }
                int result = boundary_operation(file, kind, &bytes, &pin);
                assert(result == VFS_ERR_UNCERTAIN && !bytes && !pin &&
                       active_pin && pin_unresolved && fs.replace_transaction &&
                       !fs.version_confident && !fs.ownership_proven);
                fat32_replace_transaction_t *tx = fs.replace_transaction;
                assert(tx->ordinary && !tx->old_clusters && !tx->fresh_clusters &&
                       !tx->data_buffer && tx->ordinary->mutation_attempted);
                if (tx->metadata_write_pending && tx->metadata_cluster &&
                    tx->ordinary->fat_role == 0U)
                    assert(tx->ordinary->candidate == tx->metadata_cluster);
                if (tx->ordinary->old_tail_link_attempted)
                    assert(tx->ordinary->fresh_first && tx->ordinary->old_tail);
                /* Shrink never frees reachable old allocation before publication. */
                if (kind == 3U && !tx->ordinary->directory_confirmed)
                    assert(((u32 *)media[1])[3] == 7U && ((u32 *)media[1])[7] == 5U &&
                           ((u32 *)media[1])[5] == FAT32_EOC_MIN);
                unsigned before = writes;
                assert(fat32_write_pinned(file, 0U, 1U, "x", &bytes, &pin) ==
                       VFS_ERR_UNCERTAIN && !bytes && !pin && writes == before);
                assert(vfs_recovery_pin_finish_handoff(active_pin) == VFS_ERR_UNCERTAIN);
                cleanup(); assert(!live_requested);
            }
        }
    }
    puts("FAT32 ordinary write/resize: every data, FAT-copy, directory, FSInfo and flush boundary passed");
}

static void cancelled_growth_faults(void)
{
    for (unsigned mode = 0U; mode < 3U; mode++) {
        setup(); vfs_node_t *file = ordinary_file(false);
        for (u32 i = 5U; i < fs.total_clusters; i++) ((u32 *)media[1])[i] = FAT32_EOC_MIN;
        if (mode == 0U) { fail_write_at = 5U; fail_write_partial_bytes = 17U; }
        if (mode == 1U) fail_flush_at = 1U;
        if (mode == 2U) remove_after_writes = 5U;
        assert(resize_done(file, 1536U) == VFS_ERR_UNCERTAIN && active_pin &&
               pin_unresolved && fs.replace_transaction && !fs.version_confident &&
               file->size == 3U && ((u32 *)media[1])[3] == FAT32_EOC_MIN &&
               !memcmp(media[11], "old", 3U));
        fat32_replace_transaction_t *tx = fs.replace_transaction;
        assert(tx->ordinary->fresh_first == 4U && tx->ordinary->fresh_count == 1U &&
               !tx->ordinary->old_tail_link_attempted && !tx->ordinary->directory_attempted);
        cleanup(); assert(!live_requested);
    }
}

static void preparation_and_conflicts(void)
{
    for (unsigned fail = 1U; fail <= 8U; fail++) {
        setup(); vfs_node_t *file = ordinary_file(true); u64 bytes = 0U, pin = 0U;
        fail_allocation = allocations + fail;
        int result = fat32_write_pinned(file, 0U, 1U, "x", &bytes, &pin);
        if (result == VFS_OK) assert(pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        else assert(result == VFS_ERR_NO_MEM && !writes && !pin && !active_pin &&
                    !fs.replace_transaction && !file->size);
        cleanup(); assert(!live_requested);
    }
    setup(); vfs_node_t *file = ordinary_file(true); u64 bytes = 99U, pin = 0U;
    fail_pin_begin = true;
    assert(fat32_write_pinned(file, 0U, 1U, "x", &bytes, &pin) == VFS_ERR_NO_SPACE &&
           !bytes && !pin && !writes && !active_pin);
    cleanup();
    setup(); file = ordinary_file(false); entry(1U, "SHARED  BIN", 3U, 1U);
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_BAD_FORMAT &&
           !bytes && !writes && !active_pin && !memcmp(media[11], "old", 3U));
    cleanup();
    setup(); file = ordinary_file(false); ((u32 *)media[1])[3] = 3U;
    assert(resize_done(file, 0U) == VFS_ERR_BAD_FORMAT && !writes && !active_pin);
    cleanup();
    setup(); file = ordinary_file(false); fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U); ((u32 *)media[2])[3] = 0U;
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_BAD_FORMAT && !writes);
    cleanup();
    setup(); file = ordinary_file(false);
    assert(fat32_write_pinned(file, ~(u64)0, 1U, "x", &bytes, &pin) ==
           VFS_ERR_INVALID_PARAM && !bytes && !pin && !writes);
    assert(fat32_resize_pinned(file, 0x100000000ULL, &pin) == VFS_ERR_INVALID_PARAM && !writes);
    assert(fat32_write_pinned(file, 0U, 0U, NULL, &bytes, &pin) == VFS_OK && !writes);
    ((fat32_on_disk_entry_t *)media[10])[0].attr |= 1U;
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_ACCESS_DENIED && !writes);
    cleanup();
    setup(); file = ordinary_file(false); device.id++;
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_DEVICE_GONE && !writes);
    cleanup();
    setup(); file = ordinary_file(false); fail_pin_finish = true;
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_UNCERTAIN && !bytes &&
           active_pin && fs.replace_transaction && !fs.version_confident);
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(active_pin) == VFS_OK);
    cleanup(); assert(!live_requested);
}

static void proof_reuse_and_scaling(void)
{
    setup(); vfs_node_t *file = ordinary_file(false); u64 bytes;
    unsigned before = allocations;
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_OK);
    unsigned first_allocations = allocations - before, first_reads = reads;
    before = allocations; unsigned old_reads = reads;
    assert(write_done(file, 1U, 1U, "y", &bytes) == VFS_OK &&
           allocations - before == 2U && reads - old_reads < first_reads);
    assert(fs.ownership_proven);
    assert(fat32_mounted_write(&super, 1U, 1U, media[1]) && !fs.ownership_proven);
    before = allocations;
    assert(write_done(file, 2U, 1U, "z", &bytes) == VFS_OK &&
           allocations - before == first_allocations);
    /* Model a mounted low-level allocation change: rebuilding the certificate
     * must not revive an older count/tail summary after preparation failure. */
    u8 fat_sector[512]; memcpy(fat_sector, media[1], 512U);
    ((u32 *)fat_sector)[3] = 4U; ((u32 *)fat_sector)[4] = FAT32_EOC_MIN;
    assert(fat32_mounted_write(&super, 1U, 1U, fat_sector));
    fail_allocation = allocations + first_allocations - 1U;
    unsigned before_writes = writes;
    assert(write_done(file, 0U, 1U, "x", &bytes) == VFS_ERR_NO_MEM && !bytes &&
           writes == before_writes && fs.ownership_proven &&
           fat32_state(file)->allocation_count == 2U);
    fail_allocation = 0U;
    assert(resize_done(file, 3U) == VFS_OK && !((u32 *)media[1])[4] &&
           fat32_state(file)->allocation_count == 1U);
    cleanup(); assert(!live_requested);

    setup();
    fs.total_clusters = 8200U; fs.fat_size_sectors = 65U; fs.data_start_lba = 100U;
    memset(media[1], 0, 65U * 512U);
    ((u32 *)media[1])[2] = FAT32_EOC_MIN;
    root_state.first_cluster = 2U;
    fat32_on_disk_entry_t *disk = (void *)media[100];
    memcpy(disk->name, "LARGE   BIN", 11U); disk->attr = 0x20U;
    file = fat32_finddir(&root, "LARGE.BIN"); assert(file);
    usize length = 3U * 1024U * 1024U + 17U;
    u8 *input = malloc(length), *output = malloc(length); assert(input && output);
    for (usize i = 0U; i < length; i++) input[i] = (u8)(i * 7U);
    usize baseline = live_requested; peak_requested = baseline;
    clock_t start = clock();
    assert(write_done(file, 0U, length, input, &bytes) == VFS_OK && bytes == length);
    double elapsed_ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    unsigned operation_reads = reads;
    assert(fat32_read(file, 0U, length, output) == length && !memcmp(input, output, length));
    printf("FAT32 ordinary 3 MiB+17 growth: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline), operation_reads, writes, flushes, elapsed_ms);
    assert(resize_done(file, 513U) == VFS_OK && file->size == 513U &&
           fat32_state(file)->allocation_count == 2U);
    assert(resize_done(file, 0U) == VFS_OK && !fat32_state(file)->first_cluster);
    free(input); free(output); cleanup(); assert(!live_requested);
    printf("FAT32 ordinary recovery: common=%zu operation=%zu per_pin=%zu aggregate32=%zu requested bytes\n",
           sizeof(fat32_replace_transaction_t), sizeof(fat32_ordinary_t),
           sizeof(fat32_replace_transaction_t) + sizeof(fat32_ordinary_t),
           32U * (sizeof(fat32_replace_transaction_t) + sizeof(fat32_ordinary_t)));
}

/* Generated disk backing only: the proof, chain walk, mutation and cleanup
 * are production functions. Host FAT/data storage is excluded from heap counts. */
static u32 *huge_fat;
static u8 huge_directory[512], huge_tail_data[4][512];
static u32 huge_units;
static bool huge_read(block_device_t *dev, u64 id, u64 lba, u32 count, void *output)
{
    if (!online || dev != &device || id != device.id || count != 1U ||
        lba >= device.sector_count) return false;
    reads++;
    if (lba < fs.data_start_lba) {
        memcpy(output, (u8 *)huge_fat + (lba - fs.fat_start_lba) * 512U, 512U);
    } else {
        u32 cluster = (u32)(lba - fs.data_start_lba) + 2U;
        if (cluster == 2U) memcpy(output, huge_directory, 512U);
        else if (cluster >= huge_units + 2U && cluster < huge_units + 6U)
            memcpy(output, huge_tail_data[cluster - huge_units - 2U], 512U);
        else memset(output, 0, 512U);
    }
    return true;
}
static bool huge_write(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *input)
{
    if (!online || dev != &device || id != device.id || count != 1U ||
        lba >= device.sector_count) return false;
    writes++;
    if (lba < fs.data_start_lba)
        memcpy((u8 *)huge_fat + (lba - fs.fat_start_lba) * 512U, input, 512U);
    else {
        u32 cluster = (u32)(lba - fs.data_start_lba) + 2U;
        if (cluster == 2U) memcpy(huge_directory, input, 512U);
        else {
            assert(cluster >= huge_units + 2U && cluster < huge_units + 6U);
            memcpy(huge_tail_data[cluster - huge_units - 2U], input, 512U);
        }
    }
    return true;
}
static void large_fragmented(void)
{
    setup(); huge_units = 262144U;
    fs.total_clusters = huge_units + 10U; fs.fat_count = 2U;
    fs.fat_size_sectors = (fs.total_clusters + 127U) / 128U;
    fs.data_start_lba = 1U + 2U * fs.fat_size_sectors;
    fs.next_free_cluster = huge_units + 3U;
    device.sector_count = fs.data_start_lba + fs.total_clusters - 2U;
    huge_fat = calloc((size_t)fs.fat_size_sectors * 2U, 512U); assert(huge_fat);
    for (u32 copy = 0U; copy < 2U; copy++) {
        u32 *fat = huge_fat + copy * fs.fat_size_sectors * 128U;
        fat[2] = FAT32_EOC_MIN;
        for (u32 cluster = 3U; cluster < huge_units + 3U; cluster++) {
            if (cluster < huge_units / 2U + 3U) fat[cluster] = cluster + huge_units / 2U;
            else fat[cluster] = cluster == huge_units + 2U ? FAT32_EOC_MIN :
                                    cluster - huge_units / 2U + 1U;
        }
    }
    memset(huge_directory, 0, sizeof(huge_directory));
    memset(huge_tail_data, 0, sizeof(huge_tail_data));
    fat32_on_disk_entry_t *disk = (void *)huge_directory;
    memcpy(disk->name, "LARGE   BIN", 11U); disk->attr = 0x20U;
    disk->first_cluster_low = 3U; disk->file_size = huge_units * 512U;
    scaling_read = huge_read; scaling_write = huge_write;
    vfs_node_t *file = fat32_finddir(&root, "LARGE.BIN"); assert(file);
    usize baseline = live_requested; peak_requested = baseline;
    clock_t start = clock(); u64 bytes;
    assert(write_done(file, file->size - 1U, 2U, "xy", &bytes) == VFS_OK && bytes == 2U);
    double elapsed_ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    unsigned operation_reads = reads;
    u8 output[2];
    assert(fat32_read(file, (u64)huge_units * 512U - 1U, 2U, output) == 2U &&
           !memcmp(output, "xy", 2U));
    printf("FAT32 128 MiB fragmented ordinary append: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline), operation_reads, writes, flushes, elapsed_ms);
    /* A later append uses the proved, confirmed tail; it does not rescan the file. */
    unsigned previous_reads = reads; start = clock();
    assert(write_done(file, file->size, 1U, "z", &bytes) == VFS_OK && bytes == 1U &&
           reads - previous_reads < 10U);
    printf("FAT32 cached-tail append: reads=%u cpu_ms=%.3f\n", reads - previous_reads,
           (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC);
    start = clock(); previous_reads = reads; unsigned previous_writes = writes;
    assert(resize_done(file, 0U) == VFS_OK && !file->size);
    printf("FAT32 128 MiB fragmented truncate: reads=%u writes=%u cpu_ms=%.3f\n",
           reads - previous_reads, writes - previous_writes,
           (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC);
    for (u32 copy = 0U; copy < 2U; copy++)
        for (u32 cluster = 3U; cluster < huge_units + 4U; cluster++)
            assert(!huge_fat[copy * fs.fat_size_sectors * 128U + cluster]);
    cleanup(); assert(!live_requested); free(huge_fat); huge_fat = NULL;
}

static void image_case(const char *path)
{
    setup();
    image_file = fopen(path, "r+b"); assert(image_file); image_writable = true;
    assert(!fseeko(image_file, 0, SEEK_END));
    image_sectors = (u64)ftello(image_file) / 512U;
    device.id = 904U; device.sector_size = 512U; device.sector_count = image_sectors; online = true;
    writes = reads = flushes = image_reads = 0U;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    vfs_node_t *file = fat32_finddir(mounted->root_node, "TARGET.BIN"); assert(file);
    usize length = 3U * 1024U * 1024U + 17U; u64 bytes;
    u8 *input = malloc(length), *output = malloc(length); assert(input && output);
    for (usize i = 0U; i < length; i++) input[i] = (u8)(i * 7U);
    clock_t start = clock();
    assert(write_done(file, 0U, length, input, &bytes) == VFS_OK && bytes == length);
    assert(write_done(file, length, 4U, "TAIL", &bytes) == VFS_OK && bytes == 4U);
    assert(resize_done(file, length + 1000U) == VFS_OK);
    assert(resize_done(file, length + 4U) == VFS_OK);
    assert(fat32_read(file, 0U, length, output) == length && !memcmp(input, output, length));
    char tail[4]; assert(fat32_read(file, length, 4U, tail) == 4U && !memcmp(tail, "TAIL", 4U));
    printf("FAT32 independent-image ordinary growth/readback: reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           reads, writes, flushes, (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC);
    free(input); free(output);
    vfs_node_t *root_node = mounted->root_node;
    assert(fat32_unmount(mounted) == VFS_OK);
    kfree(file); kfree(root_node); kfree(mounted);
    assert(!live_requested); fclose(image_file); image_file = NULL; image_writable = false;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--image")) { image_case(argv[2]); return 0; }
    successes(); geometries(); partial_and_cancellation(); every_mutation_boundary();
    cancelled_growth_faults(); preparation_and_conflicts(); proof_reuse_and_scaling(); large_fragmented();
    puts("FAT32 production ordinary write, growth, resize and recovery passed");
    return 0;
}
