/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ordinary creation executes the shared production transaction, not a model. */
#define main fat32_identity_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

static char maximum_name[256];

static const char *creation_name(unsigned kind)
{
    if (kind >= 4U) return maximum_name;
    return kind >= 2U ?
        "A long Unicode directory or file \xc3\xa9 name spanning sectors.txt" :
        "NEW.TXT";
}

static unsigned sector_write_requests;
static bool count_sector_writes(block_device_t *dev, u64 id, u64 lba,
                                 u32 count, const void *buffer)
{
    sector_write_requests += count;
    scaling_write = NULL;
    bool result = block_write_for_id(dev, id, lba, count, buffer);
    scaling_write = count_sector_writes;
    return result;
}

static void creation_setup(unsigned kind)
{
    setup(); sector_write_requests = 0U; scaling_write = count_sector_writes;
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    if (kind >= 2U) {
        /* A full directory with a preexisting file and fragmented successor.
         * Maximum names span newly appended clusters; shorter names test the
         * existing one-sector growth path. */
        ((u32 *)media[1])[2] = 7U;
        ((u32 *)media[1])[7] = FAT32_EOC_MIN;
        memcpy(media[2], media[1], 512U);
        for (u32 c = 0U; c < 2U; c++)
            for (u32 i = 0U; i < 16U; i++) {
                char name[12];
                snprintf(name, sizeof(name), "F%02u     TXT", c * 16U + i);
                entry_at(c ? 7U : 2U, i, name, 0U, 0U, 0x20U);
            }
        entry_at(2U, 0U, "KEEP    TXT", 3U, 3U, 0x20U);
        memcpy(media[11], "old", 3U);
    }
    if (kind >= 6U) {
        memset(media[10] + 32U, 0, 480U);
        memset(media[15], 0, 512U);
    }
    /* Exercise both retained FSInfo images and shared metadata sectors. */
    fs.fs_info_sector = 3U;
    fs.backup_boot_sector = 1U;
    fs.reserved_sector_count = 6U;
    for (u32 i = 3U; i <= 4U; i++) {
        fat32_store_le32(media[i], 0x41615252U);
        fat32_store_le32(media[i] + 484U, 0x61417272U);
        fat32_store_le32(media[i] + 508U, 0xaa550000U);
    }
}

static void check_directory(vfs_node_t *dir, vfs_node_t *parent)
{
    u32 first = fat32_state(dir)->first_cluster;
    u32 parent_first = fat32_state(parent)->first_cluster;
    assert(first >= 2U && dir->type == VFS_TYPE_DIRECTORY && !dir->size);
    const fat32_on_disk_entry_t *dot = (void *)media[fat32_cluster_to_lba(&fs, first)];
    assert(!memcmp(dot[0].name, ".          ", 11U) &&
           !memcmp(dot[1].name, "..         ", 11U) &&
           dot[0].attr == 0x10U && dot[1].attr == 0x10U &&
           fat32_entry_first(&dot[0]) == first &&
           fat32_entry_first(&dot[1]) ==
             (parent_first == fs.root_cluster ? 0U : parent_first) &&
           !dot[0].file_size && !dot[1].file_size && !dot[2].name[0]);
}

static void successes(void)
{
    for (unsigned kind = 0U; kind < 8U; kind++) {
        creation_setup(kind);
        vfs_node_t *node = NULL;
        clock_t start = clock();
        u64 pin = 0U;
        node = kmalloc(sizeof(*node)); assert(node); memset(node, 0, sizeof(*node));
        int created = kind & 1U ? fat32_mkdir_pinned(&root, node, creation_name(kind), &pin) :
                                  fat32_create_pinned(&root, node, creation_name(kind), &pin);
        if (created != VFS_OK) fprintf(stderr, "creation layout %u failed: %d\n", kind, created);
        assert(created == VFS_OK);
        assert(pin && active_pin && !fs.version_confident);
        usize retained = live_requested - sizeof(*node) - sizeof(fat32_node_state_t);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        printf("FAT32 ordinary %s layout %u: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
               (kind & 1U) ? "mkdir" : "create", kind, (size_t)peak_requested,
               reads, writes, flushes, 1000.0 * (clock() - start) / CLOCKS_PER_SEC);
        printf("  retained transaction=%zu bytes, allocations=%u\n", (size_t)retained, allocations);
        assert(node == fat32_finddir(&root, creation_name(kind)) &&
               fat32_state(node)->generation == 1U &&
               fat32_ownership_preflight_locked(node) == VFS_OK &&
               fs.version_confident && !active_pin);
        if (kind & 1U) {
            check_directory(node, &root);
            vfs_node_t *child = NULL, *nested = NULL;
            assert(mkdir_finished(node, "Nested \xc3\xa9", &nested) == VFS_OK);
            check_directory(nested, node);
            assert(create_finished(nested, "CHILD.TXT", &child) == VFS_OK);
            assert(write_finished(child, 510U, 4U, "test") == 4U &&
                   child->size == 514U);
            assert(rmdir_finished(node, "Nested \xc3\xa9") == VFS_ERR_NOT_EMPTY);
            assert(unlink_finished(nested, "CHILD.TXT") == VFS_OK);
            assert(rmdir_finished(node, "Nested \xc3\xa9") == VFS_OK);
            assert(rmdir_finished(&root, creation_name(kind)) == VFS_OK);
            assert(!fat32_state(node)->alive);
            vfs_node_t *reused = NULL;
            assert(mkdir_finished(&root, creation_name(kind), &reused) == VFS_OK &&
                   reused != node && reused->inode != node->inode &&
                   !fat32_state(node)->alive);
            assert(rmdir_finished(&root, creation_name(kind)) == VFS_OK);
        } else {
            assert(!fat32_state(node)->first_cluster && !node->size);
            assert(write_finished(node, 7U, 6U, "growth") == 6U && node->size == 13U);
            char output[13]; assert(fat32_read(node, 0U, 13U, output) == 13U);
            assert(!memcmp(output, "\0\0\0\0\0\0\0growth", 13U));
            if (!kind) {
                assert(fat32_rename(&root, "NEW.TXT", &root, "MOVED.TXT") == VFS_OK);
                assert(fat32_finddir(&root, "MOVED.TXT") == node);
                assert(unlink_finished(&root, "MOVED.TXT") == VFS_OK);
            } else assert(unlink_finished(&root, creation_name(kind)) == VFS_OK);
            assert(!fat32_state(node)->alive && !write_finished(node, 0U, 1U, "x"));
        }
        if (kind >= 2U) {
            vfs_node_t *keep = fat32_finddir(&root, "KEEP.TXT");
            char old[3]; assert(keep && fat32_read(keep, 0U, 3U, old) == 3U &&
                               !memcmp(old, "old", 3U));
        }
        cleanup(); assert(!live_requested);
    }
    setup(); vfs_node_t *ignored = (void *)1;
    assert(fat32_node_ops.create(&root, "NEW.TXT", &ignored) == VFS_ERR_UNSUPPORTED &&
           !ignored && !writes);
    assert(fat32_node_ops.mkdir(&root, "NEW", &ignored) == VFS_ERR_UNSUPPORTED &&
           !ignored && !writes && !pin_begins);
    cleanup();
}

static void large_cluster_case(void)
{
    setup(); sector_write_requests = 0U; scaling_write = count_sector_writes;
    fs.sectors_per_cluster = 128U; fs.cluster_size_bytes = 65536U;
    fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
    fat32_on_disk_entry_t *entries = (void *)media[10];
    for (unsigned i = 0U; i < 2048U; i++) {
        char name[12]; snprintf(name, sizeof(name), "F%04u   TXT", i);
        memcpy(entries[i].name, name, 11U); entries[i].attr = 0x20U;
    }
    vfs_node_t *node = kmalloc(sizeof(*node)); assert(node); memset(node, 0, sizeof(*node));
    u64 pin = 0U;
    clock_t start = clock();
    assert(fat32_mkdir_pinned(&root, node, maximum_name, &pin) == VFS_OK && pin);
    printf("FAT32 64 KiB-cluster mkdir with growth: peak=%zu tx=%zu reads=%u write_requests=%u write_sectors=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)peak_requested,
           (size_t)(live_requested - sizeof(*node) - sizeof(fat32_node_state_t)),
           reads, writes, sector_write_requests, flushes,
           1000.0 * (clock() - start) / CLOCKS_PER_SEC);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fat32_ownership_preflight_locked(node) == VFS_OK &&
           fat32_finddir(&root, maximum_name) == node);
    assert(rmdir_finished(&root, maximum_name) == VFS_OK);
    cleanup(); assert(!live_requested);
}

static void preflight_failures(void)
{
    for (unsigned kind = 0U; kind < 8U; kind++) {
        creation_setup(kind);
        vfs_node_t *node;
        assert(create_node_finished(&root, creation_name(kind), &node,
                                    kind & 1U) == VFS_OK);
        unsigned count = allocations; cleanup(); assert(!live_requested);
        /* Every host allocation in admission must fail before any media write. */
        for (unsigned allocation = 1U; allocation <= count; allocation++) {
            creation_setup(kind); fail_allocation = allocation;
            assert(create_node_finished(&root, creation_name(kind), &node,
                                        kind & 1U) == VFS_ERR_NO_MEM && !node &&
                   !writes && !active_pin && fs.version_confident);
            cleanup(); assert(!live_requested);
        }
        creation_setup(kind); fail_pin_begin = true;
        assert(create_node_finished(&root, creation_name(kind), &node,
                                    kind & 1U) == VFS_ERR_NO_SPACE && !node &&
               !writes && !active_pin && fs.version_confident);
        cleanup(); assert(!live_requested);
    }
    setup(); vfs_node_t *unpublished = NULL;
    replace_after_reads = 1U;
    assert(mkdir_finished(&root, "NEW", &unpublished) == VFS_ERR_IO &&
           !unpublished && !writes && !active_pin && fs.version_confident);
    cleanup(); assert(!live_requested);
    setup(); entry(0U, "ONE     TXT", 3U, 1U); entry(1U, "TWO     TXT", 3U, 1U);
    vfs_node_t *node = NULL;
    assert(mkdir_finished(&root, "NEW", &node) == VFS_ERR_BAD_FORMAT && !writes && !node);
    cleanup(); assert(!live_requested);
    setup(); entry(0U, "NEW     TXT", 0U, 0U);
    assert(create_finished(&root, "new.txt", &node) == VFS_ERR_ALREADY_EXISTS &&
           !node && !writes);
    assert(mkdir_finished(&root, "NEW.TXT", &node) == VFS_ERR_ALREADY_EXISTS &&
           !node && !writes);
    assert(mkdir_finished(&root, "\xed\xa0\x80", &node) == VFS_ERR_INVALID_PARAM && !writes);
    cleanup(); assert(!live_requested);
}

static unsigned replace_at_write;
static bool replacement_write(block_device_t *dev, u64 id, u64 lba,
                               u32 count, const void *buffer)
{
    scaling_write = NULL;
    bool result = block_write_for_id(dev, id, lba, count, buffer);
    scaling_write = replacement_write;
    if (writes == replace_at_write) device.id++;
    return result;
}

static void uncertain(void)
{
    unsigned boundaries = 0U;
    for (unsigned kind = 0U; kind < 8U; kind++) {
        creation_setup(kind); vfs_node_t *node = NULL;
        assert(create_node_finished(&root, creation_name(kind), &node,
                                    kind & 1U) == VFS_OK);
        unsigned wc = writes, fc = flushes; cleanup();
        for (unsigned w = 1U; w <= wc; w++) {
            for (unsigned fault = 0U; fault < 5U; fault++) {
                creation_setup(kind);
                if (fault == 4U) {
                    replace_at_write = w; scaling_write = replacement_write;
                } else if (fault == 3U) remove_after_writes = w;
                else {
                    fail_write_at = w;
                    fail_write_definite = fault == 0U;
                    fail_write_partial_bytes = fault == 1U ? 17U : 512U;
                }
                int result = create_node_finished(&root, creation_name(kind), &node,
                                                   kind & 1U);
                assert(result == VFS_ERR_UNCERTAIN && !node && active_pin &&
                       pin_unresolved && !fs.version_confident && fs.replace_transaction &&
                       fs.replace_transaction->pin_id == active_pin);
                unsigned stopped = writes;
                assert(create_finished(&root, "RETRY.TXT", &node) == VFS_ERR_UNCERTAIN &&
                       !node && writes == stopped && !fat32_finddir(&root, creation_name(kind)));
                if (fs.replace_transaction->new_first)
                    assert(((u32 *)media[1])[fs.replace_transaction->new_first] != 0U);
                boundaries++;
                cleanup(); assert(!live_requested);
            }
        }
        for (unsigned f = 1U; f <= fc; f++) {
            creation_setup(kind); fail_flush_at = f;
            assert(create_node_finished(&root, creation_name(kind), &node,
                                        kind & 1U) == VFS_ERR_UNCERTAIN &&
                   !node && active_pin && pin_unresolved && !fs.version_confident);
            boundaries++; cleanup(); assert(!live_requested);
        }
    }
    printf("FAT32 ordinary create/mkdir fault cases: %u write/removal/re-registration/flush boundaries\n", boundaries);
    /* Confirmed cancellation before publication, including cleanup failures. */
    for (unsigned fault = 0U; fault < 3U; fault++) {
        setup(); fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
        for (u32 i = 0U; i < 16U; i++) entry(i, "FULL    TXT", 0U, 0U);
        for (u32 i = 5U; i < fs.total_clusters; i++) {
            ((u32 *)media[1])[i] = ((u32 *)media[2])[i] = FAT32_EOC_MIN;
        }
        if (fault == 1U) fail_write_at = 4U; /* first cancellation FAT copy */
        if (fault == 2U) fail_flush_at = 2U; /* cancellation flush after staging */
        vfs_node_t *node = NULL;
        int result = mkdir_finished(&root, "NEW", &node);
        assert(!node && media[10][0] == 'F');
        if (!fault) {
            assert(result == VFS_ERR_NO_SPACE && !active_pin && fs.version_confident &&
                   !((u32 *)media[1])[4] && !((u32 *)media[2])[4]);
            ((u32 *)media[1])[5] = ((u32 *)media[2])[5] = 0U;
            assert(mkdir_finished(&root, "NEW", &node) == VFS_OK && node);
        } else assert(result == VFS_ERR_UNCERTAIN && active_pin && pin_unresolved);
        cleanup(); assert(!live_requested);
    }
}

static void image_case(const char *path)
{
    setup(); image_file = fopen(path, "r+b"); assert(image_file);
    assert(!fseeko(image_file, 0, SEEK_END));
    image_sectors = (u64)ftello(image_file) / 512U;
    image_writable = true; device.sector_size = 512U; device.sector_count = image_sectors;
    vfs_super_t *sb = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &sb, false) == VFS_OK && sb);
    pin_super = sb;
    u32 before_reads = reads, before_writes = writes, before_flushes = flushes;
    usize baseline = live_requested; peak_requested = baseline;
    clock_t start = clock();
    vfs_node_t *folder = NULL, *file = NULL, *empty = NULL;
    char name[256];
    memset(name, 'd', 250U); memcpy(name + 250U, "\xc3\xa9" "dir", 6U);
    folder = kmalloc(sizeof(*folder)); assert(folder); memset(folder, 0, sizeof(*folder));
    u64 pin = 0U;
    assert(fat32_mkdir_pinned(sb->root_node, folder, name, &pin) == VFS_OK && pin);
    fat32_replace_transaction_t *creation =
        ((fat32_fs_t *)sb->private_data)->replace_transaction;
    assert(creation && creation->lfn_creation && creation->lfn_creation->count >= 2U &&
           creation->growth && creation->growth->append);
    printf("Independent image mkdir: LFN sectors=%u appended parent clusters=%u\n",
           creation->lfn_creation->count, creation->growth->append_count);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(create_finished(folder, "ordinary large file.txt", &file) == VFS_OK && file);
    u8 *data = malloc(3U * 1024U * 1024U + 17U); assert(data);
    for (u32 i = 0U; i < 3U * 1024U * 1024U + 17U; i++) data[i] = (u8)(i * 13U);
    assert(write_finished(file, 0U, 3U * 1024U * 1024U + 17U, data) == 3U * 1024U * 1024U + 17U);
    u8 check[8192];
    for (u64 at = 0U; at < file->size;) {
        u64 n = file->size - at > sizeof(check) ? sizeof(check) : file->size - at;
        assert(fat32_read(file, at, n, check) == n && !memcmp(check, data + at, n));
        at += n;
    }
    free(data);
    assert(mkdir_finished(folder, "Empty long directory name", &empty) == VFS_OK && empty);
    assert(rmdir_finished(folder, "Empty long directory name") == VFS_OK);
    vfs_node_t *short_file = NULL;
    assert(create_finished(folder, "SHORT.TXT", &short_file) == VFS_OK);
    assert(write_finished(short_file, 0U, 3U, "abc") == 3U);
    assert(fat32_rename(folder, "SHORT.TXT", folder, "RENAMED.TXT") == VFS_OK);
    assert(unlink_finished(folder, "RENAMED.TXT") == VFS_OK);
    assert(fat32_ownership_preflight_locked(folder) == VFS_OK);
    printf("FAT32 independently formatted create/mkdir lifecycle: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline), reads - before_reads,
           writes - before_writes, flushes - before_flushes,
           1000.0 * (clock() - start) / CLOCKS_PER_SEC);
    fat32_fs_t *mounted = sb->private_data;
    vfs_node_t *nodes[256]; unsigned count = 0U;
    for (fat32_node_state_t *s = mounted->nodes; s; s = s->next) {
        assert(count < 256U); nodes[count++] = s->node;
    }
    assert(fat32_unmount(sb) == VFS_OK);
    for (unsigned i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(sb); fclose(image_file); image_file = NULL;
    assert(!live_requested);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--image")) { image_case(argv[2]); return 0; }
    memset(maximum_name, 'a', 251U); memcpy(maximum_name + 251U, ".txt", 5U);
    successes(); large_cluster_case(); preflight_failures(); uncertain();
    printf("FAT32 ordinary create/mkdir production regressions passed: common=%zu node=%zu identity=%zu\n",
           sizeof(fat32_replace_transaction_t), sizeof(vfs_node_t), sizeof(fat32_node_state_t));
    return 0;
}
