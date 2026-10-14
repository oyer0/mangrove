/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production driver and VFS, reusing controlled visibility dependencies. */
#define EXFAT_VISIBILITY_NO_MAIN
#include "exfat_system_test.c"

static void finish_boundary(int result, u64 pin)
{
    assert(result == VFS_OK && pin);
    assert(vfs_operation_finish_pin(&super, result, pin) == VFS_OK);
    assert(fs.version_confident && !fs.replace_transaction && !pin_active);
}
static void range_cases(void)
{
    setup(); add_system_entries();
    u32 last = fs.boot.cluster_count + 1U, mapped;
    assert(exfat_contiguous_range_valid(&fs, 2U, fs.boot.cluster_count));
    assert(exfat_contiguous_range_valid(&fs, last, 1U));
    assert(exfat_contiguous_range_valid(&fs, last - 1U, 2U));
    const u32 starts[] = {0U, 1U, last + 1U, ~(u32)0};
    unsigned before = writes, before_reads = reads;
    for (unsigned i = 0; i < sizeof(starts)/sizeof(starts[0]); i++) {
        assert(!exfat_contiguous_range_valid(&fs, starts[i], 1U));
        assert(!exfat_bitmap_range_unused(&fs, starts[i], 1U));
        assert(!exfat_bitmap_set_range(&fs, starts[i], 1U, true));
    }
    assert(!exfat_contiguous_range_valid(&fs, last, 0U));
    assert(!exfat_contiguous_range_valid(&fs, last, 2U));
    assert(!exfat_contiguous_range_valid(&fs, 2U, ~(u64)0));
    assert(writes == before && reads == before_reads);
    assert(exfat_chain_cluster(&fs, 2U, true, fs.boot.cluster_count - 1U, &mapped) && mapped == last);
    assert(!exfat_chain_cluster(&fs, 2U, true, fs.boot.cluster_count, &mapped));
    assert(exfat_bitmap_range_unused(&fs, last - 1U, 2U));
    assert(exfat_bitmap_set_range(&fs, last - 1U, 2U, true));
    assert(!exfat_bitmap_range_unused(&fs, last, 1U));
    assert(exfat_free_chain(&fs, last - 1U, true, 1024U));
    assert(exfat_bitmap_range_unused(&fs, last - 1U, 2U));
    assert(!exfat_free_chain(&fs, last, true, 1024U));
    cleanup();
}
static void lifecycle(const char *path, const char *output)
{
    if (path) load_image(path);
    else {
        setup(); add_system_entries(); super.reference_count = 1U;
        root.permissions = EXFAT_VOLUME_PERMISSIONS;
        memset(media[41], 0xff, 8U); media[41][7] &= 0x7fU;
        vfs_node_t *created = prepared_created_node(); vfs_file_facts_t facts;
        u64 pin; u8 input = 'B';
        assert(exfat_create_contents_locked(&root, created, "boundary.txt", 1U,
            replace_source, &input, &facts, &pin) == VFS_OK);
        bind_created_node(created, &facts); finish_boundary(VFS_OK, pin);
        /* Release two fixture-only unreachable reservations for replacement. */
        media[41][0] &= (u8)~0x18U; fs.allocation_count_valid = false;
    }
    clock_t start = clock(); usize baseline = live_requested;
    peak_requested = baseline;
    vfs_node_t *node; vfs_file_handle_t *handle; vfs_file_facts_t facts;
    assert(vfs_lookup("/boundary.txt", &node) == VFS_OK);
    exfat_node_t *state = node->fs_data;
    u64 count = (state->data_length + fs.cluster_bytes - 1U) / fs.cluster_bytes;
    u32 last;
    assert(exfat_chain_cluster(&fs, state->first_cluster, state->no_fat_chain, count - 1U, &last));
    assert(last == fs.boot.cluster_count + 1U);
    assert(exfat_file_facts(node, &facts) == VFS_OK);
    assert(exfat_ownership_preflight_locked(node) == VFS_OK);
    assert(vfs_open("/boundary.txt", VFS_OPEN_RDWR, &handle) == VFS_OK && handle->save_expectation_valid);
    u8 byte; assert(vfs_file_read(handle, 1U, &byte) == 1U && byte == 'B');
    assert(vfs_close(handle) == VFS_OK);
    u8 *snapshot = malloc((size_t)node->size); assert(snapshot);
    mg_file_lines_state_t saved;
    assert(exfat_snapshot_stream(node, (usize)node->size, sink, snapshot, &saved) == VFS_OK);
    for (u64 i = 0; i < node->size; i++) assert(snapshot[i] == 'B');
    free(snapshot);
    assert(vfs_write(node, 0U, 1U, "W") == 1U && fs.version_confident);
    u64 pin; u8 payload[513]; memset(payload, 'R', sizeof(payload));
    assert(vfs_super_operation_lock(&super));
    int result = exfat_replace_locked(&super, node->inode, state->generation,
        sizeof(payload), replace_source, payload, &pin);
    vfs_super_operation_unlock(&super); finish_boundary(result, pin);
    assert(exfat_file_facts(node, &facts) == VFS_OK && facts.size == sizeof(payload));
    assert(vfs_super_operation_lock(&super)); result = exfat_truncate_pinned(node, &pin);
    vfs_super_operation_unlock(&super); finish_boundary(result, pin);
    assert(exfat_file_facts(node, &facts) == VFS_OK && !facts.size);
    assert(vfs_unlink(&root, "boundary.txt") == VFS_OK && !state->alive);
    assert(vfs_lookup("/boundary.txt", &node) == VFS_ERR_NOT_FOUND && fs.version_confident);
    printf("exFAT boundary lifecycle: extra_peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
        (size_t)(peak_requested-baseline), reads, writes, flushes,
        1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
    if (output) { FILE *f=fopen(output,"wb"); assert(f && fwrite(media,sizeof(media),1U,f)==1U && !fclose(f)); }
    if (path) release_image(); else cleanup();
}
static void ordinary_growth_boundaries(void)
{
    for (unsigned existing = 0U; existing < 2U; existing++) {
        setup(); add_system_entries(); super.reference_count = 1U;
        root.permissions = EXFAT_VOLUME_PERMISSIONS;
        memset(media[41], 0xff, 8U); media[41][7] &= 0x7fU;
        if (existing) {
            add_entry(160U, "grow.txt", 0U, 64U, 512U, true);
            memset(media[102], 'B', 512U);
        }
        vfs_file_handle_t *handle;
        assert(vfs_open(existing ? "/grow.txt" : "/old.txt", VFS_OPEN_RDWR, &handle) == VFS_OK);
        handle->offset = existing ? 512U : 0U;
        u64 bytes;
        assert(vfs_file_write_status(handle, 1U, "B", &bytes) == VFS_OK && bytes == 1U);
        vfs_file_facts_t facts;
        assert(exfat_file_facts(handle->node, &facts) == VFS_OK && facts.size == (existing ? 513U : 1U));
        exfat_node_t *state = handle->node->fs_data;
        assert(state->no_fat_chain && state->first_cluster == (existing ? 64U : 65U));
        assert(exfat_ownership_preflight_locked(handle->node) == VFS_OK);
        assert(vfs_close(handle) == VFS_OK && fs.version_confident);
        cleanup();
    }
    puts("Empty-file and contiguous ordinary growth reach the final cluster without conversion");
}
static void boundary_faults(void)
{
    unsigned write_boundaries = 0, flush_boundaries = 0;
    for (unsigned mode = 0; mode < 5U; mode++) {
        unsigned limit = mode ? (mode == 4U ? flush_boundaries : write_boundaries) : 1U;
        for (unsigned at = 1U; at <= limit; at++) {
            setup(); add_system_entries(); super.reference_count = 1U;
            root.permissions = EXFAT_VOLUME_PERMISSIONS;
            memset(media[41], 0xff, 8U); media[41][7] &= 0x7fU;
            if (mode && mode < 4U) {
                fail_write_number = at;
                fail_write_copy_bytes = mode == 1U ? 0U : mode == 2U ? 128U : 512U;
            }
            if (mode == 4U) fail_flush_number = at;
            vfs_node_t *node = prepared_created_node(); vfs_file_facts_t facts;
            u64 pin; u8 byte = 'B';
            int result = exfat_create_contents_locked(&root, node, "boundary.txt", 1U,
                replace_source, &byte, &facts, &pin);
            if (result == VFS_OK) bind_created_node(node, &facts);
            if (pin) result = vfs_operation_finish_pin(&super, result, pin);
            if (!mode) {
                assert(result == VFS_OK && fs.version_confident);
                write_boundaries = writes; flush_boundaries = flushes;
            } else {
                assert(result == VFS_ERR_UNCERTAIN && pin_active && pin_unresolved &&
                    !fs.version_confident && fs.replace_transaction);
                unsigned issued = writes;
                vfs_node_t spare = {0};
                assert(exfat_create_contents_locked(&root, &spare, "again", 0U,
                    NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN && writes == issued);
                discard_unresolved_fixture_pin();
            }
            if (!node->fs_data) kfree(node);
            cleanup();
        }
    }
    printf("Final-cluster fault coverage: %u write and %u flush boundaries\n", write_boundaries, flush_boundaries);
}
static void malformed(const char *path)
{
    load_image(path); vfs_node_t *node = exfat_finddir(&root,"boundary.txt"); assert(node);
    unsigned before = writes; vfs_file_facts_t facts;
    assert(exfat_file_facts(node,&facts)==VFS_ERR_BAD_FORMAT && !fs.version_confident);
    assert(!writes && writes==before && !pin_active); release_image();
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1],"--invalid")) { malformed(argv[2]); return 0; }
    if (argc == 3) { lifecycle(argv[1],argv[2]); return 0; }
    assert(argc==1); range_cases(); lifecycle(NULL,NULL); ordinary_growth_boundaries(); boundary_faults();
    puts("exFAT inclusive cluster-count and invalid-range production checks passed"); return 0;
}
