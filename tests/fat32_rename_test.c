/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production rename, growth and retirement with controlled mounted I/O. */
#define main fat32_identity_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

static char maximum_name[256], changed_maximum[256], unicode_name[256];
static vfs_node_t *source_parent, *destination_parent, *moved, *child;
static const char *old_name, *new_name;
static u32 original_first, original_fat;
static u64 original_generation;
static usize baseline;
static fat32_on_disk_entry_t original_entry;
static unsigned written_sectors;
static bool count_write_sectors(block_device_t *dev, u64 id, u64 lba,
                                 u32 count, const void *buffer)
{
    written_sectors += count;
    scaling_write = NULL;
    bool result = block_write_for_id(dev, id, lba, count, buffer);
    scaling_write = count_write_sectors;
    return result;
}

static void names(void)
{
    memset(maximum_name, 'a', 255U); maximum_name[255] = 0U;
    memcpy(changed_maximum, maximum_name, 256U); changed_maximum[0] = 'A';
    for (u32 i = 0U; i < 252U; i += 2U) {
        unicode_name[i] = (char)0xc3U; unicode_name[i + 1U] = (char)0xa9U;
    }
    memcpy(unicode_name + 252U, "dir", 4U);
}

static void prepare(unsigned kind)
{
    setup(); fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
    entry(0U, "KEEP    TXT", 3U, 3U); memcpy(media[11], "old", 3U);
    source_parent = destination_parent = &root; child = NULL;
    old_name = "OLD.TXT"; new_name = "NEW.TXT";
    if (kind == 1U || kind == 7U) old_name = maximum_name;
    if (kind == 2U || kind >= 3U) new_name = maximum_name;
    if (kind == 7U) new_name = changed_maximum;
    if (kind == 8U) new_name = unicode_name;
    if (kind >= 3U && kind != 7U) {
        assert(mkdir_finished(&root, "SOURCE", &source_parent) == VFS_OK);
        assert(mkdir_finished(&root, "DEST", &destination_parent) == VFS_OK);
        if (kind == 3U || kind == 4U) old_name = maximum_name;
    }
    if (kind == 4U || kind == 5U || kind == 8U) {
        assert(mkdir_finished(source_parent, old_name, &moved) == VFS_OK);
        assert(create_finished(moved, "CHILD.TXT", &child) == VFS_OK);
        assert(write_finished(child, 0U, 3U, "abc") == 3U);
    } else {
        assert(create_finished(source_parent, old_name, &moved) == VFS_OK);
        assert(write_finished(moved, 0U, 3U, "abc") == 3U);
    }
    if (kind == 3U || kind == 4U || kind == 6U) {
        /* Full fragmented destination directory. Growth needs two clusters
         * for a 21-slot name; allocation shares mirrored FAT sectors. */
        u32 first = fat32_state(destination_parent)->first_cluster;
        for (u32 i = 2U; i < 16U; i++) {
            char name[12]; snprintf(name, sizeof(name), "F%02u     TXT", i);
            entry_at(first, i, name, 0U, 0U, 0x20U);
        }
        fs.ownership_proven = false;
    }
    if (kind == 5U) {
        /* The end marker is in an earlier cluster, not the chain tail. */
        u32 first = fat32_state(destination_parent)->first_cluster;
        ((u32 *)media[1])[first] = 31U;
        ((u32 *)media[1])[31U] = FAT32_EOC_MIN;
        memcpy(media[2], media[1], 512U);
        fs.ownership_proven = false;
    }
    fs.fs_info_sector = 3U; fs.backup_boot_sector = 1U; fs.reserved_sector_count = 6U;
    for (u32 i = 3U; i <= 4U; i++) {
        fat32_store_le32(media[i], 0x41615252U);
        fat32_store_le32(media[i] + 484U, 0x61417272U);
        fat32_store_le32(media[i] + 508U, 0xaa550000U);
    }
    fat32_node_state_t *state = fat32_state(moved);
    fat32_on_disk_entry_t *disk = (void *)(media[fat32_cluster_to_lba(&fs, state->parent_cluster)] +
                                          state->entry_index * 32U);
    disk->nt_reserved = 0x80U; disk->attr |= 2U;
    disk->create_time_tenths = 37U; disk->create_time = 0x1234U; disk->create_date = 0x574dU;
    disk->last_access_date = 0x574dU; disk->write_time = 0x2345U; disk->write_date = 0x574dU;
    original_entry = *disk;
    original_first = fat32_state(moved)->first_cluster;
    original_fat = ((u32 *)media[1])[original_first];
    original_generation = fat32_state(moved)->generation;
    reads = writes = flushes = allocations = 0U;
    baseline = live_requested; peak_requested = baseline;
    written_sectors = 0U; scaling_write = count_write_sectors; poison_allocations = true;
}

static void verify_success(void)
{
    assert(fat32_finddir(destination_parent, new_name) == moved);
    /* Case-equivalent names intentionally continue to resolve. */
    if (strcmp(old_name, new_name) && new_name != changed_maximum)
        assert(!fat32_finddir(source_parent, old_name));
    assert(fat32_state(moved)->first_cluster == original_first &&
           ((u32 *)media[1])[original_first] == original_fat &&
           ((u32 *)media[2])[original_first] == original_fat &&
           fat32_state(moved)->generation == original_generation + 1U);
    fat32_walk_entry_t renamed;
    assert(fat32_dir_find(destination_parent, new_name, &renamed) == VFS_OK);
    original_entry.nt_reserved &= (u8)~0x18U;
    assert(!memcmp((u8 *)&renamed.disk + 11U, (u8 *)&original_entry + 11U, 21U));
    assert(fat32_ownership_preflight_locked(moved) == VFS_OK);
    char output[3];
    vfs_node_t *read_node = child ? child : moved;
    assert(fat32_read(read_node, 0U, 3U, output) == 3U && !memcmp(output, "abc", 3U));
    if (child) {
        assert(fat32_finddir(moved, "CHILD.TXT") == child);
        const fat32_on_disk_entry_t *dot = (void *)media[fat32_cluster_to_lba(&fs, original_first)];
        u32 expected = fat32_state(destination_parent)->first_cluster;
        assert(fat32_entry_first(&dot[1]) == (expected == fs.root_cluster ? 0U : expected));
        assert(rmdir_finished(destination_parent, new_name) == VFS_ERR_NOT_EMPTY);
        assert(unlink_finished(moved, "CHILD.TXT") == VFS_OK);
        assert(rmdir_finished(destination_parent, new_name) == VFS_OK);
    } else {
        vfs_file_facts_t facts;
        assert(fat32_file_facts(moved, &facts) == VFS_OK);
        u64 pin = 0U; replace_source_state_t source = {.short_at = ~(u64)0};
        assert(fat32_replace_locked(&super, moved->inode, original_generation,
            3U, replace_source, &source, &pin) == VFS_ERR_STALE_GENERATION);
        assert(write_finished(moved, 3U, 1U, "!") == 1U);
        assert(unlink_finished(destination_parent, new_name) == VFS_OK);
    }
    assert(!fat32_state(moved)->alive);
    vfs_node_t *reused = NULL;
    assert(create_finished(source_parent, old_name, &reused) == VFS_OK && reused != moved);
    assert(unlink_finished(source_parent, old_name) == VFS_OK);
    cleanup(); assert(!live_requested);
}

static void successes(void)
{
    for (unsigned kind = 0U; kind < 9U; kind++) {
        prepare(kind); u64 pin = 0U; clock_t start = clock();
        int result = fat32_rename_pinned(&super, source_parent, old_name,
            destination_parent, new_name, &pin);
        if (result != VFS_OK) fprintf(stderr, "rename layout %u: %d\n", kind, result);
        assert(result == VFS_OK && pin && fs.replace_transaction && !fs.version_confident);
        usize retained = live_requested - baseline;
        assert(!fat32_finddir(source_parent, old_name) && !fat32_finddir(destination_parent, new_name));
        printf("FAT32 rename layout %u: peak=%zu retained=%zu allocations=%u reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
            kind, (size_t)(peak_requested - baseline), (size_t)retained, allocations,
            reads, writes, flushes, 1000.0 * (clock() - start) / CLOCKS_PER_SEC);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && live_requested == baseline);
        verify_success();
    }
}

static unsigned fault_cases;
static void failures(void)
{
    for (unsigned kind = 0U; kind < 9U; kind++) {
        prepare(kind);
        assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_OK);
        unsigned all_writes = writes, all_flushes = flushes, all_allocations = allocations, all_reads = reads;
        cleanup(); assert(!live_requested);
        for (unsigned mode = 0U; mode < 7U; mode++) {
            unsigned bound = mode < 2U || mode == 6U ? all_writes : mode == 2U ? all_flushes :
                             mode == 3U ? all_allocations : all_reads;
            for (unsigned at = 1U; at <= bound; at++) {
                prepare(kind);
                if (mode == 0U) { fail_write_at = at; fail_write_partial_bytes = (at & 1U) ? 17U : 511U; }
                if (mode == 1U) remove_after_writes = at;
                if (mode == 2U) fail_flush_at = at;
                if (mode == 3U) fail_allocation = at;
                if (mode == 4U) remove_after_reads = at;
                if (mode == 5U) replace_after_reads = at;
                if (mode == 6U) { fail_write_at = at; fail_write_definite = true; }
                int result = rename_finished(source_parent, old_name, destination_parent, new_name);
                if (result == VFS_ERR_UNCERTAIN) {
                    assert(result == VFS_ERR_UNCERTAIN && active_pin && pin_unresolved &&
                        !fs.version_confident && fs.namespace_hidden && fs.replace_transaction);
                    assert(((u32 *)media[1])[original_first] == original_fat &&
                           ((u32 *)media[2])[original_first] == original_fat);
                    unsigned attempts = writes;
                    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_UNCERTAIN);
                    assert(writes == attempts);
                    assert(!fat32_finddir(destination_parent, new_name));
                } else {
                    assert(result != VFS_OK && !active_pin && fs.version_confident &&
                           (!online || device.id != fs.device_id ||
                            fat32_finddir(source_parent, old_name) == moved));
                }
                cleanup(); assert(!live_requested); fault_cases++;
            }
        }
    }
    printf("FAT32 rename injected boundary cases: %u\n", fault_cases);
}

static void refusals(void)
{
    prepare(4U);
    unsigned before = writes; usize heap = live_requested;
    assert(rename_finished(source_parent, old_name, moved, "BAD") == VFS_ERR_INVALID_PARAM);
    assert(writes == before && live_requested == heap && !active_pin);
    vfs_node_t *descendant = NULL;
    assert(mkdir_finished(moved, "NESTED", &descendant) == VFS_OK);
    before = writes;
    assert(rename_finished(source_parent, old_name, descendant, "BAD") == VFS_ERR_INVALID_PARAM && writes == before);
    assert(rename_finished(source_parent, old_name, destination_parent, "bad.") == VFS_ERR_INVALID_PARAM && writes == before);
    assert(fat32_rename(source_parent, old_name, destination_parent, new_name) == VFS_ERR_UNSUPPORTED && writes == before);
    fail_pin_begin = true;
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_NO_SPACE && writes == before && !active_pin);
    fail_pin_begin = false;
    vfs_node_t *collision = NULL;
    assert(create_finished(destination_parent, new_name, &collision) == VFS_OK);
    before = writes;
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_ALREADY_EXISTS && writes == before);
    cleanup(); assert(!live_requested);

    prepare(0U); before = writes;
    assert(rename_finished(&root, old_name, &root, "OLD.TXT") == VFS_OK && writes == before &&
        fat32_state(moved)->generation == original_generation);
    assert(rename_finished(&root, old_name, &root, "old.txt") == VFS_OK && fat32_finddir(&root, "OLD.TXT") == moved);
    fat32_walk_entry_t match;
    assert(fat32_dir_find(&root, "old.txt", &match) == VFS_OK && match.has_lfn);
    char display[256]; assert(fat32_lfn_encode(&match.lfn, &match.disk, display, sizeof(display)) && !strcmp(display, "old.txt"));
    cleanup(); assert(!live_requested);

    prepare(0U);
    entry(2U, "SHARED  TXT", original_first, 3U); fs.ownership_proven = false;
    assert(rename_finished(&root, old_name, &root, new_name) == VFS_ERR_BAD_FORMAT && !writes && !active_pin);
    cleanup(); assert(!live_requested);
}

static bool fail_retirement_read(block_device_t *dev, u64 id, u64 lba,
                                  u32 count, void *buffer)
{
    if (writes && lba == 10U) return false;
    scaling_read = NULL;
    bool result = block_read_for_id(dev, id, lba, count, buffer);
    scaling_read = fail_retirement_read;
    return result;
}

static void cancellation_and_namespace(void)
{
    prepare(2U);
    scaling_read = fail_retirement_read;
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_UNCERTAIN);
    assert(fs.replace_transaction->rename->destination_confirmed &&
           !fs.replace_transaction->rename->source_retired && active_pin && pin_unresolved);
    cleanup(); assert(!live_requested);

    prepare(3U);
    for (u32 i = 2U; i < fs.total_clusters; i++)
        if (!((u32 *)media[1])[i]) ((u32 *)media[1])[i] = FAT32_EOC_MIN;
    memcpy(media[2], media[1], 512U);
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_NO_SPACE &&
           !writes && !active_pin && fs.version_confident && fat32_finddir(source_parent, old_name) == moved);
    cleanup(); assert(!live_requested);

    prepare(3U);
    for (u32 i = 2U; i < fs.total_clusters; i++)
        if (!((u32 *)media[1])[i] && i != 27U) ((u32 *)media[1])[i] = FAT32_EOC_MIN;
    memcpy(media[2], media[1], 512U);
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_UNCERTAIN);
    assert(fs.replace_transaction->growth->links_confirmed == 1U &&
           fs.replace_transaction->growth->candidate == 27U &&
           !fs.replace_transaction->rename->destination_attempted &&
           ((u32 *)media[1])[27U] == FAT32_EOC_MIN && ((u32 *)media[2])[27U] == FAT32_EOC_MIN);
    cleanup(); assert(!live_requested);

    prepare(3U);
    u32 candidate = fs.next_free_cluster;
    while (((u32 *)media[1])[candidate]) candidate++;
    fail_fat_read_after_writes = 3U;
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_IO);
    assert(!active_pin && fs.version_confident && !((u32 *)media[1])[candidate] &&
           !((u32 *)media[2])[candidate] && fat32_finddir(source_parent, old_name) == moved);
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_OK);
    verify_success();

    prepare(3U); fail_fat_read_after_writes = 3U; fail_write_at = 4U;
    assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_UNCERTAIN &&
           active_pin && pin_unresolved && !fs.replace_transaction->rename->destination_attempted);
    cleanup(); assert(!live_requested);

    prepare(0U);
    entry(2U, "old     txt", 0U, 0U);
    assert(rename_finished(&root, old_name, &root, new_name) == VFS_ERR_BAD_FORMAT && !writes && !active_pin);
    cleanup(); assert(!live_requested);

    prepare(0U);
    entry(2U, "OEM     TXT", 0U, 0U); ((u8 *)media[10U])[64U] = 0x82U;
    assert(rename_finished(&root, old_name, &root, new_name) == VFS_ERR_UNSUPPORTED &&
           !writes && !active_pin && fs.version_confident);
    cleanup(); assert(!live_requested);

    prepare(0U);
    entry(2U, "NEW     TXT", 0U, 0U);
    assert(rename_finished(&root, old_name, &root, "new.txt") == VFS_ERR_ALREADY_EXISTS && !writes);
    cleanup(); assert(!live_requested);

    prepare(0U);
    fat32_node_state_t *state = fat32_state(moved);
    fat32_on_disk_entry_t *readonly = (void *)(media[fat32_cluster_to_lba(&fs, state->parent_cluster)] +
                                              state->entry_index * 32U);
    readonly->attr |= 1U;
    assert(rename_finished(&root, old_name, &root, new_name) == VFS_OK);
    fat32_walk_entry_t renamed;
    assert(fat32_dir_find(&root, new_name, &renamed) == VFS_OK && (renamed.disk.attr & 1U));
    u64 written = 0U, pin = 0U;
    assert(fat32_write_pinned(moved, 0U, 1U, "x", &written, &pin) == VFS_ERR_ACCESS_DENIED && !pin && !written);
    cleanup(); assert(!live_requested);

    prepare(0U);
    ((u32 *)media[2])[original_first] = 0U;
    assert(rename_finished(&root, old_name, &root, new_name) == VFS_ERR_BAD_FORMAT && !writes && !active_pin);
    cleanup(); assert(!live_requested);
}

static vfs_super_t detached_super;
static bool detach_during_write(block_device_t *dev, u64 id, u64 lba,
                                 u32 count, const void *buffer)
{
    scaling_write = NULL;
    bool result = block_write_for_id(dev, id, lba, count, buffer);
    for (fat32_node_state_t *state = fs.nodes; state; state = state->next)
        state->node->super = &detached_super;
    return result;
}

static void configure_copies(bool active)
{
    fs.fat_count = 4U;
    fs.fs_info_sector = fs.backup_boot_sector = 0U;
    memcpy(media[3], media[1], 512U); memcpy(media[4], media[1], 512U);
    if (active) {
        fs.bpb.ext_flags = 0x82U;
        ((u32 *)media[1])[original_first] = 0U;
        ((u32 *)media[2])[original_first] = 0U;
        ((u32 *)media[4])[original_first] = 0U;
    }
}

static void mirrored_and_active(void)
{
    for (u32 active = 0U; active < 2U; active++) {
        prepare(4U);
        configure_copies(active);
        assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_OK &&
               fat32_finddir(destination_parent, new_name) == moved &&
               fat32_ownership_preflight_locked(moved) == VFS_OK);
        assert(((u32 *)media[3])[original_first] == original_fat);
        if (active) assert(!((u32 *)media[1])[original_first] &&
                           !((u32 *)media[2])[original_first] && !((u32 *)media[4])[original_first]);
        else assert(!memcmp(media[1], media[2], 512U) &&
                    !memcmp(media[1], media[3], 512U) && !memcmp(media[1], media[4], 512U));
        printf("FAT32 rename FAT mode %u: reads=%u writes=%u sectors_written=%u flushes=%u\n",
               active, reads, writes, written_sectors, flushes);
        unsigned count_writes = writes, count_flushes = flushes;
        cleanup(); assert(!live_requested);
        for (u32 mode = 0U; mode < 2U; mode++) {
            unsigned bound = mode ? count_flushes : count_writes;
            for (unsigned at = 1U; at <= bound; at++) {
                prepare(4U); configure_copies(active);
                if (mode) fail_flush_at = at;
                else { fail_write_at = at; fail_write_partial_bytes = 511U; }
                assert(rename_finished(source_parent, old_name, destination_parent, new_name) == VFS_ERR_UNCERTAIN &&
                       active_pin && pin_unresolved && fs.replace_transaction && !fs.version_confident);
                assert(((u32 *)media[3])[original_first] == original_fat);
                cleanup(); assert(!live_requested);
            }
        }
    }
}

static void lifetime_and_large(void)
{
    prepare(4U);
    scaling_write = detach_during_write;
    u64 pin = 0U;
    assert(fat32_rename_pinned(&super, source_parent, old_name, destination_parent,
                              new_name, &pin) == VFS_OK && pin && fs.replace_transaction);
    assert(source_parent->super == &detached_super && destination_parent->super == &detached_super);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    for (fat32_node_state_t *state = fs.nodes; state; state = state->next)
        state->node->super = &super;
    verify_success();

    prepare(2U);
    assert(fat32_rename_pinned(&super, source_parent, old_name, destination_parent,
                              new_name, &pin) == VFS_OK && pin);
    fail_pin_finish = true;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY && fs.replace_transaction && !fs.version_confident);
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    verify_success();

    /* No rename-specific contents or allocation ceiling. A fragmented file
     * larger than safe-save's bound is moved without inventorying its chain. */
    setup(); fs.total_clusters = 8192U; fs.fat_count = 2U; fs.fat_size_sectors = 64U;
    fs.data_start_lba = 160U; fs.next_free_cluster = 4U;
    memset(media[1], 0, 128U * 512U);
    ((u32 *)media[1])[2U] = FAT32_EOC_MIN;
    ((u32 *)media[1])[3U] = FAT32_EOC_MIN;
    const u32 count = 6145U;
    for (u32 i = 0U; i < count; i++) {
        u32 current = 100U + i * 17U % count;
        u32 next = i + 1U == count ? FAT32_EOC_MIN : 100U + (i + 1U) * 17U % count;
        ((u32 *)media[1])[current] = next;
    }
    memcpy(media[65], media[1], 64U * 512U);
    fat32_on_disk_entry_t *root_entries = (void *)media[160U];
    memset(root_entries, 0, 512U);
    memcpy(root_entries[0].name, "LARGE   BIN", 11U);
    root_entries[0].attr = 0x20U; root_entries[0].first_cluster_low = 100U;
    root_entries[0].file_size = 3U * 1024U * 1024U + 17U;
    memcpy(media[258U], "abc", 3U);
    vfs_node_t *file = fat32_finddir(&root, "LARGE.BIN"), *destination = NULL;
    assert(file && mkdir_finished(&root, "DEST", &destination) == VFS_OK);
    usize initial = live_requested; peak_requested = initial; reads = writes = flushes = 0U;
    clock_t start = clock();
    assert(rename_finished(&root, "LARGE.BIN", destination, maximum_name) == VFS_OK &&
           fat32_finddir(destination, maximum_name) == file && !fat32_finddir(&root, "LARGE.BIN"));
    assert(file->size == 3U * 1024U * 1024U + 17U && fat32_state(file)->first_cluster == 100U);
    assert(fat32_ownership_preflight_locked(file) == VFS_OK);
    printf("FAT32 fragmented 3 MiB rename: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
        (size_t)(peak_requested - initial), reads, writes, flushes,
        1000.0 * (clock() - start) / CLOCKS_PER_SEC);
    assert(unlink_finished(destination, maximum_name) == VFS_OK);
    cleanup(); assert(!live_requested);

    /* Maximum cluster-size scratch is temporary, never retained by a pin. */
    setup(); written_sectors = 0U; scaling_write = count_write_sectors;
    fs.sectors_per_cluster = 128U; fs.cluster_size_bytes = 65536U;
    fs.fat_count = 2U; memcpy(media[2], media[1], 512U);
    fat32_on_disk_entry_t *entries = (void *)media[10U];
    for (u32 i = 0U; i < 2048U; i++) {
        char short_name[12]; snprintf(short_name, sizeof(short_name), "F%07uTXT", i);
        memcpy(entries[i].name, short_name, 11U); entries[i].attr = 0x20U;
    }
    memcpy(entries[0].name, "OLD     TXT", 11U);
    vfs_node_t *empty = fat32_finddir(&root, "OLD.TXT"); assert(empty);
    initial = live_requested; peak_requested = initial; reads = writes = flushes = 0U;
    start = clock();
    assert(fat32_rename_pinned(&super, &root, "OLD.TXT", &root, maximum_name, &pin) == VFS_OK && pin);
    printf("FAT32 64 KiB-cluster rename: peak=%zu retained=%zu reads=%u writes=%u sectors_written=%u flushes=%u cpu_ms=%.3f\n",
        (size_t)(peak_requested - initial), (size_t)(live_requested - initial), reads, writes, written_sectors, flushes,
        1000.0 * (clock() - start) / CLOCKS_PER_SEC);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK && fat32_finddir(&root, maximum_name) == empty);
    assert(fat32_ownership_preflight_locked(empty) == VFS_OK);
    cleanup(); assert(!live_requested);
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
    vfs_node_t *src = fat32_finddir(sb->root_node, "SOURCE");
    vfs_node_t *dst = fat32_finddir(sb->root_node, "DEST");
    vfs_node_t *file = fat32_finddir(src, maximum_name);
    vfs_node_t *folder = fat32_finddir(src, "Imported long directory name");
    vfs_node_t *keep = fat32_finddir(sb->root_node, "KEEP.TXT");
    assert(src && dst && file && folder && keep);
    u64 inode = file->inode, generation = fat32_state(file)->generation;
    usize baseline_image = live_requested; peak_requested = baseline_image;
    unsigned old_reads = reads, old_writes = writes, old_flushes = flushes;
    clock_t start = clock();
    u64 pin = 0U;
    assert(fat32_rename_pinned(sb, src, maximum_name, dst, changed_maximum, &pin) == VFS_OK && pin);
    fat32_replace_transaction_t *tx = ((fat32_fs_t *)sb->private_data)->replace_transaction;
    assert(tx->lfn_creation && tx->lfn_creation->count >= 2U && tx->growth && tx->growth->append);
    printf("Independent FAT32 rename: destination sectors=%u appended clusters=%u retained=%zu\n",
           tx->lfn_creation->count, tx->growth->append_count,
           (size_t)(live_requested - baseline_image));
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fat32_finddir(dst, changed_maximum) == file && file->inode == inode &&
           fat32_state(file)->generation == generation + 1U && !fat32_finddir(src, maximum_name));
    char output[32];
    assert(fat32_read(file, 0U, 15U, output) == 15U && !memcmp(output, "external-bytes\n", 15U));
    assert(rename_finished(src, "Imported long directory name", dst, unicode_name) == VFS_OK);
    assert(fat32_finddir(dst, unicode_name) == folder && !fat32_finddir(src, "Imported long directory name"));
    vfs_node_t *child_file = fat32_finddir(folder, "CHILD.TXT");
    assert(child_file && fat32_read(child_file, 0U, 15U, output) == 15U && !memcmp(output, "external-bytes\n", 15U));
    assert(fat32_ownership_preflight_locked(folder) == VFS_OK);
    assert(rename_finished(dst, unicode_name, sb->root_node, "Moved Unicode directory \xc3\xa9") == VFS_OK);
    assert(fat32_finddir(sb->root_node, "Moved Unicode directory \xc3\xa9") == folder);
    assert(create_finished(folder, "New Unicode file \xc3\xa9", &child_file) == VFS_OK);
    assert(write_finished(child_file, 0U, 14U, "native-growth\n") == 14U);
    assert(rename_finished(folder, "New Unicode file \xc3\xa9", dst, "NATIVE.TXT") == VFS_OK);
    assert(create_finished(src, maximum_name, &child_file) == VFS_OK);
    assert(unlink_finished(src, maximum_name) == VFS_OK);
    assert(fat32_ownership_preflight_locked(file) == VFS_OK);
    assert(fat32_read(keep, 0U, 15U, output) == 15U && !memcmp(output, "external-bytes\n", 15U));
    printf("Independent FAT32 rename lifecycle: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline_image), reads - old_reads, writes - old_writes,
           flushes - old_flushes, 1000.0 * (clock() - start) / CLOCKS_PER_SEC);
    fat32_fs_t *mounted = sb->private_data;
    vfs_node_t *nodes[256]; unsigned count = 0U;
    for (fat32_node_state_t *state = mounted->nodes; state; state = state->next) {
        assert(count < 256U); nodes[count++] = state->node;
    }
    assert(fat32_unmount(sb) == VFS_OK);
    for (unsigned i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(sb); fclose(image_file); image_file = NULL; pin_super = &super;
    assert(!live_requested);
}

int main(int argc, char **argv)
{
    names();
    if (argc == 3 && !strcmp(argv[1], "--image")) { image_case(argv[2]); return 0; } successes(); failures(); refusals(); cancellation_and_namespace(); mirrored_and_active(); lifetime_and_large();
    printf("FAT32 rename structures: common=%zu rename=%zu retirement=%zu LFN=%zu growth=%zu second=%zu\n",
        sizeof(fat32_replace_transaction_t), sizeof(fat32_rename_state_t), sizeof(fat32_lfn_retirement_t),
        sizeof(fat32_lfn_creation_t), sizeof(fat32_directory_growth_t), sizeof(fat32_growth_second_images_t));
    puts("FAT32 production rename, directory ancestry, shared sectors and recovery passed");
    return 0;
}
