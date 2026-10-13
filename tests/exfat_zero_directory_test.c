/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production driver and VFS dispatch; controlled block, lock and pin services. */
#define main exfat_identity_fixture_main
#include "exfat_mount_identity_test.c"
#undef main
#include "vfs_zero_directory.inc"

static vfs_node_t *zero_directory(unsigned split)
{
    setup(); add_system_entries();
    u64 at = split ? 480U : 160U;
    if (split) {
        ((u32 *)media[24])[2] = 20U;
        ((u32 *)media[24])[20] = EXFAT_CLUSTER_EOC;
        media[41][2] |= 1U << 2; /* cluster 20 */
        for (u32 i = 160U; i < 480U; i += 32U) media[40][i] = 0x05;
    }
    for (unsigned i = 0; i < 2; i++) {
        u8 set[96], count;
        assert(exfat_build_file_set(&fs, i ? "other" : "empty", EXFAT_ATTR_DIRECTORY,
            0, 0, 0, false, set, &count) && count == 3);
        for (unsigned j = 0; j < 3; j++) {
            u64 offset = at + i*96U + j*32U;
            memcpy(media[offset < 512U ? 40U : 58U] + offset%512U, set+j*32U, 32U);
        }
    }
    vfs_node_t *dir = exfat_finddir(&root, "empty");
    assert(dir && exfat_finddir(&root, "other") != dir);
    super.reference_count = 1U;
    reads = writes = flushes = allocations = 0U;
    return dir;
}

static void zero_reads(void)
{
    for (unsigned split = 0; split < 2; split++) {
        vfs_node_t *dir = zero_directory(split);
        u64 capacity = 123;
        assert(exfat_directory_capacity(&fs, 0, false, 0, &capacity) && !capacity);
        unsigned before = reads;
        vfs_dirent_t entry, saved; memset(&entry, 0xa5, sizeof(entry)); saved = entry;
        assert(exfat_readdir_checked(dir, 0, &entry) == 0 && !memcmp(&entry, &saved, sizeof(entry)));
        void *cursor; assert(exfat_readdir_open(dir, &cursor));
        assert(exfat_readdir_next_checked(cursor, &entry) == 0 && reads == before);
        exfat_readdir_close(cursor);
        assert(!exfat_finddir(dir, "missing") && reads == before);
        assert(exfat_ownership_preflight_locked(dir) == VFS_OK && fs.version_confident && !writes);
        exfat_entry_info_t facts;
        exfat_node_t *state = dir->fs_data;
        assert(exfat_entry_set_read(&fs, state->parent_cluster, false, state->entry_offset, &facts));
        assert(exfat_directory_empty(&fs, &facts) == VFS_OK);
        unsigned bitmap_before = bitmap_writes;
        assert(vfs_rmdir(&root, "empty") == VFS_OK && !state->alive &&
            bitmap_writes == bitmap_before && !pin_active && !fs.replace_transaction);
        assert(vfs_rmdir(&root, "empty") == VFS_ERR_NOT_FOUND);
        assert(exfat_finddir(&root, "other") && fs.version_confident);
        cleanup();
    }
}

static void zero_create(void)
{
    for (unsigned split = 0; split < 2; split++)
        for (unsigned directory = 0; directory < 2; directory++)
            for (unsigned maximum = 0; maximum < 2; maximum++) {
                vfs_node_t *parent = zero_directory(split), *child;
                char name[256]; memset(name, 'Z', 255); name[255] = 0;
                if (!maximum) strcpy(name, "R\xc3\xa9sum\xc3\xa9.txt");
                void *cursor; assert(exfat_readdir_open(parent, &cursor));
                usize baseline = live_requested; peak_requested = baseline;
                clock_t start = clock();
                int result = directory ? vfs_mkdir(parent, name, &child) : vfs_create(parent, name, &child);
                assert(result == VFS_OK && child && exfat_finddir(parent, name) == child &&
                    !pin_active && fs.version_confident && super.reference_count == 1U);
                exfat_node_t *p = parent->fs_data, *c = child->fs_data;
                assert(p->first_cluster >= 2 && !p->no_fat_chain &&
                    p->data_length == (maximum ? 1024 : 512) &&
                    p->valid_data_length == p->data_length && parent->size == p->data_length &&
                    c->parent_cluster == p->first_cluster && !c->parent_no_fat_chain);
                vfs_dirent_t out;
                assert(exfat_readdir_next_checked(cursor, &out) == 1 && !strcmp(out.name, name));
                exfat_readdir_close(cursor);
                assert(exfat_ownership_preflight_locked(child) == VFS_OK);
                printf("exFAT zero-directory first %s split=%u max=%u: peak=%zu reads=%u writes=%u flushes=%u allocations=%u cpu_ms=%.3f\n",
                    directory ? "mkdir" : "create", split, maximum,
                    (size_t)(peak_requested-baseline), reads, writes, flushes, allocations,
                    1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
                assert(vfs_rmdir(&root, "empty") == VFS_ERR_NOT_EMPTY);
                for (unsigned i = 0; i < 10; i++) {
                    char more[24]; snprintf(more, sizeof(more), "child%u", i);
                    vfs_node_t *member; assert(vfs_create(parent, more, &member) == VFS_OK);
                    assert(exfat_finddir(parent, more) == member);
                }
                for (unsigned i = 0; i < 10; i++) {
                    char more[24]; snprintf(more, sizeof(more), "child%u", i);
                    assert(vfs_unlink(parent, more) == VFS_OK);
                }
                if (directory) assert(vfs_rmdir(parent, name) == VFS_OK);
                else assert(vfs_unlink(parent, name) == VFS_OK);
                assert(vfs_rmdir(&root, "empty") == VFS_OK && fs.version_confident);
                cleanup();
            }
}

static void zero_faults(void)
{
    unsigned injected = 0;
    char maximum[256]; memset(maximum, 'X', 255); maximum[255] = 0;
    for (unsigned split = 0; split < 2; split++)
        for (unsigned directory = 0; directory < 2; directory++) {
            vfs_node_t *parent = zero_directory(split), *child;
            assert((directory ? vfs_mkdir(parent, maximum, &child) :
                    vfs_create(parent, maximum, &child)) == VFS_OK);
            unsigned total_reads = reads, total_writes = writes;
            unsigned total_flushes = flushes, total_allocations = allocations;
            cleanup();
            for (unsigned replacement = 0; replacement < 2; replacement++)
                for (unsigned boundary = 1; boundary <= total_reads; boundary++) {
                    parent = zero_directory(split);
                    u8 bitmap[512]; memcpy(bitmap, media[41], sizeof(bitmap));
                    if (replacement) replace_after_reads = boundary;
                    else fail_read_number = boundary;
                    child = (void *)1;
                    int result = directory ? vfs_mkdir(parent, maximum, &child) :
                        vfs_create(parent, maximum, &child);
                    assert(result < 0 && !child && super.reference_count == 1U);
                    if (pin_active) {
                        assert(pin_unresolved && !fs.version_confident &&
                            result == VFS_ERR_UNCERTAIN);
                        discard_unresolved_fixture_pin();
                    } else {
                        assert(!fs.replace_transaction && result != VFS_ERR_UNCERTAIN);
                        /* A failed staging read can cancel confirmed fresh
                         * allocations before either parent sector is attempted. */
                        assert(!memcmp(bitmap, media[41], sizeof(bitmap)));
                    }
                    assert(!((exfat_node_t *)parent->fs_data)->first_cluster);
                    cleanup(); injected++;
                }
            for (unsigned mode = 0; mode < 4; mode++)
                for (unsigned boundary = 1; boundary <= (mode == 2 ? total_flushes : total_writes); boundary++) {
                    parent = zero_directory(split);
                    if (mode < 2) { fail_write_number = boundary; fail_write_copy_bytes = mode ? 128 : 0; }
                    if (mode == 2) fail_flush_number = boundary;
                    if (mode == 3) remove_after_write_number = boundary;
                    child = (void *)1;
                    assert((directory ? vfs_mkdir(parent, maximum, &child) :
                            vfs_create(parent, maximum, &child)) == VFS_ERR_UNCERTAIN &&
                        !child && pin_active && pin_unresolved && !fs.version_confident &&
                        super.reference_count == 1U);
                    exfat_directory_growth_t *g = fs.replace_transaction->directory_growth;
                    assert(g && g->first_allocation && g->appended && !g->tail_attempted &&
                        !g->marker_attempted && g->candidate && g->extra_candidate);
                    exfat_node_t *p = parent->fs_data;
                    assert(!p->first_cluster && !p->data_length && p->generation == 1);
                    unsigned attempted = writes; u64 pin;
                    assert(exfat_rmdir_pinned(&root, "empty", &pin) == VFS_ERR_UNCERTAIN && !pin);
                    assert(!exfat_finddir(&root, "empty") && writes == attempted);
                    discard_unresolved_fixture_pin(); cleanup(); injected++;
                }
            for (unsigned boundary = 1; boundary <= total_allocations; boundary++) {
                parent = zero_directory(split); fail_allocation = boundary;
                assert((directory ? vfs_mkdir(parent, maximum, &child) :
                        vfs_create(parent, maximum, &child)) == VFS_ERR_NO_MEM &&
                    !child && !writes && !pin_active && fs.version_confident);
                cleanup(); injected++;
            }
            parent = zero_directory(split); fail_pin_begin = true;
            assert((directory ? vfs_mkdir(parent, maximum, &child) :
                    vfs_create(parent, maximum, &child)) == VFS_ERR_NO_SPACE && !child &&
                !writes && !pin_active && fs.version_confident);
            cleanup();
        }
    /* Source failure precedes any parent-entry publication. Cancel both
     * reserved directory clusters and the staged file allocation. */
    for (unsigned cleanup_failure = 0; cleanup_failure < 5; cleanup_failure++) {
        vfs_node_t *parent = zero_directory(1), *prepared = prepared_created_node();
        u8 bitmap[512]; memcpy(bitmap, media[41], sizeof(bitmap));
        vfs_file_facts_t facts; u64 pin;
        if (cleanup_failure && cleanup_failure < 4) fail_bitmap_write_number = 3 + cleanup_failure;
        if (cleanup_failure == 4) fail_flush_number = 1;
        int result = exfat_create_contents_locked(parent, prepared, maximum, 700,
            short_source, NULL, &facts, &pin);
        assert(result == (cleanup_failure ? VFS_ERR_UNCERTAIN : VFS_ERR_IO));
        assert(!prepared->fs_data && !((exfat_node_t *)parent->fs_data)->first_cluster);
        if (cleanup_failure) {
            assert(!pin && pin_unresolved && fs.replace_transaction && !fs.version_confident);
            discard_unresolved_fixture_pin();
        } else {
            assert(pin && !pin_unresolved); finish_pin();
            assert(!memcmp(bitmap, media[41], sizeof(bitmap)) && fs.version_confident);
        }
        kfree(prepared); cleanup(); injected++;
    }
    printf("exFAT zero-directory failure boundaries: %u\n", injected);
}

static void zero_fragmented_contents(void)
{
    vfs_node_t *parent = zero_directory(1);
    u32 count = 0;
    for (u32 cluster = 6; cluster <= 64; cluster += 2) {
        if (cluster == 20) continue; /* root's second cluster */
        media[41][(cluster-2)/8] |= (u8)(1U << ((cluster-2)%8));
        ((u32 *)media[24])[cluster] = cluster == 64 ? EXFAT_CLUSTER_EOC :
            cluster == 18 ? 22 : cluster + 2;
        count++;
    }
    u8 *old = media[40];
    exfat_put32(old + 52, 6U); exfat_put64(old + 40, count*512U); exfat_put64(old + 56, count*512U);
    exfat_put16(old + 2, exfat_entry_checksum(old, 3));
    char name[256]; memset(name, 'Z', 255); name[255] = 0;
    char contents[700]; memset(contents, 'D', sizeof(contents));
    vfs_node_t *created = prepared_created_node(); vfs_file_facts_t facts; u64 pin;
    usize baseline = live_requested; peak_requested = baseline;
    clock_t start = clock();
    assert(exfat_create_contents_locked(parent, created, name, sizeof(contents),
        replace_source, contents, &facts, &pin) == VFS_OK && pin);
    exfat_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx->directory_growth->candidate == 5 && tx->directory_growth->extra_candidate == 7 &&
        !tx->new_no_fat && tx->fresh_count == 2 && tx->fresh_clusters[0] == 9 && tx->fresh_clusters[1] == 11);
    usize retained = live_requested - baseline;
    printf("exFAT zero-directory fragmented create: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f; growth=%zu common=%zu\n",
        (size_t)(peak_requested-baseline), (size_t)retained, reads, writes, flushes,
        1000.0*(double)(clock()-start)/CLOCKS_PER_SEC,
        sizeof(exfat_directory_growth_t), sizeof(exfat_replace_transaction_t));
    bind_created_node(created, &facts); finish_pin();
    char actual[700]; assert(exfat_node_read(created, 0, sizeof(actual), actual) == sizeof(actual) &&
        !memcmp(actual, contents, sizeof(actual)) && exfat_ownership_preflight_locked(created) == VFS_OK);
    assert(((u32 *)media[24])[5] == 7 && ((u32 *)media[24])[6] == 8);
    assert(vfs_unlink(parent, name) == VFS_OK && vfs_rmdir(&root, "empty") == VFS_OK);
    cleanup();
}

static void zero_maximum_contents(void)
{
    for (unsigned fragmented = 0; fragmented < 2; fragmented++) {
        setup_large();
        if (fragmented) {
            const u32 last = 9000U;
            for (u32 c = 8U; c <= last; c += 2U) {
                u32 bit = c-2U;
                media[121U+bit/4096U][(bit/8U)%512U] |= 1U << (bit%8U);
                exfat_put32(media[24U+c/128U]+(c%128U)*4U, c == last ? EXFAT_CLUSTER_EOC : c+2U);
            }
            u8 *set = media[120]; u64 size = ((last-8U)/2U+1U)*512ULL;
            exfat_put32(set+52U, 8U); exfat_put64(set+40U, size); exfat_put64(set+56U, size);
            exfat_put16(set+2U, exfat_entry_checksum(set, 3));
        }
        u8 set[96], count;
        assert(exfat_build_file_set(&fs, "empty", EXFAT_ATTR_DIRECTORY, 0, 0, 0, false, set, &count));
        memcpy(media[120]+160U, set, sizeof(set));
        vfs_node_t *parent = exfat_finddir(&root, "empty"), *created = prepared_created_node();
        assert(parent); super.reference_count = 1;
        char name[256]; memset(name, 'Z', 255); name[255] = 0;
        char *contents = malloc(MG_FILE_CONTENT_MAX_BYTES), *actual = malloc(MG_FILE_CONTENT_MAX_BYTES);
        assert(contents && actual); memset(contents, 'Q', MG_FILE_CONTENT_MAX_BYTES);
        reads = writes = flushes = allocations = 0;
        usize baseline = live_requested; peak_requested = baseline;
        clock_t start = clock(); vfs_file_facts_t facts; u64 pin;
        assert(exfat_create_contents_locked(parent, created, name, MG_FILE_CONTENT_MAX_BYTES,
            replace_source, contents, &facts, &pin) == VFS_OK && pin);
        exfat_replace_transaction_t *tx = fs.replace_transaction;
        assert(tx->fresh_capacity == 4096 && tx->fresh_count == 4096 &&
            tx->new_no_fat == !fragmented && tx->directory_growth->first_allocation);
        usize retained = live_requested-baseline;
        printf("exFAT zero-directory 2 MiB fragmented=%u: peak=%zu retained=%zu (32=%zu) reads=%u writes=%u flushes=%u allocations=%u cpu_ms=%.3f\n",
            fragmented, (size_t)(peak_requested-baseline), (size_t)retained, (size_t)(32*retained),
            reads, writes, flushes, allocations, 1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
        bind_created_node(created, &facts); finish_pin();
        assert(exfat_node_read(created, 0, MG_FILE_CONTENT_MAX_BYTES, actual) == MG_FILE_CONTENT_MAX_BYTES &&
            !memcmp(contents, actual, MG_FILE_CONTENT_MAX_BYTES) && exfat_ownership_preflight_locked(created) == VFS_OK);
        assert(vfs_unlink(parent, name) == VFS_OK && vfs_rmdir(&root, "empty") == VFS_OK);
        free(contents); free(actual); cleanup();
    }
}

static void zero_corruption_and_moves(void)
{
    for (unsigned malformed = 0; malformed < 6; malformed++) {
        vfs_node_t *parent = zero_directory(0), *child;
        u8 *set = media[40] + 160U;
        if (malformed == 0) set[33] |= EXFAT_STREAM_NO_FAT_CHAIN;
        if (malformed == 1) exfat_put32(set + 52, 5U);
        if (malformed == 2) exfat_put64(set + 40, 512U);
        if (malformed == 3) exfat_put64(set + 56, 512U);
        if (malformed == 4) { exfat_put64(set + 56, 1U); exfat_put64(set + 40, 1U); }
        exfat_put16(set + 2, exfat_entry_checksum(set, 3U));
        if (malformed == 5) set[2] ^= 1U;
        assert(vfs_create(parent, "child", &child) == VFS_ERR_BAD_FORMAT && !child &&
            !writes && !pin_active && !fs.version_confident);
        cleanup();
    }
    vfs_node_t *parent = zero_directory(1), *other = exfat_finddir(&root, "other");
    u64 pin; unsigned before = writes;
    assert(exfat_rename_pinned(&super, &root, "empty", parent, "self", &pin) ==
        VFS_ERR_INVALID_PARAM && !pin && writes == before && fs.version_confident);
    assert(exfat_rename_pinned(&super, &root, "empty", other, "moved", &pin) == VFS_OK && pin);
    finish_pin();
    assert(exfat_finddir(other, "moved") == parent && !exfat_finddir(&root, "empty") &&
        exfat_ownership_preflight_locked(parent) == VFS_OK);
    vfs_node_t *child; assert(vfs_mkdir(parent, "nested", &child) == VFS_OK);
    assert(vfs_rmdir(parent, "nested") == VFS_OK && vfs_rmdir(other, "moved") == VFS_OK);
    assert(vfs_rmdir(&root, "other") == VFS_OK);
    cleanup();

    /* An allocated source can contain a zero-allocation descendant. Its
     * cluster-zero mapping must not disable the ancestry proof. */
    parent = zero_directory(0);
    assert(vfs_create(parent, "existing", &child) == VFS_OK);
    exfat_node_t *state = parent->fs_data;
    u8 set[96], count;
    assert(exfat_build_file_set(&fs, "descendant", EXFAT_ATTR_DIRECTORY,
        0, 0, 0, false, set, &count));
    memcpy(media[exfat_cluster_lba(&fs, state->first_cluster)] + 96U, set, sizeof(set));
    vfs_node_t *descendant = exfat_finddir(parent, "descendant");
    assert(descendant && !descendant->size && exfat_ownership_preflight_locked(parent) == VFS_OK);
    before = writes;
    assert(exfat_rename_pinned(&super, &root, "empty", descendant, "cycle", &pin) ==
        VFS_ERR_INVALID_PARAM && !pin && writes == before && fs.version_confident);
    assert(vfs_rmdir(parent, "descendant") == VFS_OK && vfs_unlink(parent, "existing") == VFS_OK);
    assert(vfs_rmdir(&root, "empty") == VFS_OK);
    cleanup();
}

static void external_zero_image(const char *path, bool remove)
{
    setup(); device.sector_count = 12000;
    FILE *image = fopen(path, "r+b");
    assert(image && fread(media, sizeof(media), 1, image) == 1);
    exfat_fs_t *mounted = exfat_new_state(&device); assert(mounted);
    fs = *mounted; kfree(mounted); fs.next_inode = 1; fs.nodes = &root_state;
    root_state.first_cluster = fs.root_cluster;
    root_state.data_length = root_state.valid_data_length = root.size = fs.cluster_bytes;
    super.reference_count = 1;
    vfs_node_t *empty = exfat_finddir(&root, "empty"), *other = exfat_finddir(&root, "other");
    assert(empty && other && exfat_finddir(&root, "later.txt") &&
        exfat_ownership_preflight_locked(empty) == VFS_OK);
    char contents[700]; memset(contents, 'D', sizeof(contents));
    char name[256]; memset(name, 'M', 255); name[255] = 0;
    unsigned r = reads, w = writes, f = flushes;
    usize baseline = live_requested; peak_requested = baseline; clock_t start = clock();
    if (!remove) {
        assert(!empty->size && !other->size);
        vfs_dirent_t entry;
        assert(exfat_readdir_checked(empty, 0, &entry) == 0);
        vfs_node_t *child;
        assert(vfs_create(empty, "R\xc3\xa9sum\xc3\xa9.txt", &child) == VFS_OK);
        u64 bytes, pin;
        assert(exfat_node_write_pinned(child, 0, sizeof(contents), contents, &bytes, &pin) ==
            VFS_OK && bytes == sizeof(contents) && pin); finish_pin();
        assert(vfs_mkdir(other, "nested", &child) == VFS_OK);
        assert(vfs_create(child, "inside", &child) == VFS_OK);
        for (unsigned i = 0; i < 9; i++) {
            vfs_node_t *member; name[0] = 'a' + i;
            assert(vfs_create(empty, name, &member) == VFS_OK && exfat_finddir(empty, name) == member);
        }
        assert(empty->size > fs.cluster_bytes && exfat_ownership_preflight_locked(empty) == VFS_OK);
    } else {
        vfs_node_t *child = exfat_finddir(empty, "R\xc3\xa9sum\xc3\xa9.txt");
        char actual[700]; assert(child && exfat_node_read(child, 0, sizeof(actual), actual) == sizeof(actual) &&
            !memcmp(contents, actual, sizeof(actual)));
        assert(vfs_unlink(empty, "R\xc3\xa9sum\xc3\xa9.txt") == VFS_OK);
        for (unsigned i = 0; i < 9; i++) {
            name[0] = 'a' + i; assert(vfs_unlink(empty, name) == VFS_OK);
        }
        vfs_node_t *nested = exfat_finddir(other, "nested"); assert(nested);
        assert(vfs_unlink(nested, "inside") == VFS_OK && vfs_rmdir(other, "nested") == VFS_OK);
        assert(vfs_rmdir(&root, "empty") == VFS_OK && vfs_rmdir(&root, "other") == VFS_OK);
        assert(exfat_finddir(&root, "later.txt") && exfat_ownership_preflight_locked(&root) == VFS_OK);
    }
    printf("Independent exFAT zero-directory %s: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
        remove ? "remount/readback/removal" : "first allocations/growth",
        (size_t)(peak_requested-baseline), reads-r, writes-w, flushes-f,
        1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
    assert(exfat_sync(&super) == VFS_OK);
    rewind(image); assert(fwrite(media, sizeof(media), 1, image) == 1 &&
        fflush(image) == 0 && fclose(image) == 0);
    kfree(fs.upcase); fs.upcase = NULL; cleanup();
}

int main(int argc, char **argv)
{
    if (argc == 3) { external_zero_image(argv[2], !strcmp(argv[1], "--remove")); return 0; }
    zero_reads(); zero_create(); zero_faults(); zero_corruption_and_moves(); zero_fragmented_contents(); zero_maximum_contents();
    puts("exFAT zero-allocation directory production lifecycle passed");
    return 0;
}
