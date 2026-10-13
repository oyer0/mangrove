/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production driver/decoder; controlled block, heap, pin and realtime sources. */
#define main exfat_metadata_fixture_main
#include "exfat_mount_identity_test.c"
#undef main

static vfs_node_t *metadata_file, *metadata_parent, *metadata_created;
static u8 input[700];
static int complete_result(int result, u64 pin)
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
static void prepare_metadata(unsigned operation)
{
    metadata_created = NULL;
    if (operation < 4U) {
        metadata_file = prepare_split_replacement(); metadata_parent = &root;
    } else {
        vfs_node_t *existing;
        metadata_parent = prepare_cross_create(4U, &existing);
        assert(metadata_parent != &root);
        assert(create_node_finished(metadata_parent, "metadata", &metadata_file,
                                   operation == 8U) == VFS_OK);
        if (operation == 4U || operation == 5U) metadata_created = prepared_created_node();
    }
    memset(input, 'M', sizeof(input));
    test_calendar = (mg_calendar_time_t){2026, 12, 31, 23, 59, 59, 0};
    writes = reads = flushes = bitmap_writes = 0U;
    allocations = 0U; peak_requested = live_requested;
}
static int perform_metadata(unsigned operation)
{
    u64 pin = 0U, bytes = 0U; int result;
    vfs_file_facts_t facts;
    switch (operation) {
    case 0: result = exfat_node_write_pinned(metadata_file, 699U, 2U, "XY", &bytes, &pin); break;
    case 1: result = exfat_node_write_pinned(metadata_file, 0U, 1U, "X", &bytes, &pin); break;
    case 2: result = exfat_replace_locked(&super, metadata_file->inode,
        ((exfat_node_t *)metadata_file->fs_data)->generation, sizeof(input),
        replace_source, input, &pin); break;
    case 3: result = exfat_truncate_pinned(metadata_file, &pin); break;
    case 4: result = exfat_create_contents_locked(metadata_parent, metadata_created,
        "created", sizeof(input), replace_source, input, &facts, &pin); break;
    case 5: result = exfat_mkdir_pinned(metadata_parent, metadata_created, "created", &pin); break;
    case 6: result = exfat_rename_pinned(&super, metadata_parent, "metadata",
        metadata_parent, "renamed", &pin); break;
    case 7: result = exfat_unlink_pinned(metadata_parent, "metadata", &pin); break;
    default: result = exfat_rmdir_pinned(metadata_parent, "metadata", &pin); break;
    }
    result = complete_result(result, pin);
    if (result == VFS_OK && operation == 4U) bind_created_node(metadata_created, &facts);
    return result;
}
static void release_metadata(void)
{
    if (pin_unresolved) discard_unresolved_fixture_pin();
    if (metadata_created && !metadata_created->fs_data) kfree(metadata_created);
    metadata_created = NULL; cleanup();
}
static void current_times(vfs_node_t *node, bool creation)
{
    u8 set[640]; exfat_node_t *state = node->fs_data;
    assert(exfat_node_entry_read(state, &fs, set, sizeof(set)));
    u32 modified = exfat_le32(set + 12U), access = exfat_le32(set + 16U);
    assert((modified >> 25) + 1980U == 2026U && ((modified >> 21) & 15U) == 12U &&
        ((modified >> 16) & 31U) == 31U && ((modified >> 11) & 31U) == 23U &&
        ((modified >> 5) & 63U) == 59U && (modified & 31U) == 29U);
    assert(access == modified && set[21] == 100U && set[23] == 0x80U && set[24] == 0x80U);
    if (creation) assert(exfat_le32(set + 8U) == modified && set[20] == 100U && set[22] == 0x80U);
    assert(exfat_le16(set + 2U) == exfat_entry_checksum(set, state->entry_count));
}
static void allocation_accounting(void)
{
    u64 allocated = 0U;
    for (u32 cluster = 2; cluster < fs.boot.cluster_count + 2U; cluster++) {
        bool bit; assert(exfat_bitmap_bit(&fs, cluster, &bit)); allocated += bit;
    }
    assert((!fs.allocation_count_valid || allocated == fs.allocated_clusters) &&
           media[0][112] == allocated * 100U / fs.boot.cluster_count &&
           !(exfat_le16(media[0]+106) & 2U));
}
static void timestamp_boundaries(void)
{
    exfat_replace_transaction_t tx = {0};
    const mg_calendar_time_t valid[] = {{1980,1,1,0,0,0,0}, {2000,2,29,1,2,3,0},
        {2107,12,31,23,59,59,0}};
    for (usize i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
        test_calendar = valid[i]; assert(exfat_timestamp_sample(&tx) == VFS_OK);
        assert((tx.timestamp >> 25) + 1980U == (u32)valid[i].year);
        assert(tx.timestamp_increment == (valid[i].second & 1U) * 100U);
    }
    const mg_calendar_time_t invalid[] = {{1979,12,31,0,0,0,0}, {2108,1,1,0,0,0,0},
        {2001,2,29,0,0,0,0}, {2026,13,1,0,0,0,0}};
    for (usize i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        test_calendar = invalid[i]; assert(exfat_timestamp_sample(&tx) == VFS_ERR_TIME_UNAVAILABLE);
    }
}
static void metadata_regressions(void)
{
    unsigned faults = 0;
    for (unsigned op = 0; op < 9U; op++) {
        prepare_metadata(op);
        u8 before[640];
        assert(exfat_node_entry_read(metadata_file->fs_data, &fs, before, sizeof(before)));
        usize baseline = live_requested; clock_t start = clock();
        assert(perform_metadata(op) == VFS_OK && !pin_active);
        printf("metadata operation=%u peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
            op, (size_t)(peak_requested-baseline), reads, writes, flushes,
            1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
        unsigned nw = writes, nf = flushes, na = allocations;
        if (op < 4U) {
            current_times(metadata_file, false);
            u8 after[640]; assert(exfat_node_entry_read(metadata_file->fs_data, &fs, after, sizeof(after)));
            assert(!memcmp(before+8, after+8, 4U) && before[20] == after[20] && before[22] == after[22]);
            unsigned attempts = writes; char byte;
            (void)exfat_node_read(metadata_file, 0U, 1U, &byte);
            u8 again[640]; assert(exfat_node_entry_read(metadata_file->fs_data, &fs, again, sizeof(again)));
            assert(writes == attempts && !memcmp(after, again, sizeof(after)));
        } else {
            current_times(metadata_parent, false);
            if (op == 4U || op == 5U) current_times(metadata_created, true);
            if (op == 6U) {
                u8 after[640]; assert(exfat_node_entry_read(metadata_file->fs_data, &fs, after, sizeof(after)));
                assert(!memcmp(before+8, after+8, 17U));
            }
        }
        allocation_accounting(); release_metadata();
        for (unsigned at = 1; at <= nw; at++) for (unsigned torn = 0; torn < 3U; torn++) {
            prepare_metadata(op); fail_write_number = at;
            fail_write_copy_bytes = torn == 0 ? 0 : torn == 1 ? 128 : 512;
            assert(perform_metadata(op) == VFS_ERR_UNCERTAIN && pin_unresolved &&
                fs.replace_transaction && !fs.version_confident && writes == at);
            exfat_percent_transaction_t *percent = fs.replace_transaction->percent;
            if (percent && percent->unknown_flushed && !percent->final_attempted)
                assert(media[0][112] == 0xffU);
            release_metadata(); faults++;
        }
        for (unsigned at = 1; at <= nf; at++) {
            prepare_metadata(op); fail_flush_number = at;
            assert(perform_metadata(op) == VFS_ERR_UNCERTAIN && pin_unresolved && fs.replace_transaction);
            release_metadata(); faults++;
        }
        for (unsigned at = 1; at <= nw; at++) {
            prepare_metadata(op); remove_after_write_number = at;
            assert(perform_metadata(op) == VFS_ERR_UNCERTAIN && pin_unresolved && fs.replace_transaction);
            release_metadata(); faults++;
        }
        for (unsigned at = 1; at <= na; at++) {
            prepare_metadata(op); fail_allocation = at;
            assert(perform_metadata(op) == VFS_ERR_NO_MEM && !writes && !pin_active);
            release_metadata(); faults++;
        }
        prepare_metadata(op); realtime_available = false;
        assert(perform_metadata(op) == VFS_ERR_TIME_UNAVAILABLE && !writes && !pin_active);
        release_metadata();
    }
    printf("metadata mutation and completion fault boundaries: %u verified\n", faults);
}
static void shared_ancestor_and_noop(void)
{
    setup(); add_system_entries(); test_calendar = (mg_calendar_time_t){2026,10,13,12,0,0,0};
    vfs_node_t *left, *right, *file;
    assert(create_node_finished(&root, "left", &left, true) == VFS_OK);
    assert(create_node_finished(&root, "right", &right, true) == VFS_OK);
    exfat_node_t *ls = left->fs_data, *rs = right->fs_data;
    assert(ls->entry_offset / 512U == rs->entry_offset / 512U);
    assert(create_node_finished(left, "file", &file, false) == VFS_OK);
    realtime_available = false;
    unsigned before = writes; u64 pin;
    u64 generation = ((exfat_node_t *)file->fs_data)->generation;
    assert(exfat_truncate_pinned(file, &pin) == VFS_OK && !pin && writes == before &&
        ((exfat_node_t *)file->fs_data)->generation == generation);
    realtime_available = true;
    test_calendar = (mg_calendar_time_t){2026,12,31,23,59,59,0};
    int result = exfat_rename_pinned(&super, left, "file", right, "moved", &pin);
    assert(complete_result(result, pin) == VFS_OK);
    current_times(left, false); current_times(right, false);
    assert(exfat_finddir(right, "moved") == file && !exfat_finddir(left, "file"));
    assert(exfat_ownership_preflight_locked(file) == VFS_OK);
    cleanup();
    for (unsigned op = 0; op < 9U; op++) {
        prepare_metadata(op);
        test_calendar = (mg_calendar_time_t){2108,1,1,0,0,0,0};
        assert(perform_metadata(op) == VFS_ERR_TIME_UNAVAILABLE && !writes && !pin_active);
        release_metadata();
    }
}
static void ancestor_bound(void)
{
    setup_large();
    u8 set[640], count;
    const u32 depth = EXFAT_SCAN_MAX_DIRECTORIES - 1U;
    u32 previous = 2U;
    /* A narrow imported tree reaches the existing directory/slot proof bound.
     * Only the leaf is cached; ancestry must come from the protected scan. */
    for (u32 i = 0; i < depth; i++) {
        u32 cluster = 7U + i;
        assert(exfat_build_file_set(&fs, "child", EXFAT_ATTR_DIRECTORY,
            cluster, 512U, 512U, true, set, &count));
        u64 lba = exfat_cluster_lba(&fs, previous);
        u32 at = 0U; /* Replace the old empty file, preserve system entries. */
        memcpy(media[lba] + at, set, (usize)count * 32U);
        if (i) memset(media[lba] + count * 32U, 0, 512U - count * 32U);
        u32 bit = cluster - 2U;
        media[121U + bit / 4096U][(bit / 8U) % 512U] |= 1U << (bit & 7U);
        previous = cluster;
    }
    exfat_node_t leaf = {0}; vfs_node_t node = {0};
    leaf.first_cluster = previous; leaf.no_fat_chain = true;
    leaf.attributes = EXFAT_ATTR_DIRECTORY; leaf.data_length = leaf.valid_data_length = 512U;
    leaf.parent_cluster = previous - 1U; leaf.parent_no_fat_chain = true;
    leaf.entry_count = 3; strcpy(leaf.name, "child");
    leaf.alive = leaf.version_valid = true; leaf.generation = 1;
    leaf.vfs_node = &node;
    node.super = &super; node.fs_data = &leaf; node.type = VFS_TYPE_DIRECTORY; node.inode = 2; fs.next_inode = 2;
    usize baseline = live_requested; peak_requested = baseline;
    clock_t start = clock();
    vfs_node_t *created = prepared_created_node(); vfs_file_facts_t facts; u64 pin;
    assert(exfat_create_contents_locked(&node, created, "last", 0U, NULL, NULL, &facts, &pin) == VFS_OK);
    assert(fs.replace_transaction->timestamps->count == depth);
    usize retained = live_requested - baseline;
    bind_created_node(created, &facts); finish_pin();
    printf("ancestor depth=%u peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f aggregate32=%zu\n",
        depth, (size_t)(peak_requested-baseline), (size_t)retained, reads, writes, flushes,
        1000.0*(double)(clock()-start)/CLOCKS_PER_SEC, (size_t)(32U*retained));
    assert(exfat_ownership_preflight_locked(&node) == VFS_OK);
    cleanup();
}
static void save_external(const char *path)
{
    FILE *out = fopen(path, "wb"); assert(out && fwrite(media, sizeof(media), 1, out) == 1 && !fclose(out));
}
static void external_metadata(int argc, char **argv)
{
    assert(argc == 5);
    setup(); device.sector_count = 12000U;
    FILE *image = fopen(argv[1], "rb"); assert(image && fread(media, sizeof(media), 1, image) == 1 && !fclose(image));
    exfat_boot_info_t geometry; u8 original_percent = media[0][112];
    const u8 invalid_percent[] = {101U, 254U};
    for (usize i = 0; i < sizeof(invalid_percent); i++) {
        media[0][112] = invalid_percent[i];
        assert(!exfat_read_geometry(&device, device.id, &geometry) && !writes);
    }
    media[0][112] = 0xffU;
    assert(exfat_read_geometry(&device, device.id, &geometry) && !writes);
    media[0][112] = original_percent;
    vfs_super_t *mounted;
    assert(exfat_mount(NULL, &device, &mounted, false) == VFS_OK && !writes);
    exfat_fs_t *loaded = mounted->private_data;
    exfat_node_t *private_root = mounted->root_node->fs_data;
    root_state = *private_root; root = *mounted->root_node; fs = *loaded;
    kfree(private_root); kfree(mounted->root_node); kfree(loaded); kfree(mounted);
    fs.nodes = &root_state; root_state.vfs_node = &root;
    root.fs_data = &root_state; root.super = &super; super.root_node = &root; super.private_data = &fs;
    vfs_node_t *file = exfat_finddir(&root, "split.txt"); assert(file);
    assert(((exfat_node_t *)file->fs_data)->entry_offset % 512U == 480U);
    u8 before[640]; assert(exfat_node_entry_read(file->fs_data, &fs, before, sizeof(before)));
    test_calendar = (mg_calendar_time_t){2026,12,31,23,59,59,0};
    u8 *large = malloc(MG_FILE_CONTENT_MAX_BYTES); assert(large); memset(large, 'L', MG_FILE_CONTENT_MAX_BYTES);
    u64 pin = 0, bytes; int result;
    result = exfat_node_write_pinned(file, 0, MG_FILE_CONTENT_MAX_BYTES, large, &bytes, &pin); assert(complete_result(result, pin) == VFS_OK);
    assert(bytes == MG_FILE_CONTENT_MAX_BYTES); current_times(file, false); allocation_accounting();
    assert(media[0][112] > 10U && media[0][112] < 100U); save_external(argv[2]);
    exfat_node_t *state = file->fs_data;
    result = exfat_replace_locked(&super, file->inode, state->generation,
        513U, replace_source, large, &pin); assert(complete_result(result, pin) == VFS_OK);
    current_times(file, false); allocation_accounting();
    assert(exfat_node_entry_read(file->fs_data, &fs, before, sizeof(before)));
    vfs_node_t *directory;
    assert(create_node_finished(&root, "nested", &directory, true) == VFS_OK);
    vfs_node_t *child; assert(create_node_finished(directory, "child", &child, false) == VFS_OK);
    current_times(directory, true); current_times(child, true);
    result = exfat_node_write_pinned(child, 0, 1, "X", &bytes, &pin); assert(complete_result(result, pin) == VFS_OK);
    current_times(directory, false); allocation_accounting(); save_external(argv[3]);
    result = exfat_unlink_pinned(directory, "child", &pin); assert(complete_result(result, pin) == VFS_OK);
    result = exfat_rmdir_pinned(&root, "nested", &pin); assert(complete_result(result, pin) == VFS_OK);
    result = exfat_truncate_pinned(file, &pin); assert(complete_result(result, pin) == VFS_OK);
    allocation_accounting(); save_external(argv[4]);
    free(large); kfree(fs.upcase); fs.upcase = NULL; cleanup();
    puts("Independent split file: 2 MiB write, replacement, child timestamps and deletion fields verified");
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) { external_metadata(argc, argv); return 0; }
    timestamp_boundaries(); metadata_regressions(); shared_ancestor_and_noop(); ancestor_bound();
    printf("metadata structures: tx=%zu percent=%zu cleanup=%zu ancestor=%zu job=%zu\n",
        sizeof(exfat_replace_transaction_t), sizeof(exfat_percent_transaction_t),
        sizeof(exfat_boot_cleanup_t), sizeof(exfat_timestamp_entry_t), sizeof(exfat_scan_directory_t));
    return 0;
}
