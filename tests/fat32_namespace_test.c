/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual FAT32 functions; shared fixture mocks block I/O, heap and VFS pins. */
#define main fat32_identity_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

/* Build raw imported entries from independently specified UTF-16 vectors.
 * This uses the FAT specification's checksum/ordinal algorithm, not the
 * driver's collector, decoder or a future creation builder. */
static void import_lfn(const u16 *name, u32 length, const char alias[11])
{
    u32 count = (length + 12U) / 13U;
    assert(count && count <= 20U);
    u8 checksum = 0;
    for (u32 i = 0; i < 11U; i++)
        checksum = (u8)((checksum >> 1U) | (checksum << 7U)) + (u8)alias[i];
    if (count >= 16U) {
        ((u32 *)media[1])[2] = 4U;
        ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    }
    for (u32 slot = 0; slot < count; slot++) {
        u32 sequence = count - slot;
        u16 words[13];
        for (u32 i = 0; i < 13U; i++) {
            u32 index = (sequence - 1U) * 13U + i;
            words[i] = index < length ? name[index] :
                       index == length ? 0U : 0xffffU;
        }
        fat32_lfn_entry_t raw = {0};
        raw.order = (u8)sequence | (slot == 0U ? 0x40U : 0U);
        raw.attr = 0x0fU;
        raw.checksum = checksum;
        memcpy(raw.name1, words, 10U);
        memcpy(raw.name2, words + 5U, 12U);
        memcpy(raw.name3, words + 11U, 4U);
        u32 lba = slot < 16U ? 10U : 12U;
        memcpy(media[lba] + (slot % 16U) * 32U, &raw, sizeof(raw));
    }
    entry_at(count < 16U ? 2U : 4U, count % 16U, alias, 0U, 0U, 0x20U);
}

static void comparison_vectors(void)
{
    /* UnicodeData 15.1 field 12: Latin, Greek, Cyrillic, Armenian,
     * Georgian, titlecase, compatibility and supplementary-plane mappings. */
    static const struct { const char *a, *b; bool equal; } vectors[] = {
        {"Report.txt", "REPORT.TXT", true},
        {"é.txt", "É.TXT", true},
        {"σς.txt", "ΣΣ.TXT", true},
        {"я.txt", "Я.TXT", true},
        {"ա.txt", "Ա.TXT", true},
        {"ა.txt", "Ა.TXT", true},
        {"ǆǅ.txt", "ǄǄ.TXT", true},
        {"µ.txt", "Μ.TXT", true},
        {"ı.txt", "I.TXT", true},
        {"𐐨.txt", "𐐀.TXT", true},
        {"\xcd\x85.txt", "Ι.TXT", true}, /* U+0345 -> U+0399 */
        {"ß.txt", "SS.TXT", false}, /* no multi-character expansion */
        {"ﬃ.txt", "FFI.TXT", false},
        {"İ.txt", "i.txt", false}, /* no locale-dependent Turkish casing */
        {"K.txt", "K.txt", false}, /* simple uppercase differs from casefold */
        {"é.txt", "e\xcc\x81.txt", false}, /* no normalization */
        {"  report.txt. ", "REPORT.TXT", true},
        {"a b.txt", "ab.txt", false},
        {"中.txt", "中.TXT", true},
        /* Garay casing was added after the pinned release: identity here. */
        {"\xf0\x90\xb5\xb0", "\xf0\x90\xb5\x90", false},
    };
    usize heap = live_requested;
    unsigned allocation_count = allocations;
    for (u32 repeat = 0; repeat < 100U; repeat++) {
        for (u32 i = 0; i < sizeof(vectors) / sizeof(*vectors); i++) {
            assert(fat32_match_text(vectors[i].a, vectors[i].b) == vectors[i].equal);
            assert(fat32_match_text(vectors[i].b, vectors[i].a) == vectors[i].equal);
            assert(fat32_match_text(vectors[i].a, vectors[i].a));
        }
    }
    assert(live_requested == heap && allocations == allocation_count);
    assert(sizeof(fat32_upper_ranges) == 3200U);
    /* Also verify every possible scalar against the checked-in range bounds
     * and idempotence, including identity holes in stride-two ranges. */
    for (u32 value = 0; value <= 0x10ffffU; value++) {
        u32 upper = fat32_uppercase(value);
        assert(upper <= 0x10ffffU && fat32_uppercase(upper) == upper);
        assert(fat32_uppercase(value) == upper);
    }
    static const char *invalid[] = {
        "", "\x80", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf4\x90\x80\x80", "\xc3", "\xe2\x82", "\xf0\x90\x80",
        "ok\xff", "\xef\xbf\xbf", " ", ".", "..",
    };
    for (u32 i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        assert(!fat32_match_text(invalid[i], invalid[i]));
    char too_long[257];
    memset(too_long, 'a', sizeof(too_long) - 1U);
    too_long[256] = '\0';
    assert(!fat32_match_text(too_long, too_long));
}

static void imported_names(void)
{
    static const u16 resume[] = {0x00e9, '.', 't', 'x', 't'};
    setup();
    import_lfn(resume, 5U, "RESUME~1TXT");
    vfs_node_t *file = fat32_finddir(&root, "É.TXT");
    assert(file && fat32_finddir(&root, "é.txt") == file &&
           fat32_finddir(&root, "resume~1.txt") == file);
    vfs_dirent_t listed;
    assert(fat32_readdir(&root, 0U, &listed) && !strcmp(listed.name, "é.txt"));
    unsigned before = writes;
    assert(fat32_check_name_available_locked_view(&root, "É.txt", NULL) ==
           VFS_ERR_INVALID_PARAM);
    assert(fat32_check_name_available_locked_view(&root, "RESUME~1.TXT", NULL) ==
           VFS_ERR_INVALID_PARAM);
    assert(writes == before && fs.version_confident);
    assert(unlink_finished(&root, "É.TXT") == VFS_OK);
    cleanup();

    /* An LFN can collide with an ASCII short alias through simple uppercase. */
    static const u16 cross[] = {0x0131, '.', 't', 'x', 't'};
    setup();
    import_lfn(cross, 5U, "DOTLES~1TXT");
    file = fat32_finddir(&root, "I.TXT");
    assert(file && fat32_finddir(&root, "dotles~1.txt") == file);
    vfs_node_t prepared = {0};
    vfs_file_facts_t facts;
    replace_source_state_t source = {.short_at = ~(u64)0};
    u64 pin = 0;
    assert(fat32_create_contents_locked(&root, &prepared, "I.TXT", 0U,
           replace_source, &source, &facts, &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(!pin && !active_pin && !writes && !fs.replace_transaction);
    cleanup();

    /* Valid 150-unit LFN is 300 UTF-8 bytes: old 256-byte matching lost it.
     * Its ASCII uppercase fits the public ABI and must still conflict. */
    u16 long_units[260];
    char uppercase[256];
    for (u32 i = 0; i < 255U; i++) { long_units[i] = 0x0131; uppercase[i] = 'I'; }
    uppercase[150] = '\0';
    setup();
    import_lfn(long_units, 150U, "LONGNA~1TXT");
    assert(fat32_finddir(&root, uppercase));
    assert(fat32_check_name_available_locked_view(&root, uppercase, NULL) ==
           VFS_ERR_INVALID_PARAM && fs.version_confident);
    assert(fat32_readdir(&root, 0U, &listed) && !strcmp(listed.name, "LONGNA~1.TXT"));
    cleanup();
    uppercase[150] = 'I'; uppercase[255] = '\0';
    setup();
    import_lfn(long_units, 255U, "MAXNAM~1TXT");
    assert(fat32_finddir(&root, uppercase));
    assert(fat32_check_name_available_locked_view(&root, uppercase, NULL) ==
           VFS_ERR_INVALID_PARAM);
    cleanup();

    static const u16 supplementary[] = {0xd801, 0xdc28, '.', 't', 'x', 't'};
    setup();
    import_lfn(supplementary, 6U, "DESERE~1TXT");
    assert(fat32_finddir(&root, "𐐀.TXT"));
    cleanup();
}

static void ambiguous_aliases(void)
{
    /* Byte 0x82 is é in CP437/850 but another character in other OEM pages.
     * Neither a boot OEM label nor plausible bytes identify the code page. */
    static const char oem[] = "\x82       TXT";
    setup();
    entry_at(2U, 0U, oem, 0U, 0U, 0x20U);
    entry_at(2U, 1U, "KNOWN   TXT", 0U, 0U, 0x20U);
    assert(!fat32_finddir(&root, "é.txt") && !fat32_finddir(&root, "\x82.txt"));
    assert(fat32_finddir(&root, "KNOWN.TXT"));
    vfs_dirent_t listed;
    assert(fat32_readdir(&root, 0U, &listed) && !strcmp(listed.name, "KNOWN.TXT"));
    assert(!fat32_readdir(&root, 1U, &listed));
    unsigned before = writes;
    usize retained = live_requested;
    vfs_node_t prepared = {0}, *created = NULL;
    vfs_file_facts_t facts;
    replace_source_state_t source = {.short_at = ~(u64)0};
    u64 pin = 0;
    for (u32 i = 0; i < 3U; i++) {
        assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT", 0U,
               replace_source, &source, &facts, &pin) == VFS_ERR_UNSUPPORTED);
        assert(!pin && !active_pin && !fs.replace_transaction && fs.version_confident);
        assert(create_finished(&root, "NEW.TXT", &created) == VFS_ERR_UNSUPPORTED);
        assert(mkdir_finished(&root, "NEWDIR", &created) == VFS_ERR_UNSUPPORTED);
        assert(unlink_finished(&root, "KNOWN.TXT") == VFS_ERR_UNSUPPORTED);
        assert(fat32_rename(&root, "KNOWN.TXT", &root, "OTHER.TXT") == VFS_ERR_UNSUPPORTED);
        assert(!created && writes == before && live_requested == retained);
    }
    /* Unsupported namespace is local: another directory can be scanned. */
    ((u32 *)media[1])[6] = FAT32_EOC_MIN;
    entry_at(2U, 2U, "SUBDIR     ", 6U, 0U, 0x10U);
    vfs_node_t *subdir = fat32_finddir(&root, "subdir");
    assert(subdir && fat32_check_name_available_locked_view(subdir, "NEW.TXT", NULL) == VFS_OK);
    assert(rmdir_finished(&root, "SUBDIR") == VFS_ERR_UNSUPPORTED);
    ((fat32_on_disk_entry_t *)media[10])[0].name[0] = (char)0xe5;
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) == VFS_OK);
    cleanup();

    /* A good LFN remains readable even if its alias is not interpretable. */
    static const u16 name[] = {0x00e9, '.', 't', 'x', 't'};
    setup();
    import_lfn(name, 5U, oem);
    vfs_node_t *file = fat32_finddir(&root, "É.TXT");
    assert(file && fat32_readdir(&root, 0U, &listed) && !strcmp(listed.name, "é.txt"));
    assert(fat32_file_facts(file, &facts) == VFS_OK &&
           fat32_ownership_preflight_locked(file) == VFS_OK && fs.version_confident);
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) == VFS_ERR_UNSUPPORTED);
    cleanup();

    /* 0x05 escapes an OEM 0xe5 first byte; it is not ASCII control U+0005. */
    setup();
    entry_at(2U, 0U, "\x05       TXT", 0U, 0U, 0x20U);
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) == VFS_ERR_UNSUPPORTED);
    assert(fs.version_confident && !writes);
    cleanup();
}

static void malformed_and_failures(void)
{
    static const u16 invalid[][3] = {{0xd800}, {0xdc00}, {0xd800, 'A'}, {0xffff}};
    for (u32 i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        setup();
        import_lfn(invalid[i], invalid[i][1] ? 2U : 1U, "BROKEN~1TXT");
        /* Read through a known short alias, but never mutate malformed media. */
        assert(fat32_finddir(&root, "BROKEN~1.TXT"));
        assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) ==
               VFS_ERR_BAD_FORMAT && !fs.version_confident && !writes);
        cleanup();
    }
    setup();
    static const u16 one[] = {'A'};
    import_lfn(one, 1U, "PADBAD~1TXT");
    ((fat32_lfn_entry_t *)media[10])->name1[3] = 'X';
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) ==
           VFS_ERR_BAD_FORMAT && !writes);
    cleanup();
    setup();
    u16 overlong[256];
    for (u32 i = 0; i < 256U; i++) overlong[i] = 'A';
    import_lfn(overlong, 256U, "TOOLON~1TXT");
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) ==
           VFS_ERR_BAD_FORMAT && !writes);
    cleanup();

    setup();
    unsigned before = reads, count = allocations;
    assert(fat32_check_name_available_locked_view(&root, "\xed\xa0\x80", NULL) == VFS_ERR_INVALID_PARAM);
    assert(reads == before && allocations == count && !writes && fs.version_confident);
    fail_allocation = allocations + 1U;
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) == VFS_ERR_NO_MEM);
    assert(!live_requested && !writes && fs.version_confident);
    fail_allocation = 0U;
    fail_read_lba = 10U;
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) == VFS_ERR_IO);
    assert(!live_requested && !writes && fs.version_confident);
    fail_read_lba = ~(u64)0;
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) == VFS_OK);
    vfs_node_t *created = NULL;
    assert(create_finished(&root, "LONGERNAME.TXT", &created) == VFS_OK && created);
    assert(mkdir_finished(&root, "é", &created) == VFS_OK && created &&
           created->type == VFS_TYPE_DIRECTORY && fs.version_confident);
    assert(fat32_ownership_preflight_locked(created) == VFS_OK);
    cleanup();
}

/* Optional independently formatted image: read-only production mount and
 * namespace operations. The harness still supplies heap/VFS/block services. */
static void namespace_image_case(const char *path)
{
    image_file = fopen(path, "rb");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    device.id = 903U;
    device.sector_size = 512U;
    device.sector_count = image_sectors;
    online = true;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, true) == VFS_OK);
    vfs_node_t *accented = fat32_finddir(mounted->root_node, "RÉSUMÉ LONG.TXT");
    vfs_node_t *ascii = fat32_finddir(mounted->root_node, "LONG REPORT.TXT");
    vfs_node_t *plain = fat32_finddir(mounted->root_node, "plain.txt");
    assert(accented && ascii && plain);
    assert(fat32_finddir(mounted->root_node, "Résumé long.txt") == accented);
    char contents[8];
    assert(fat32_read(accented, 0U, sizeof(contents), contents) == 8U &&
           !memcmp(contents, "fixture\n", 8U));
    fat32_walk_entry_t imported;
    fat32_query_t query = {.mode = FAT32_QUERY_NAME, .name = "Résumé long.txt"};
    assert(fat32_dir_query(mounted->root_node, &query) == VFS_OK && query.found);
    imported = query.match;
    /* This fixture deliberately uses mtools' OEM alias. Even a known fixture
     * codepage must not be inferred by the production driver from these bytes. */
    assert(!fat32_short_ascii(&imported.disk));
    unsigned before = writes;
    assert(fat32_check_name_available_locked_view(mounted->root_node, "NEW.TXT", NULL) == VFS_ERR_UNSUPPORTED);
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    assert(mounted_fs->version_confident && writes == before);
    vfs_node_t *nodes[4];
    u32 count = 0;
    for (fat32_node_state_t *state = mounted_fs->nodes; state; state = state->next) {
        assert(count < 4U);
        nodes[count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0; i < count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    assert(!live_requested);
    puts("FAT32 independent image: accented/ASCII LFN reads and OEM mutation refusal passed");
}

int main(int argc, char **argv)
{
    comparison_vectors();
    imported_names();
    ambiguous_aliases();
    malformed_and_failures();
    if (argc > 1) namespace_image_case(argv[1]);
    assert(!live_requested);
    puts("FAT32 pinned Unicode comparison, imported aliases and conservative namespace failures passed");
    return 0;
}
