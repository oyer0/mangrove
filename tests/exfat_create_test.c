/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ordinary and trusted production creation, with controlled mounted I/O/pins. */
#define main exfat_identity_fixture_main
#include "exfat_mount_identity_test.c"
#undef main

static char maximum_name[256];

static void verify_created(vfs_node_t *parent, vfs_node_t *node, bool directory,
                            const char *name)
{
    exfat_node_t *state = node->fs_data;
    assert(node->super == &super && state->alive && state->version_valid &&
        state->generation == 1 && node->inode > parent->inode &&
        state->attributes == (directory ? EXFAT_ATTR_DIRECTORY : 0U) &&
        node->owner_uid == 7 && node->permissions == (directory ?
            VFS_DEFAULT_DIRECTORY_PERMISSIONS : VFS_DEFAULT_FILE_PERMISSIONS) &&
        node->size == (directory ? fs.cluster_bytes : 0U) &&
        exfat_finddir(parent, name) == node && !fs.replace_transaction && !pin_active);
    exfat_entry_info_t entry;
    assert(exfat_entry_set_read(&fs, state->parent_cluster,
        state->parent_no_fat_chain, state->entry_offset, &entry) &&
        entry.attributes == state->attributes && entry.data_length == node->size &&
        entry.valid_data_length == node->size && entry.first_cluster == state->first_cluster);
    void *cursor; vfs_dirent_t out; bool seen = false;
    assert(exfat_readdir_open(parent, &cursor));
    int result;
    while ((result = exfat_readdir_next_checked(cursor, &out)) != 0) {
        if (result == VFS_ERR_NAME_TOO_LONG) continue;
        assert(result == 1);
        if (!strcmp(name, out.name)) { seen = true; assert(out.inode == node->inode); }
    }
    exfat_readdir_close(cursor); assert(seen);
    if (directory) {
        u64 lba = exfat_cluster_lba(&fs, state->first_cluster);
        assert(state->no_fat_chain && state->first_cluster &&
            node->type == VFS_TYPE_DIRECTORY && exfat_directory_empty(&fs, &entry) == VFS_OK);
        for (u32 i = 0; i < fs.cluster_bytes; i++)
            assert(((u8 *)media[lba])[i] == 0);
    } else assert(node->type == VFS_TYPE_FILE && !state->first_cluster);
}

static void ordinary_layouts(void)
{
    for (unsigned directory = 0; directory < 2; directory++)
        for (unsigned layout = 0; layout <= 6; layout++) {
            vfs_node_t *existing, *parent = prepare_cross_create(layout, &existing), *created;
            usize baseline = live_requested; peak_requested = baseline;
            clock_t start = clock();
            assert(create_node_finished(parent, maximum_name, &created, directory) == VFS_OK);
            printf("exFAT ordinary %s layout=%u: peak=%zu reads=%u writes=%u flushes=%u "
                "allocations=%u cpu_ms=%.3f\n", directory ? "mkdir" : "create", layout,
                (size_t)(peak_requested - baseline), reads, writes, flushes, allocations,
                1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC);
            verify_created(parent, created, directory, maximum_name);
            assert(exfat_ownership_preflight_locked(created) == VFS_OK);
            unsigned before = writes;
            vfs_node_t *collision = (void *)1;
            char equivalent[256]; memset(equivalent, 'c', 255); equivalent[255] = 0;
            assert(create_node_finished(parent, equivalent, &collision, directory) ==
                VFS_ERR_ALREADY_EXISTS && !collision && before == writes);
            u64 pin;
            if (directory) {
                vfs_node_t *child;
                assert(create_finished(created, "child", &child) == VFS_OK);
                assert(exfat_rmdir_pinned(parent, maximum_name, &pin) == VFS_ERR_NOT_EMPTY);
                assert(exfat_unlink_pinned(created, "child", &pin) == VFS_OK && pin);
                finish_pin();
                assert(exfat_rmdir_pinned(parent, maximum_name, &pin) == VFS_OK && pin);
            } else {
                u64 bytes_written, write_pin;
                assert(exfat_node_write_pinned(created, 0, 5, (u8 *)"hello", &bytes_written, &write_pin) == VFS_OK &&
                    bytes_written == 5 && write_pin);
                finish_pin();
                char actual[5]; assert(exfat_node_read(created, 0, 5, actual) == 5 &&
                    !memcmp(actual, "hello", 5));
                assert(exfat_unlink_pinned(parent, maximum_name, &pin) == VFS_OK && pin);
            }
            finish_pin();
            assert(!((exfat_node_t *)created->fs_data)->alive &&
                !exfat_finddir(parent, maximum_name) && fs.version_confident);
            assert(create_node_finished(parent, maximum_name, &collision, directory) == VFS_OK &&
                collision->inode != created->inode);
            cleanup();
        }
}

static void ordinary_faults(void)
{
    unsigned injected = 0;
    for (unsigned directory = 0; directory < 2; directory++)
        for (unsigned layout = 0; layout <= 6; layout++) {
            vfs_node_t *existing, *parent = prepare_cross_create(layout, &existing), *node;
            assert(create_node_finished(parent, maximum_name, &node, directory) == VFS_OK);
            unsigned write_count = writes, flush_count = flushes, allocation_count = allocations;
            unsigned read_count = reads;
            cleanup();
            for (unsigned at = 1; at <= allocation_count; at++) {
                injected++;
                parent = prepare_cross_create(layout, &existing);
                usize baseline = live_requested; fail_allocation = at;
                assert(create_node_finished(parent, maximum_name, &node, directory) == VFS_ERR_NO_MEM &&
                    !node && !writes && !pin_active && live_requested == baseline);
                cleanup();
            }
            for (unsigned at = 1; at <= read_count; at++) {
                injected++;
                parent = prepare_cross_create(layout, &existing); fail_read_number = at;
                int result = create_node_finished(parent, maximum_name, &node, directory);
                assert(result != VFS_OK && !node);
                if (result == VFS_ERR_UNCERTAIN) {
                    assert(pin_active && pin_unresolved && !fs.version_confident);
                    discard_unresolved_fixture_pin();
                } else assert(result == VFS_ERR_IO && !pin_active && !fs.replace_transaction);
                cleanup();
            }
            for (unsigned at = 1; at <= write_count; at++)
                for (unsigned torn = 0; torn < 2; torn++) {
                    injected++;
                    parent = prepare_cross_create(layout, &existing);
                    fail_write_number = at; fail_write_copy_bytes = torn ? 128 : 0;
                    assert(create_node_finished(parent, maximum_name, &node, directory) ==
                        VFS_ERR_UNCERTAIN && !node && pin_active && pin_unresolved &&
                        !fs.version_confident && fs.replace_transaction && writes == at);
                    assert(exfat_create_pinned(parent, &(vfs_node_t){0}, "retry", &(u64){0}) ==
                        VFS_ERR_UNCERTAIN && writes == at);
                    discard_unresolved_fixture_pin(); cleanup();
                }
            for (unsigned at = 1; at <= flush_count; at++) {
                injected++;
                parent = prepare_cross_create(layout, &existing); fail_flush_number = at;
                assert(create_node_finished(parent, maximum_name, &node, directory) ==
                    VFS_ERR_UNCERTAIN && !node && pin_active && pin_unresolved && !fs.version_confident);
                discard_unresolved_fixture_pin(); cleanup();
            }
            for (unsigned at = 1; at <= write_count; at++) {
                injected++;
                parent = prepare_cross_create(layout, &existing); remove_after_write_number = at;
                assert(create_node_finished(parent, maximum_name, &node, directory) ==
                    VFS_ERR_UNCERTAIN && !node && pin_active && pin_unresolved && !fs.version_confident);
                discard_unresolved_fixture_pin(); cleanup();
            }
            parent = prepare_cross_create(layout, &existing); fail_pin_begin = true;
            usize baseline = live_requested;
            assert(create_node_finished(parent, maximum_name, &node, directory) == VFS_ERR_NO_SPACE &&
                !node && !writes && !pin_active && live_requested == baseline);
            cleanup();
        }
    printf("exFAT ordinary create/mkdir: %u allocation/read/write/flush/removal fault cases passed\n",
        injected);
}

static void ordinary_admission(void)
{
    for (unsigned directory = 0; directory < 2; directory++) {
        vfs_node_t *node;
        setup(); add_system_entries();
        /* Reserved END bytes are not an entry type. Imported valid tails
         * remain usable and unrelated entries stay intact. */
        for (u32 at = 160; at < 512; at += 32) memset(media[40] + at + 1, 0xa5, 31);
        u8 original[160]; memcpy(original, media[40], 160);
        assert(create_node_finished(&root, "new", &node, directory) == VFS_OK);
        assert(!memcmp(original, media[40], 160));
        verify_created(&root, node, directory, "new"); cleanup();
        setup(); add_system_entries();
        /* The legacy allocator could skip an earlier END and publish a live
         * but unreachable entry. No automatic repair of malformed tails. */
        media[40][192] = EXFAT_ENTRY_STREAM;
        u8 before[512]; memcpy(before, media[40], 512);
        assert(create_node_finished(&root, "new", &node, directory) == VFS_ERR_UNSUPPORTED &&
            !node && !writes && !pin_active && !memcmp(before, media[40], 512)); cleanup();
        setup(); add_system_entries();
        /* A candidate at 160 must not hide a malformed tail beyond its next
         * marker. Validate the complete allocation, not just proposed slots. */
        media[40][480] = EXFAT_ENTRY_STREAM;
        assert(create_node_finished(&root, "new", &node, directory) == VFS_ERR_UNSUPPORTED &&
            !node && !writes && !pin_active); cleanup();
        setup(); add_system_entries(); allow_write = false;
        assert(create_node_finished(&root, "new", &node, directory) == VFS_ERR_ACCESS_DENIED &&
            !node && !writes); cleanup();
        setup(); add_system_entries(); fs.read_only = true;
        assert(create_node_finished(&root, "new", &node, directory) == VFS_ERR_ACCESS_DENIED &&
            !node && !writes); cleanup();
        setup(); add_system_entries();
        add_entry(160, "shared", EXFAT_ATTR_DIRECTORY, 2, 512, false);
        assert(create_node_finished(&root, "new", &node, directory) == VFS_ERR_BAD_FORMAT &&
            !node && !writes && !pin_active); cleanup();
        setup(); add_system_entries();
        vfs_node_t *prepared = prepared_created_node(); u64 pin = 0;
        allow_write = false;
        assert((directory ? exfat_mkdir_owned_pinned : exfat_create_owned_pinned)
            (&root, prepared, "owned", 23, VFS_PERMISSION_OWNER_READ, &pin) == VFS_OK && pin);
        finish_pin(); vfs_node_register(prepared);
        assert(prepared->owner_uid == 23 && prepared->permissions == VFS_PERMISSION_OWNER_READ &&
            exfat_finddir(&root, "owned") == prepared); cleanup();
    }
    setup(); add_system_entries();
    memset(media[41], 0xff, 512); vfs_node_t *node;
    assert(mkdir_finished(&root, "no-space", &node) == VFS_ERR_NO_SPACE &&
        !node && !pin_active && !fs.replace_transaction && fs.version_confident &&
        !exfat_finddir(&root, "no-space")); cleanup();
    setup(); add_system_entries();
    memset(media[41], 0xff, 512); fail_flush_number = 2;
    assert(mkdir_finished(&root, "no-space", &node) == VFS_ERR_UNCERTAIN &&
        !node && pin_active && pin_unresolved && !fs.version_confident &&
        fs.replace_transaction->phase == EXFAT_REPLACE_CANCELLING);
    discard_unresolved_fixture_pin(); cleanup();
    assert(!exfat_node_ops.create && !exfat_node_ops.mkdir &&
        !exfat_node_ops.create_owned && !exfat_node_ops.mkdir_owned &&
        exfat_node_ops.create_pinned && exfat_node_ops.mkdir_pinned &&
        exfat_node_ops.create_owned_pinned && exfat_node_ops.mkdir_owned_pinned);
}

static void external_ordinary_image(const char *path, bool has_imported)
{
    setup(); device.sector_count = 12000;
    FILE *image = fopen(path, "r+b");
    assert(image && fread(media, sizeof(media), 1, image) == 1);
    /* Actual read-only mount on the same writable block callbacks. */
    {
        unsigned before_writes = writes;
        vfs_super_t *readonly;
        assert(exfat_mount(NULL, &device, &readonly, true) == VFS_OK && readonly->read_only);
        exfat_fs_t *readonly_fs = readonly->private_data;
        assert(readonly_fs->read_only && writes == before_writes);
        vfs_node_t prepared = {0}; u64 pin;
        assert(exfat_create_pinned(readonly->root_node, &prepared, "readonly-test", &pin) == VFS_ERR_ACCESS_DENIED && !pin);
        assert(exfat_mkdir_pinned(readonly->root_node, &prepared, "readonly-test", &pin) == VFS_ERR_ACCESS_DENIED && !pin);
        assert(writes == before_writes && !pin_active && readonly_fs->version_confident);
        vfs_node_t *readonly_root = readonly->root_node;
        assert(exfat_unmount(readonly) == VFS_OK && writes == before_writes);
        kfree(readonly_root); kfree(readonly);
    }
    exfat_fs_t *mounted = exfat_new_state(&device);
    assert(mounted && mounted->cluster_bytes == 4096);
    fs = *mounted; kfree(mounted); fs.next_inode = 1; fs.nodes = &root_state;
    root_state.first_cluster = fs.root_cluster;
    root_state.data_length = root_state.valid_data_length = root.size = fs.cluster_bytes;
    unsigned r = reads, w = writes, f = flushes;
    usize baseline = live_requested; peak_requested = baseline; clock_t start = clock();
    vfs_node_t *directory, *child;
    char imported_name[511];
    for (unsigned i = 0; i < 255; i++) memcpy(imported_name + i * 2, "\xc3\xa9", 2);
    imported_name[510] = 0;
    /* Native pathname queries retain their 255-byte boundary. Use the
     * production full-name parser to obtain this imported internal node. */
    vfs_node_t *imported = NULL;
    u64 imported_offset = 0; exfat_entry_info_t imported_entry; bool imported_error = false;
    while (exfat_directory_next(&fs, root_state.first_cluster, false,
        root_state.data_length, &imported_offset, &imported_entry, false, &imported_error))
        if (!strcmp(imported_entry.name, imported_name)) {
            imported = exfat_make_node(&root, &imported_entry); assert(imported); break;
        }
    assert(!imported_error && (imported != NULL) == has_imported);
    if (imported) {
        vfs_node_t *inside = exfat_finddir(imported, "inside.txt"); char original;
        assert(inside && exfat_node_read(inside, 0, 1, &original) == 1 && original == 'Z');
        assert(create_finished(imported, "ordinary imported child.txt", &child) == VFS_OK);
        verify_created(imported, child, false, "ordinary imported child.txt");
        assert(mkdir_finished(imported, "ordinary imported directory", &child) == VFS_OK);
        verify_created(imported, child, true, "ordinary imported directory");
        exfat_node_t *state = imported->fs_data;
        assert(exfat_entry_set_read(&fs, state->parent_cluster, state->parent_no_fat_chain,
            state->entry_offset, &imported_entry) && !strcmp(imported_entry.name, imported_name) &&
            exfat_finddir(imported, "inside.txt") == inside &&
            exfat_node_read(inside, 0, 1, &original) == 1 && original == 'Z');
    }
    assert(mkdir_finished(&root, maximum_name, &directory) == VFS_OK);
    verify_created(&root, directory, true, maximum_name);
    exfat_node_t *dir = directory->fs_data;
    /* Adjacent child-directory allocations force the parent's conversion
     * when enough long entries exhaust its initial contiguous capacity. */
    for (unsigned i = 0; i < 8; i++) {
        char name[256]; memcpy(name, maximum_name, 256); name[0] = 'a' + i;
        assert(mkdir_finished(directory, name, &child) == VFS_OK);
        verify_created(directory, child, true, name);
        assert(exfat_finddir(directory, name) == child);
    }
    assert(!dir->no_fat_chain && dir->data_length > fs.cluster_bytes);
    assert(create_finished(directory, "R\xc3\xa9sum\xc3\xa9.txt", &child) == VFS_OK);
    vfs_node_t *collision = (void *)1; unsigned before = writes;
    assert(create_finished(directory, "R\xc3\x89SUM\xc3\x89.TXT", &collision) ==
        VFS_ERR_ALREADY_EXISTS && !collision && writes == before);
    u64 bytes_written, write_pin;
    assert(exfat_node_write_pinned(child, 0, 8, (u8 *)"readback", &bytes_written, &write_pin) == VFS_OK &&
        bytes_written == 8 && write_pin); finish_pin();
    char bytes[8]; assert(exfat_node_read(child, 0, 8, bytes) == 8 && !memcmp(bytes, "readback", 8));
    u64 pin;
    assert(exfat_rename_pinned(&super, directory, "R\xc3\xa9sum\xc3\xa9.txt", directory,
        "renamed", &pin) == VFS_OK && pin); finish_pin();
    assert(exfat_unlink_pinned(directory, "renamed", &pin) == VFS_OK && pin); finish_pin();
    assert(mkdir_finished(directory, "remove-me", &child) == VFS_OK);
    assert(exfat_rmdir_pinned(directory, "remove-me", &pin) == VFS_OK && pin); finish_pin();
    assert(exfat_ownership_preflight_locked(directory) == VFS_OK);
    printf("exFAT external ordinary create/mkdir/growth/lifecycle: peak=%zu reads=%u writes=%u "
        "flushes=%u cpu_ms=%.3f\n", (size_t)(peak_requested-baseline), reads-r, writes-w,
        flushes-f, 1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
    assert(!(exfat_le16(media[0] + 106U) & 2U) && block_flush_for_id(&device, fs.device_id));
    rewind(image); assert(fwrite(media, sizeof(media), 1, image) == 1 &&
        fflush(image) == 0 && fclose(image) == 0);
    kfree(fs.upcase); fs.upcase = NULL; cleanup();
}

int main(int argc, char **argv)
{
    memset(maximum_name, 'C', 255); maximum_name[255] = 0;
    if (argc == 3 && !strcmp(argv[1], "--imported-image")) {
        external_ordinary_image(argv[2], true); return 0;
    }
    if (argc == 2) { external_ordinary_image(argv[1], false); return 0; }
    ordinary_admission(); ordinary_layouts(); ordinary_faults();
    maximum_namespace_inventory_regression(true, true);
    puts("exFAT ordinary create/mkdir production regressions passed"); return 0;
}
