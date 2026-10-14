/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production exFAT write/read/recovery; controlled media, heap, pins and clock. */
#define main exfat_extension_identity_main
#include "exfat_mount_identity_test.c"
#undef main

static u8 expected[3U * 4096U], actual[3U * 4096U];
static void fixture_timestamp(u8 *set, u8 count)
{
    for (u32 at = 8U; at <= 16U; at += 4U) exfat_put32(set + at, 0x28210000U);
    set[22] = set[23] = set[24] = 0x80U; /* 2000-01-01 midnight UTC. */
    exfat_put16(set + 2U, exfat_entry_checksum(set, count));
}
static vfs_node_t *prepare_extension(u64 length, u64 valid, bool contiguous, bool blocker)
{
    setup(); add_system_entries();
    device.sector_count = fs.boot.volume_length = 12000U;
    fs.boot.cluster_count = 1400U; fs.boot.fat_length = 11U; fs.boot.heap_offset = 120U;
    fs.sectors_per_cluster = 8U; fs.cluster_bytes = 4096U;
    root.size = root_state.data_length = root_state.valid_data_length = 4096U;
    memcpy(media[120], media[40], 512U);
    memset(media[128], 0, 4096U); media[128][0] = 7U;
    u32 count = (u32)(length / 4096U + (length % 4096U != 0));
    for (u32 i = 0; i < count; i++) {
        u32 cluster = 10U + i * (contiguous ? 1U : 2U);
        assert(cluster < fs.boot.cluster_count + 2U);
        media[128][(cluster - 2U) / 8U] |= (u8)(1U << ((cluster - 2U) % 8U));
        exfat_put32((u8 *)media[24] + cluster * 4U,
            i + 1U < count ? cluster + (contiguous ? 1U : 2U) : EXFAT_CLUSTER_EOC);
        memset(media[exfat_cluster_lba(&fs, cluster)], 0xa5, 4096U);
        for (u32 at = 0; at < 4096U && (u64)i * 4096U + at < valid; at++)
            ((u8 *)media[exfat_cluster_lba(&fs, cluster)])[at] =
                (u8)('A' + ((u64)i * 4096U + at) % 23U);
    }
    u8 set[640], entries;
    assert(exfat_build_file_set(&fs, "old.txt", 0, count ? 10U : 0U,
        valid, length, count && contiguous, set, &entries));
    fixture_timestamp(set, entries);
    memcpy(media[120], set, entries * 32U);
    if (blocker) {
        assert(count == 1U);
        media[128][1] |= 2U; /* Cluster 11 blocks adjacent growth. */
        exfat_put32((u8 *)media[24] + 11U * 4U, EXFAT_CLUSTER_EOC);
        memset(media[exfat_cluster_lba(&fs, 11U)], 'B', 4096U);
        assert(exfat_build_file_set(&fs, "blocker", 0, 11U, 1U, 1U, true, set, &entries));
        fixture_timestamp(set, entries);
        memcpy(media[120] + 160U, set, entries * 32U);
    }
    vfs_node_t *file = exfat_finddir(&root, "old.txt"); assert(file);
    writes = reads = flushes = bitmap_writes = allocations = 0;
    peak_requested = live_requested;
    return file;
}
static int finish_extension(int result, u64 pin)
{
    if (pin) {
        if (!pin_handoff(&super, pin_context)) {
            assert(vfs_recovery_pin_mark_unresolved(pin) == VFS_OK);
            return VFS_ERR_UNCERTAIN;
        }
        pin_active = false; pin_handoff = NULL; pin_context = NULL;
    }
    return result;
}
static void release_extension(void)
{
    if (pin_unresolved) discard_unresolved_fixture_pin();
    cleanup();
}
static void reference(u64 length, u64 valid, u64 offset, u32 size)
{
    assert(length <= sizeof(expected) && offset + size <= sizeof(expected));
    memset(expected, 0, sizeof(expected));
    for (u64 i = 0; i < valid; i++) expected[i] = (u8)('A' + i % 23U);
    for (u32 i = 0; i < size; i++) expected[offset + i] = (u8)(0x30U + i % 10U);
}
static void success_case(u64 length, u64 valid, u64 offset, u32 size,
                         bool contiguous, bool blocker)
{
    vfs_node_t *file = prepare_extension(length, valid, contiguous, blocker);
    reference(length, valid, offset, size);
    u64 bytes = 0, pin = 0, final = length > offset + size ? length : offset + size;
    int result = exfat_node_write_pinned(file, offset, size, expected + offset, &bytes, &pin);
    assert(result == VFS_OK && bytes == size && pin && !pin_unresolved);
    assert(finish_extension(result, pin) == VFS_OK && fs.version_confident && !fs.replace_transaction);
    unsigned operation_reads = reads, operation_writes = writes;
    assert(exfat_node_read(file, 0, final, actual) == final);
#ifdef EXFAT_EXTENSION_BASELINE
    unsigned wrong = 0;
    for (u64 i = valid; i < offset; i++) wrong += actual[i] != 0;
    assert(wrong == (offset < 4096U ? offset : 4096U) - valid);
    printf("baseline extension offset=%llu: %u poisoned gap bytes exposed; reads=%u writes=%u flushes=%u allocations=%u peak_requested=%zu tx=%zu after successful handoff\n",
        (unsigned long long)offset, wrong, operation_reads, operation_writes,
        flushes, allocations, (size_t)peak_requested, sizeof(exfat_replace_transaction_t));
#else
    assert(!memcmp(expected, actual, final));
    exfat_node_t *state = file->fs_data;
    assert(state->valid_data_length == (offset + size > valid ? offset + size : valid));
    if (blocker) {
        assert(!state->no_fat_chain);
        assert(media[exfat_cluster_lba(&fs, 11U)][0] == 'B');
    }
    if (length == 4U && valid == 4U && size == 1U && contiguous && !blocker)
        printf("extension offset=%llu: driver sectors read=%u written=%u flushes=%u allocations=%u peak_requested=%zu tx=%zu\n",
            (unsigned long long)offset, operation_reads, operation_writes, flushes,
            allocations, (size_t)peak_requested, sizeof(exfat_replace_transaction_t));
#endif
    release_extension();
}
#ifndef EXFAT_EXTENSION_BASELINE
static void failure_invariants(vfs_node_t *file, int result, u64 bytes)
{
    assert(result != VFS_OK && !bytes);
    if (writes) {
        assert(result == VFS_ERR_UNCERTAIN && pin_active && pin_unresolved &&
            !fs.version_confident && fs.replace_transaction);
        u64 retry_bytes = 123U, retry_pin = 123U; unsigned before = writes;
        assert(exfat_node_write_pinned(file, 16U, 1U, "X", &retry_bytes, &retry_pin) ==
            VFS_ERR_UNCERTAIN && !retry_bytes && !retry_pin && writes == before);
        exfat_replace_transaction_t *tx = fs.replace_transaction;
        if (tx->data_attempted) {
            assert(tx->mutation_attempted && tx->data_sector < device.sector_count);
            usize copied = fail_write_copy_bytes;
            assert(!memcmp(media[tx->data_sector], tx->proposed_data_sector, copied));
            u64 old_lba = exfat_cluster_lba(&fs, 10U);
            if (tx->data_sector >= old_lba && tx->data_sector - old_lba < 8U)
                assert(!memcmp(media[tx->data_sector] + copied,
                    tx->old_data_sector + copied, 512U - copied));
        }
    } else assert(!pin_active && fs.version_confident && !fs.replace_transaction);
    /* Diagnostic decoding only: the quarantined public reader stays gated.
     * A coherent persisted stream may advance VDL only over initialized bytes. */
    fail_read_number = 0U; fail_read_lba = ~(u64)0;
    exfat_node_t *state = file->fs_data;
    exfat_entry_info_t entry; bool io_error;
    if (exfat_entry_set_read_checked(&fs, state->parent_cluster,
        state->parent_no_fat_chain, state->entry_offset, &entry, &io_error)) {
        assert(entry.valid_data_length <= 4197U);
        exfat_node_t view = *state;
        view.first_cluster = entry.first_cluster;
        view.no_fat_chain = !!(entry.stream_flags & EXFAT_STREAM_NO_FAT_CHAIN);
        view.data_length = entry.data_length;
        view.valid_data_length = entry.valid_data_length;
        assert(exfat_file_read_bytes(&fs, &view, 0U, entry.valid_data_length, actual));
        assert(!memcmp(expected, actual, entry.valid_data_length));
    }
}
static void split_extension_file(vfs_node_t *file)
{
    u8 entry[96]; memcpy(entry, media[120], sizeof(entry));
    memset(media[120], 0, 96U);
    for (u32 at = 0; at < 480U; at += 32U)
        if (at < 96U || at >= 160U) media[120][at] = EXFAT_ENTRY_DELETED;
    memcpy(media[120] + 480U, entry, 32U); memcpy(media[121], entry + 32U, 64U);
    ((exfat_node_t *)file->fs_data)->entry_offset = 480U;
}
static void faults(void)
{
    unsigned checked = 0U;
    for (u32 layout = 0U; layout < 4U; layout++) {
        bool contiguous = layout != 1U, blocker = layout == 2U;
        vfs_node_t *file = prepare_extension(4U, 4U, contiguous, blocker);
        if (layout == 3U) split_extension_file(file);
        reference(4U, 4U, 4196U, 1U);
        u64 bytes, pin;
        int result = exfat_node_write_pinned(file, 4196U, 1U, expected + 4196U, &bytes, &pin);
        assert(finish_extension(result, pin) == VFS_OK);
        unsigned totals[] = {reads, writes, flushes, allocations};
        release_extension();
        for (u32 kind = 0U; kind < 4U; kind++) for (unsigned n = 1U; n <= totals[kind]; n++) {
            unsigned variants = kind == 1U ? 3U : 1U;
            for (unsigned torn = 0U; torn < variants; torn++) {
                file = prepare_extension(4U, 4U, contiguous, blocker);
                if (layout == 3U) split_extension_file(file);
                reference(4U, 4U, 4196U, 1U);
                if (kind == 0U) fail_read_number = n;
                if (kind == 1U) { fail_write_number = n; fail_write_copy_bytes = torn == 0U ? 0U : torn == 1U ? 128U : 512U; }
                if (kind == 2U) fail_flush_number = n;
                if (kind == 3U) fail_allocation = n;
                result = exfat_node_write_pinned(file, 4196U, 1U, expected + 4196U, &bytes, &pin);
                if (result != VFS_OK) assert(!bytes);
                result = finish_extension(result, pin);
                if (result != VFS_OK) bytes = 0U; /* Same final-error rule as VFS. */
                failure_invariants(file, result, bytes);
                release_extension(); checked++;
            }
        }
    }
    printf("exFAT extension fault boundaries: %u read/write/flush/allocation cases, including torn and completed-failure writes\n", checked);
}
static void repeated_and_bounds(void)
{
    vfs_node_t *file = prepare_extension(4U, 4U, true, false);
    reference(4U, 4U, 16U, 1U);
    const u64 offsets[] = {16U, 512U, 4095U, 4196U, 8191U};
    for (u32 i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        expected[offsets[i]] = (u8)('0' + i); u64 bytes, pin;
        assert(exfat_node_write_pinned(file, offsets[i], 1U, expected + offsets[i], &bytes, &pin) == VFS_OK && bytes == 1U);
        assert(finish_extension(VFS_OK, pin) == VFS_OK);
        assert(exfat_node_read(file, 0U, offsets[i] + 1U, actual) == offsets[i] + 1U &&
            !memcmp(expected, actual, offsets[i] + 1U));
    }
    unsigned before = writes; u64 bytes, pin;
    assert(exfat_node_write_pinned(file, ~(u64)0, 1U, "X", &bytes, &pin) == VFS_ERR_INVALID_PARAM);
    assert(exfat_node_write_pinned(file, (u64)EXFAT_SCAN_MAX_CLAIMS * 4096U, 1U, "X", &bytes, &pin) == VFS_ERR_NO_SPACE);
    assert(exfat_node_write_pinned(file, 0U, ~(u64)0, "X", &bytes, &pin) == VFS_ERR_NO_SPACE);
    assert(exfat_node_write_pinned(file, ~(u64)0, 0U, NULL, &bytes, &pin) == VFS_OK && !bytes && !pin);
    assert(writes == before && fs.version_confident);
    /* Truncation resets both lengths; subsequent sparse growth must zero all. */
    assert(exfat_truncate_pinned(file, &pin) == VFS_OK && finish_extension(VFS_OK, pin) == VFS_OK);
    memset(expected, 0, sizeof(expected)); expected[4196U] = 'T';
    assert(exfat_node_write_pinned(file, 4196U, 1U, "T", &bytes, &pin) == VFS_OK && finish_extension(VFS_OK, pin) == VFS_OK);
    assert(exfat_node_read(file, 0U, 4197U, actual) == 4197U && !memcmp(actual, expected, 4197U));
    /* Replacement stages every byte in fresh storage, including its slack. */
    exfat_node_t *state = file->fs_data;
    assert(exfat_replace_locked(&super, file->inode, state->generation, 3U,
        replace_source, "XYZ", &pin) == VFS_OK && finish_extension(VFS_OK, pin) == VFS_OK);
    memset(expected, 0, sizeof(expected)); memcpy(expected, "XYZ", 3U); expected[16U] = 'R';
    assert(exfat_node_write_pinned(file, 16U, 1U, "R", &bytes, &pin) == VFS_OK && finish_extension(VFS_OK, pin) == VFS_OK);
    assert(exfat_node_read(file, 0U, 17U, actual) == 17U && !memcmp(actual, expected, 17U));
    release_extension();
}
static void large_extensions(void)
{
    for (u32 boundary = 0; boundary < 2U; boundary++) {
        vfs_node_t *file = prepare_extension(4U, 4U, true, false);
        u64 final = boundary ? (u64)(fs.boot.cluster_count + 2U - 10U) * fs.cluster_bytes :
                              2ULL * 1024U * 1024U + 1U;
        u64 bytes, pin;
        assert(exfat_node_write_pinned(file, final - 1U, 1U, "Z", &bytes, &pin) == VFS_OK && bytes == 1U);
        assert(finish_extension(VFS_OK, pin) == VFS_OK);
        for (u64 offset = 0; offset < final;) {
            u64 amount = final - offset; if (amount > 4096U) amount = 4096U;
            assert(exfat_node_read(file, offset, amount, actual) == amount);
            for (u64 i = 0; i < amount; i++)
                assert(actual[i] == (offset+i < 4U ? (u8)('A'+offset+i) :
                                     offset+i == final-1U ? 'Z' : 0U));
            offset += amount;
        }
        printf("ordinary extension length=%llu: initialized content checked in 4 KiB windows, peak_requested=%zu\n",
            (unsigned long long)final, (size_t)peak_requested);
        release_extension();
    }
}
#endif
static vfs_node_t *load_external(const char *path)
{
    setup(); device.sector_count = 12000U;
    FILE *image = fopen(path, "rb");
    assert(image && fread(media, sizeof(media), 1U, image) == 1U && !fclose(image));
    exfat_fs_t *mounted = exfat_new_state(&device); assert(mounted && mounted->cluster_bytes == 4096U);
    fs = *mounted; kfree(mounted); fs.nodes = &root_state; fs.next_inode = 1U;
    root_state.first_cluster = fs.root_cluster;
    root.size = root_state.data_length = root_state.valid_data_length = fs.cluster_bytes;
    vfs_node_t *file = exfat_finddir(&root, "old.txt"); assert(file); return file;
}
static void release_external(void)
{ kfree(fs.upcase); fs.upcase = NULL; release_extension(); }
static void external_extension(const char *path, u64 offset, u32 size)
{
    vfs_node_t *file = load_external(path); exfat_node_t *state = file->fs_data;
    u64 length = state->data_length, valid = state->valid_data_length;
    reference(length, valid, offset, size);
    u64 bytes, pin, final = length > offset + size ? length : offset + size;
    int result = exfat_node_write_pinned(file, offset, size, expected + offset, &bytes, &pin);
    assert(result == VFS_OK && bytes == size && finish_extension(result, pin) == VFS_OK);
    FILE *image = fopen(path, "wb");
    assert(image && fwrite(media, sizeof(media), 1U, image) == 1U && !fclose(image));
    assert(exfat_node_read(file, 0U, final, actual) == final);
    release_external(); file = load_external(path);
    assert(exfat_node_read(file, 0U, final, actual) == final);
#ifdef EXFAT_EXTENSION_BASELINE
    unsigned wrong = 0U;
    for (u64 i = valid; i < offset; i++) wrong += actual[i] != 0U;
    assert(wrong == (offset < 4096U ? offset : 4096U) - valid);
    printf("independent baseline image offset=%llu: %u poisoned bytes exposed on fresh production mount\n",
        (unsigned long long)offset, wrong);
#else
    assert(!memcmp(actual, expected, final));
    vfs_node_t *blocker = exfat_finddir(&root, "blocker");
    if (blocker) { u8 byte; assert(exfat_node_read(blocker, 0U, 1U, &byte) == 1U && byte == 'B'); }
    printf("independent image offset=%llu: byte-exact production readback before and after remount passed\n",
        (unsigned long long)offset);
#endif
    release_external();
}
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--image")) {
        external_extension(argv[2], strtoull(argv[3], NULL, 10), (u32)strtoul(argv[4], NULL, 10));
        return 0;
    }
    success_case(4U, 4U, 16U, 1U, true, false);
    success_case(4U, 4U, 4196U, 1U, true, false);
#ifndef EXFAT_EXTENSION_BASELINE
    const u64 offsets[] = {4U, 16U, 511U, 512U, 513U, 4095U, 4096U, 4196U, 8191U};
    for (u32 mode = 0; mode < 2; mode++) {
        for (u32 i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++)
            success_case(4U, 4U, offsets[i], 1U, mode != 0, false);
        success_case(300U, 4U, 16U, 1U, mode != 0, false);
        success_case(8000U, 4U, 4196U, 9U, mode != 0, false);
        success_case(8000U, 0U, 512U, 17U, mode != 0, false);
        success_case(8000U, 700U, 699U, 5U, mode != 0, false);
        success_case(4U, 4U, 4196U, 1U, mode != 0, true);
        success_case(0U, 0U, 8191U, 1U, mode != 0, false);
    }
    puts("exFAT initialized prefix: poisoned slack, sectors/clusters, contiguous/FAT, imported VDL and fresh gaps passed");
    repeated_and_bounds(); faults(); large_extensions();
#endif
}
