/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production deletion and ownership proof; generated media models I/O only. */
#define FAT32_SCALING_NO_MAIN
#include "fat32_ownership_scaling_test.c"

static u32 *mutable_fat;
static u8 mutable_directory[512];
static unsigned retire_read_failure_after, replace_retire_write;

static bool retirement_read(block_device_t *dev, u64 id, u64 lba,
                             u32 count, void *buffer)
{
    if (!online || dev != &device || id != device.id || count != 1U ||
        (retire_read_failure_after && writes >= retire_read_failure_after))
        return false;
    reads++;
    if (lba >= fs.fat_start_lba && lba < fs.data_start_lba) {
        fat_reads++;
        memcpy(buffer, (u8 *)mutable_fat + (lba - fs.fat_start_lba) * 512U, 512U);
    } else {
        assert(lba == fs.data_start_lba);
        directory_reads++;
        memcpy(buffer, mutable_directory, 512U);
    }
    return true;
}

static bool retirement_write(block_device_t *dev, u64 id, u64 lba,
                              u32 count, const void *buffer)
{
    if (!online || dev != &device || id != device.id || count != 1U) return false;
    u8 *destination;
    if (lba >= fs.fat_start_lba && lba < fs.data_start_lba)
        destination = (u8 *)mutable_fat + (lba - fs.fat_start_lba) * 512U;
    else {
        assert(lba == fs.data_start_lba);
        destination = mutable_directory;
        directory_writes++;
    }
    writes++;
    if (writes == fail_write_at) {
        if (!fail_write_definite) memcpy(destination, buffer, fail_write_partial_bytes);
        return false;
    }
    memcpy(destination, buffer, 512U);
    if (remove_after_writes && --remove_after_writes == 0U) online = false;
    if (writes == replace_retire_write) device.id++;
    return true;
}

static void retirement_large_setup(u32 count, bool fragment, u32 copies)
{
    generated_setup(LARGE_FILE, count, fragment);
    fs.fat_count = copies;
    fs.data_start_lba = 1U + copies * fs.fat_size_sectors;
    device.sector_count = (u64)fs.data_start_lba + fs.total_clusters - 2U;
    /* Host-only device contents; never counted as kernel requested memory. */
    mutable_fat = calloc((size_t)copies * fs.fat_size_sectors, 512U);
    assert(mutable_fat);
    for (u32 copy = 0U; copy < copies; copy++)
        for (u32 cluster = 0U; cluster < fs.total_clusters; cluster++)
            mutable_fat[copy * fs.fat_size_sectors * 128U + cluster] =
                generated_fat(cluster);
    memset(mutable_directory, 0, sizeof(mutable_directory));
    generated_entry(mutable_directory, 0U, "LARGE   BIN", 3U, count * 512U, false);
    retire_read_failure_after = replace_retire_write = 0U;
    scaling_read = retirement_read;
    scaling_write = retirement_write;
}

static void retirement_large_cleanup(void)
{
    cleanup();
    assert(!live_requested);
    free(mutable_fat);
    mutable_fat = NULL;
}

static void successful_large(u32 count, bool fragment, u32 copies)
{
    retirement_large_setup(count, fragment, copies);
    vfs_node_t *opened = fat32_finddir(&root, "LARGE.BIN");
    assert(opened);
    u64 identity = opened->inode;
    usize baseline = live_requested;
    peak_requested = baseline;
    clock_t start = clock();
    u64 pin = 0U;
    assert(fat32_unlink_pinned(&root, "LARGE.BIN", &pin) == VFS_OK && pin);
    fat32_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx && !tx->old_clusters && tx->reclaim_streaming &&
           tx->old_count == count && tx->reclamation_cursor == count &&
           !tx->reclaim_successor_valid && tx->reclaim_cluster == FAT32_EOC_MIN &&
           tx->retirement->short_confirmed && !fat32_state(opened)->alive);
    assert(live_requested - baseline == sizeof(*tx) + sizeof(*tx->retirement));
    for (u32 copy = 0U; copy < copies; copy++)
        for (u32 cluster = 3U; cluster < count + 3U; cluster++)
            assert(mutable_fat[copy * fs.fat_size_sectors * 128U + cluster] == 0U);
    printf("FAT32 file retirement %u clusters fragmented=%u copies=%u: "
           "peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           count, fragment, copies, (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline), reads, writes, flushes,
           (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC);
    assert(writes == 1U + count * copies && flushes == 3U);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           !fs.replace_transaction && fs.version_confident && !fs.namespace_hidden);
    vfs_file_facts_t facts;
    assert(fat32_file_facts(opened, &facts) == VFS_ERR_NOT_FOUND);
    unsigned before = writes;
    assert(fat32_unlink_pinned(&root, "LARGE.BIN", &pin) == VFS_ERR_NOT_FOUND &&
           !pin && writes == before);
    /* Simulate imported slot reuse: the tombstoned identity cannot revive. */
    generated_entry(mutable_directory, 0U, "OTHER   BIN", 0U, 0U, false);
    vfs_node_t *reused = fat32_finddir(&root, "OTHER.BIN");
    assert(reused && reused != opened && reused->inode != identity &&
           fat32_file_facts(opened, &facts) == VFS_ERR_NOT_FOUND);
    retirement_large_cleanup();
}

static void shared_and_layouts(void)
{
    for (unsigned layout = 0U; layout < 4U; layout++) {
        char name[256];
        if (layout < 2U) {
            setup();
            entry(0U, "ONE     TXT", 3U, 1U);
            if (layout) {
                /* Use the normal safe creator to produce same-sector LFNs. */
                cleanup(); setup();
                vfs_node_t *created = kmalloc(sizeof(*created)); assert(created);
                memset(created, 0, sizeof(*created));
                vfs_file_facts_t facts; u64 create_pin = 0U;
                assert(fat32_create_contents_locked(&root, created, "Same sector long.txt",
                    0U, NULL, NULL, &facts, &create_pin) == VFS_OK);
                created->super = &super; created->inode = facts.record_id;
                created->type = VFS_TYPE_FILE; created->ops = &fat32_node_ops;
                assert(vfs_recovery_pin_finish_handoff(create_pin) == VFS_OK);
                /* Import data mapping into the otherwise valid entry. */
                fat32_walk_entry_t match;
                assert(fat32_dir_find(&root, "Same sector long.txt", &match) == VFS_OK);
                fat32_on_disk_entry_t *disk = (void *)(media[10] + match.slot * 32U);
                disk->first_cluster_low = 3U; disk->file_size = 1U;
                fat32_state(created)->first_cluster = 3U; created->size = 1U;
                strcpy(name, "Same sector long.txt");
            } else strcpy(name, "ONE.TXT");
        } else retirement_fixture(name, layout == 3U, false);
        fat32_walk_entry_t match;
        assert(fat32_dir_find(&root, name, &match) == VFS_OK);
        u32 first = fat32_entry_first(&match.disk);
        /* Append a second name claiming the same allocation. */
        u32 index = match.slot + 1U;
        entry_at(match.cluster, index, "TWO     TXT", first, 1U, 0x20U);
        u8 *original = malloc(sizeof(media)); assert(original);
        memcpy(original, media, sizeof(media));
        u64 pin = 0U; unsigned before = writes;
        assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_BAD_FORMAT &&
               !pin && !active_pin && !fs.replace_transaction && !fs.version_confident &&
               writes == before && !memcmp(original, media, sizeof(media)));
        free(original); cleanup();
    }
    setup(); entry(0U, "ONE     TXT", 3U, 1U);
    assert(fat32_node_ops.unlink(&root, "ONE.TXT") == VFS_ERR_UNSUPPORTED &&
           fat32_node_ops.rmdir(&root, "ONE.TXT") == VFS_ERR_UNSUPPORTED &&
           !writes && !active_pin && fs.version_confident && ((u32 *)media[1])[3]);
    cleanup();
}

static void every_boundary(void)
{
    /* One-sector retirement, three fragmented clusters, two mirrored FATs,
     * and both FSInfo copies: every persistent write and flush is injected. */
    for (unsigned mode = 0U; mode < 5U; mode++) {
        for (unsigned boundary = 1U; boundary <= (mode == 3U ? 3U : 9U); boundary++) {
            setup(); entry(0U, "ONE     TXT", 3U, 1025U);
            ((u32 *)media[1])[3] = 7U; ((u32 *)media[1])[7] = 5U;
            ((u32 *)media[1])[5] = FAT32_EOC_MIN;
            fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
            fs.fs_info_sector = 3U; fs.backup_boot_sector = 1U;
            fs.reserved_sector_count = 6U;
            /* Synthetic FSInfo LBAs do not overlap the two FAT LBAs. */
            for (u32 lba = 3U; lba <= 4U; lba++) {
                fat32_store_le32(media[lba], 0x41615252U);
                fat32_store_le32(media[lba] + 484U, 0x61417272U);
                fat32_store_le32(media[lba] + 508U, 0xaa550000U);
            }
            vfs_node_t *file = fat32_finddir(&root, "ONE.TXT"); assert(file);
            if (mode == 0U) { fail_write_at = boundary; fail_write_definite = true; }
            if (mode == 1U) { fail_write_at = boundary; fail_write_partial_bytes = 17U; }
            if (mode == 2U) { fail_write_at = boundary; fail_write_partial_bytes = 512U; }
            if (mode == 3U) fail_flush_at = boundary;
            if (mode == 4U) remove_after_writes = boundary;
            u64 pin = 0U;
            assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_ERR_UNCERTAIN &&
                   !pin && active_pin && pin_unresolved && !fs.version_confident &&
                   fs.namespace_hidden);
            fat32_replace_transaction_t *tx = fs.replace_transaction;
            assert(tx && !tx->old_clusters && tx->old_count == 3U);
            if (mode != 3U && boundary == 1U)
                assert(((u32 *)media[1])[3] == 7U && ((u32 *)media[2])[3] == 7U);
            if (mode <= 2U && boundary >= 2U && boundary <= 7U) {
                assert(tx->reclaim_successor_valid && tx->metadata_write_pending &&
                       tx->metadata_copies_completed == (boundary - 2U) % 2U &&
                       tx->reclamation_cursor == (boundary - 2U) / 2U);
                assert(tx->metadata_cluster == tx->reclaim_cluster);
            }
            unsigned before = writes;
            assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_ERR_UNCERTAIN &&
                   !pin && writes == before &&
                   fat32_finddir(&root, "ONE.TXT") == NULL);
            assert(vfs_recovery_pin_finish_handoff(active_pin) == VFS_ERR_UNCERTAIN);
            cleanup(); assert(!live_requested);
        }
    }
}

static void preparation_failures(void)
{
    setup(); entry(0U, "ONE     TXT", 3U, 1U);
    u64 pin = 0U;
    assert(unlink_finished(&root, "ONE.TXT") == VFS_OK);
    unsigned total = allocations;
    cleanup();
    for (unsigned at = 1U; at <= total; at++) {
        setup(); entry(0U, "ONE     TXT", 3U, 1U); fail_allocation = at;
        assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_ERR_NO_MEM &&
               !pin && !active_pin && !writes && !fs.replace_transaction);
        cleanup(); assert(!live_requested);
    }
    setup(); entry(0U, "ONE     TXT", 3U, 1U); fail_pin_begin = true;
    assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_ERR_NO_SPACE &&
           !pin && !writes && !fs.replace_transaction && fs.version_confident);
    cleanup(); assert(!live_requested);
    setup(); entry(0U, "ONE     TXT", 3U, 1U);
    assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_OK && pin);
    fail_pin_finish = true;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY &&
           fs.replace_transaction && fs.namespace_hidden && !fs.version_confident);
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup(); assert(!live_requested);
}

static void chain_edges(void)
{
    for (u32 mode = 0U; mode < 6U; mode++) {
        setup(); entry(0U, "ONE     TXT", mode == 0U ? 0U : 3U, 0U);
        if (mode >= 1U && mode <= 4U) {
            fs.fat_count = mode;
            for (u32 copy = 1U; copy < mode; copy++) memcpy(media[1U + copy], media[1], 512U);
        }
        if (mode == 5U) {
            fs.fat_count = 2U; fs.bpb.ext_flags = 0x81U;
            memcpy(media[2], media[1], 512U);
            ((u32 *)media[1])[3] = 11U; /* Inactive copy is not authoritative. */
        }
        u64 pin = 0U;
        assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_OK && pin);
        assert(fs.replace_transaction->old_count == (mode == 0U ? 0U : 1U) &&
               !fs.replace_transaction->old_clusters);
        if (mode == 5U) assert(((u32 *)media[1])[3] == 11U && ((u32 *)media[2])[3] == 0U);
        else if (mode) for (u32 copy = 0U; copy < mode; copy++)
            assert(((u32 *)media[1U + copy])[3] == 0U);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        cleanup(); assert(!live_requested);
    }
    for (u32 mode = 0U; mode < 5U; mode++) {
        setup(); entry(0U, "ONE     TXT", 3U, mode == 3U ? 513U : 1U);
        if (mode == 0U) ((u32 *)media[1])[3] = 3U;
        if (mode == 1U) ((u32 *)media[1])[3] = fs.total_clusters;
        if (mode == 2U) ((u32 *)media[1])[3] = 0U;
        if (mode == 4U) {
            fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
            ((u32 *)media[2])[3] = 4U;
        }
        u64 pin = 0U;
        assert(fat32_unlink_pinned(&root, "ONE.TXT", &pin) == VFS_ERR_BAD_FORMAT &&
               !pin && !writes && !active_pin && !fs.replace_transaction &&
               !fs.version_confident);
        cleanup(); assert(!live_requested);
    }
}

static void large_failures(void)
{
    for (unsigned mode = 0U; mode < 4U; mode++) {
        retirement_large_setup(262144U, true, 2U);
        if (mode == 0U) fail_write_at = 262145U; /* Second copy, mid-chain. */
        if (mode == 1U) retire_read_failure_after = 262145U;
        if (mode == 2U) remove_after_writes = 262145U;
        if (mode == 3U) replace_retire_write = 262145U;
        u64 pin = 0U;
        assert(fat32_unlink_pinned(&root, "LARGE.BIN", &pin) == VFS_ERR_UNCERTAIN &&
               !pin && pin_unresolved && active_pin && fs.namespace_hidden);
        fat32_replace_transaction_t *tx = fs.replace_transaction;
        assert(tx && !tx->old_clusters && tx->reclamation_cursor ==
               (mode == 0U ? 131071U : 131072U));
        assert(tx->reclaim_cluster == (mode == 0U ? 196610U : 65539U));
        if (mode == 0U) assert(tx->reclaim_successor_valid &&
                              tx->reclaim_successor == 65539U &&
                              tx->metadata_copies_completed == 1U);
        retirement_large_cleanup();
    }
}

static void retirement_image(int argc, char **argv)
{
    setup();
    image_file = fopen(argv[2], "r+b");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512U == 0U);
    image_sectors = (u64)bytes / 512U; image_writable = true;
    device.sector_size = 512U; device.sector_count = image_sectors;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    for (int i = 3; i < argc; i++) {
        vfs_node_t *opened = fat32_finddir(mounted->root_node, argv[i]);
        assert(opened);
        if (!strcmp(argv[i], "FRAG.BIN")) {
            fat32_read_view_t view = {0};
            u32 current = fat32_state(opened)->first_cluster, fragments = 0U;
            for (;;) {
                u32 next;
                assert(fat32_get_next_checked_view(mounted, mounted->private_data,
                    current, &next, &view) == VFS_OK);
                if (next == FAT32_EOC_MIN) break;
                if (next != current + 1U) fragments++;
                current = next;
            }
            printf("Independent imported file has %u noncontiguous chain links\n", fragments);
            assert(fragments >= 16U);
        }
        unsigned before_reads = reads, before_writes = writes, before_flushes = flushes;
        usize baseline = live_requested; peak_requested = baseline;
        clock_t start = clock(); u64 pin = 0U;
        assert(fat32_unlink_pinned(mounted->root_node, argv[i], &pin) == VFS_OK && pin);
        fat32_replace_transaction_t *tx =
            ((fat32_fs_t *)mounted->private_data)->replace_transaction;
        assert(tx && !tx->old_clusters && tx->reclamation_cursor == tx->old_count);
        printf("FAT32 external deletion %s size=%llu: clusters=%u sectors=%u peak=%zu "
               "retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
               argv[i], (unsigned long long)opened->size, tx->old_count, tx->retirement->count,
               (size_t)(peak_requested - baseline), (size_t)(live_requested - baseline),
               reads - before_reads, writes - before_writes, flushes - before_flushes,
               (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
               !fat32_finddir(mounted->root_node, argv[i]) &&
               !fat32_state(opened)->alive &&
               fat32_ownership_preflight_locked(mounted->root_node) == VFS_OK);
    }
    vfs_node_t *nodes[32]; u32 count = 0U;
    for (fat32_node_state_t *state = ((fat32_fs_t *)mounted->private_data)->nodes;
         state; state = state->next) {
        assert(count < 32U); nodes[count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(mounted); fclose(image_file); image_file = NULL; image_writable = false;
    pin_super = &super; assert(!live_requested);
    puts("FAT32 independent-image file retirement passed");
}

int main(int argc, char **argv)

{
    if (argc >= 4 && !strcmp(argv[1], "--image")) {
        retirement_image(argc, argv); return 0;
    }
    assert(argc == 1);
    shared_and_layouts();
    every_boundary();
    preparation_failures();
    successful_large(262144U, false, 1U);
    successful_large(1048576U, true, 2U);
    chain_edges();
    large_failures();
    printf("FAT32 retirement inventory: tx=%zu images=%zu per_pin=%zu aggregate32=%zu requested bytes\n",
           sizeof(fat32_replace_transaction_t), sizeof(fat32_lfn_retirement_t),
           sizeof(fat32_replace_transaction_t) + sizeof(fat32_lfn_retirement_t),
           32U * (sizeof(fat32_replace_transaction_t) + sizeof(fat32_lfn_retirement_t)));
    puts("FAT32 unified recovery-aware file retirement passed");
    return 0;
}
