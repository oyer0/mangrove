/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production exFAT and extracted VFS dispatch; controlled media/heap/pins. */
/* The runner omits the names fixture's entry point in its temporary copy. */
#include "exfat_names_test.c"
#include "delete_vfs.inc"

static u8 unchanged_media[sizeof(media)];

static void imported_delete_set(u64 offset, const char *name, bool directory,
                                u32 cluster, bool allocated)
{
    u16 units[255], length = 0;
    /* Fixture input is deliberately decoded independently of production. */
    for (const unsigned char *p = (const unsigned char *)name; *p;) {
        assert(length < 255U);
        if (*p < 0x80U) units[length++] = *p++;
        else {
            assert(p[0] == 0xc3U && (p[1] & 0xc0U) == 0x80U);
            units[length++] = (u16)((p[0] & 0x1fU) << 6U | (p[1] & 0x3fU));
            p += 2;
        }
    }
    u8 set[640];
    u32 count = imported_set(units, length, directory ? 0x10U : 0U,
        allocated ? cluster : 0U, allocated ? (directory ? 512U : 1U) : 0U, set);
    for (u32 at = 8; at <= 16; at += 4) exfat_put32(set + at, 0x28210000U);
    memset(set + 22, 0x80, 3); /* 2000-01-01 midnight; valid UTC offsets. */
    u16 sum = 0;
    for (u32 i = 0; i < count * 32U; i++)
        if (i != 2U && i != 3U) sum = fixture_rotate(sum) + set[i];
    exfat_put16(set + 2, sum);
    write_root_fixture(offset, set, count * 32U);
    if (allocated) {
        mark_used(cluster);
        if (!directory) media[118U + cluster][0] = 'D';
    }
}

static void prepare_names(bool directory, const char *first, const char *second,
                          u64 second_offset, bool allocated)
{
    imported_setup();
    if (second_offset + 640U > root_state.data_length) {
        ((u32 *)media[24])[22] = 23;
        ((u32 *)media[24])[23] = EXFAT_CLUSTER_EOC;
        mark_used(23);
        root.size = root_state.data_length = root_state.valid_data_length = 2560;
    }
    /* No end marker may hide the second imported member. */
    for (u64 at = 480U; at < second_offset; at += 32U) {
        u8 deleted[32] = {0x05}; write_root_fixture(at, deleted, sizeof(deleted));
    }
    imported_delete_set(480U, first, directory, 30U, allocated);
    if (second) imported_delete_set(second_offset, second, directory, 32U, allocated);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    super.reference_count = 1;
    reads = writes = flushes = allocations = 0;
    memcpy(unchanged_media, media, sizeof(media));
}

static int remove_name(vfs_node_t *parent, const char *name, bool directory,
                        unsigned route)
{
    if (route == 1) return directory ? vfs_rmdir(parent, name) : vfs_unlink(parent, name);
    if (route == 2) return directory ? vfs_rmdir_trusted(parent, name) : vfs_unlink_trusted(parent, name);
    u64 pin = 99;
    int result = directory ? parent->ops->rmdir_pinned(parent, name, &pin) :
                             parent->ops->unlink_pinned(parent, name, &pin);
    if (pin) finish_pin();
    else assert(!pin_active);
    return result;
}

static void assert_rejected(int result, int expected)
{
    if (result != expected || writes || flushes || !fs.version_confident)
        fprintf(stderr, "rejection result=%d expected=%d reads=%u writes=%u flushes=%u confident=%u\n",
            result, expected, reads, writes, flushes, fs.version_confident);
    assert(result == expected && !writes && !flushes && !pin_active &&
        !pin_unresolved && !fs.replace_transaction && fs.version_confident &&
        root_state.generation == 1 && super.reference_count == 1 && !operation_held);
    assert(!memcmp(media, unchanged_media, sizeof(media)));
}

static void duplicate_tests(void)
{
    unsigned cases = 0;
    for (unsigned directory = 0; directory < 2; directory++)
        for (unsigned route = 0; route < 3; route++)
            for (unsigned layout = 0; layout < 3; layout++)
                for (unsigned allocated = 0; allocated < 2; allocated++) {
                    const char *a = layout == 2 ? "caf\xc3\xa9.txt" : "same.txt";
                    const char *b = layout == 2 ? "CAF\xc3\x89.TXT" : "SAME.TXT";
                    u64 at = layout ? 1504U : 576U;
                    prepare_names(directory, a, b, at, allocated);
                    for (unsigned repeat = 0; repeat < 2; repeat++)
                        assert_rejected(remove_name(&root, b, directory, route), VFS_ERR_BAD_FORMAT);
                    if (!route) assert(!allocations); /* No transaction/proof heap. */
                    assert(exfat_finddir(&root, a) && exfat_finddir(&root, b));
                    cleanup(); cases++;
                }
    /* Long imported names whose sets span fragmented directory clusters. */
    char a[256], b[256]; memset(a, 'a', 255); memset(b, 'A', 255); a[255]=b[255]=0;
    prepare_names(false, a, b, 1504U, true);
    assert_rejected(remove_name(&root, a, false, 0), VFS_ERR_BAD_FORMAT);
    cleanup();
    /* Type does not disambiguate two equivalent pathname identities. */
    prepare_names(false, "same.txt", "SAME.TXT", 1504U, false);
    imported_delete_set(1504U, "SAME.TXT", true, 32, false);
    memcpy(unchanged_media, media, sizeof(media));
    assert_rejected(remove_name(&root, "same.txt", false, 0), VFS_ERR_BAD_FORMAT);
    assert_rejected(remove_name(&root, "same.txt", true, 0), VFS_ERR_BAD_FORMAT);
    cleanup();
    printf("Unique deletion: %u duplicate route/layout/allocation cases plus spanning long names; zero media writes\n", cases);
}

static void unique_tests(void)
{
    for (unsigned directory = 0; directory < 2; directory++)
        for (unsigned route = 0; route < 3; route++) {
            prepare_names(directory, "caf\xc3\xa9.txt", "unrelated", 1504U, true);
            usize before = live_requested; peak_requested = before;
            assert(remove_name(&root, "CAF\xc3\x89.TXT", directory, route) == VFS_OK);
            unsigned operation_reads = reads;
            unsigned operation_allocations = allocations;
            usize operation_peak = peak_requested - before;
            assert(!pin_active && fs.version_confident && !fs.replace_transaction &&
                !exfat_finddir(&root, "caf\xc3\xa9.txt") && exfat_finddir(&root, "unrelated"));
            assert(!(media[121][(30U-2U)/8U] & (1U << ((30U-2U)%8U))));
            assert(media[121][(32U-2U)/8U] & (1U << ((32U-2U)%8U)));
            printf("Unique %s route=%u: reads=%u writes=%u flushes=%u allocations=%u peak=%zu tx=%zu\n",
                directory ? "rmdir" : "unlink", route, operation_reads, writes, flushes, operation_allocations,
                (size_t)operation_peak, sizeof(exfat_replace_transaction_t));
            cleanup();
        }
    for (unsigned directory = 0; directory < 2; directory++)
        for (unsigned route = 0; route < 3; route++) {
            prepare_names(directory, "same.txt", "SAME.TXT", 1504U, true);
            assert_rejected(remove_name(&root, "missing", directory, route), VFS_ERR_NOT_FOUND);
            cleanup();
        }
    /* Non-root NoFatChain directory: same comparison/proof path. */
    prepare_names(true, "parent", NULL, 576U, true);
    u8 sets[640]; const u16 name[] = {'s','a','m','e'}, upper[] = {'S','A','M','E'};
    u32 count = imported_set(name, 4, 0, 0, 0, sets); memcpy(media[148], sets, count*32U);
    count = imported_set(upper, 4, 0, 0, 0, sets); memcpy(media[148]+96, sets, count*32U);
    vfs_node_t *parent = exfat_finddir(&root, "parent"); assert(parent);
    reads = writes = flushes = 0; memcpy(unchanged_media, media, sizeof(media));
    assert_rejected(remove_name(parent, "same", false, 1), VFS_ERR_BAD_FORMAT);
    cleanup();
}

static void incomplete_tests(void)
{
    /* Fault every read needed to prove absence of a second matching name. */
    prepare_names(false, "same.txt", "unrelated", 1504U, true);
    exfat_entry_info_t item; bool found;
    assert(exfat_find_entry_locked_view(&fs, 2, false, 2048, "same.txt", &item, &found) == VFS_OK && found);
    unsigned count = reads; cleanup();
    for (unsigned directory = 0; directory < 2; directory++)
        for (unsigned fault = 1; fault <= count; fault++) {
            prepare_names(directory, "same.txt", "unrelated", 1504U, true);
            fail_read_number = fault;
            assert_rejected(remove_name(&root, "same.txt", directory, 0), VFS_ERR_IO);
            cleanup();
        }
    for (unsigned kind = 0; kind < 3; kind++) {
        prepare_names(false, "same.txt", "unrelated", 1504U, true);
        if (kind == 0) media[139][482] ^= 1; /* Later primary checksum. */
        if (kind == 1) media[139][480] = 0xc1; /* Live orphan secondary. */
        if (kind == 2) ((u32 *)media[24])[20] = fs.boot.cluster_count + 2U;
        memcpy(unchanged_media, media, sizeof(media));
        assert_rejected(remove_name(&root, "same.txt", false, 0), VFS_ERR_IO);
        cleanup();
    }
    prepare_names(false, "same.txt", "unrelated", 1504U, true);
    fs.boot.cluster_count = EXFAT_SCAN_MAX_BITMAP_BYTES * 8U + 1U;
    assert_rejected(remove_name(&root, "same.txt", false, 0), VFS_ERR_BUFFER_TOO_SMALL);
    cleanup();
    printf("Incomplete deletion identity: %u lookup read failures, malformed trailing sets/chains and proof budget rejected\n", count);
}

static void baseline_tests(void)
{
    for (unsigned directory = 0; directory < 2; directory++) {
        prepare_names(directory, "caf\xc3\xa9.txt", "unrelated", 1504U, true);
        usize before = live_requested; peak_requested = before;
        assert(remove_name(&root, "CAF\xc3\x89.TXT", directory, 0) == VFS_OK);
        printf("Baseline unique %s: reads=%u writes=%u flushes=%u allocations=%u peak=%zu tx=%zu\n",
            directory ? "rmdir" : "unlink", reads, writes, flushes, allocations,
            (size_t)(peak_requested-before), sizeof(exfat_replace_transaction_t));
        cleanup();
        prepare_names(directory, "same.txt", "SAME.TXT", 1504U, true);
        assert(remove_name(&root, "same.txt", directory, 0) == VFS_OK && writes &&
            fs.version_confident && exfat_finddir(&root, "same.txt"));
        printf("Baseline %s: success with pathname still resolving, writes=%u\n", directory ? "rmdir" : "unlink", writes);
        cleanup();
    }
}

static void external_image(const char *path, const char *name, bool directory, bool duplicate)
{
    setup(); device.sector_count = 12000;
    FILE *image = fopen(path, "rb"); assert(image && fread(media, sizeof(media), 1, image) == 1 && !fclose(image));
    exfat_fs_t *mounted = exfat_new_state(&device); assert(mounted);
    fs = *mounted; kfree(mounted); fs.next_inode = 1; fs.nodes = &root_state;
    root_state.first_cluster = fs.root_cluster;
    root.size = root_state.data_length = root_state.valid_data_length = fs.cluster_bytes;
    super.reference_count = 1;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    reads = writes = flushes = 0; memcpy(unchanged_media, media, sizeof(media));
    int result = remove_name(&root, name, directory, 1);
#ifdef EXFAT_DELETE_BASELINE
    assert(duplicate && result == VFS_OK && writes && exfat_finddir(&root, name));
#else
    if (duplicate) assert_rejected(result, VFS_ERR_BAD_FORMAT);
    else assert(result == VFS_OK && !exfat_finddir(&root, name) && fs.version_confident);
#endif
    image = fopen(path, "wb"); assert(image && fwrite(media, sizeof(media), 1, image) == 1 && !fclose(image));
    printf("Disposable imported %s duplicate=%u: result=%d reads=%u writes=%u flushes=%u\n",
        directory ? "directory" : "file", duplicate, result, reads, writes, flushes);
    kfree(fs.upcase); fs.upcase = NULL; cleanup();
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "--image")) {
        external_image(argv[2], argv[3], atoi(argv[4]), atoi(argv[5])); return 0;
    }
#ifdef EXFAT_DELETE_BASELINE
    baseline_tests();
#else
    duplicate_tests(); unique_tests(); incomplete_tests();
    printf("Read-only identity views: FAT=%zu stream=%zu bytes, no retained or heap state\n",
        sizeof(exfat_read_view_t), sizeof(exfat_stream_view_t));
#endif
    puts("exFAT deletion identity regression completed");
    return 0;
}
