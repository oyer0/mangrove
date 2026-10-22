/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Full production parser/namespace paths; controlled mounted I/O and pins. */
#define main exfat_identity_fixture_main
#include "exfat_mount_identity_test.c"
#undef main

static void mark_used(u32 cluster)
{
    media[121U + (cluster - 2U) / 4096U][(cluster - 2U) / 8U % 512U] |=
        (u8)(1U << ((cluster - 2U) % 8U));
}

static void imported_setup(void)
{
    setup_large();
    fs.upcase = (u8 *)exfat_default_upcase;
    fs.upcase_length = sizeof(exfat_default_upcase);
    exfat_put64(media[120] + 128U + 24U, fs.upcase_length);
    for (u32 i = 0U; i < (fs.upcase_length + 511U) / 512U; i++) {
        ((u32 *)media[24])[6U + i] =
            i + 1U < (fs.upcase_length + 511U) / 512U ? 7U + i : EXFAT_CLUSTER_EOC;
        mark_used(6U + i);
    }
    ((u32 *)media[24])[2] = 20U;
    for (u32 i = 20U; i <= 22U; i++) {
        ((u32 *)media[24])[i] = i < 22U ? i + 1U : EXFAT_CLUSTER_EOC;
        mark_used(i);
    }
    root.size = root_state.data_length = root_state.valid_data_length = 2048U;
    for (u32 offset = 160U; offset < 480U; offset += 32U)
        media[120][offset] = 0x05U;
}

/* Independent byte recurrence and fixture entry assembly. No production
 * builder/UTF conversion participates in constructing imported names. */
static u16 fixture_rotate(u16 value)
{ return (u16)((value >> 1U) | (value << 15U)); }

static u32 imported_set(const u16 *units, u16 length, u16 attributes,
                       u32 first, u64 bytes, u8 set[640])
{
    u32 count = 2U + (length + 14U) / 15U;
    assert(count <= 20U);
    memset(set, 0, 640U);
    set[0] = 0x85U; set[1] = (u8)(count - 1U);
    exfat_put16(set + 4U, attributes);
    set[32] = 0xc0U; set[33] = bytes ? 3U : 1U; set[35] = (u8)length;
    exfat_put64(set + 40U, bytes); exfat_put32(set + 52U, first);
    exfat_put64(set + 56U, bytes);
    u16 hash = 0U, checksum = 0U;
    for (u16 i = 0U; i < length; i++) {
        u16 upper = exfat_upcase_unit(&fs, units[i]);
        hash = (u16)(fixture_rotate(hash) + (upper & 255U));
        hash = (u16)(fixture_rotate(hash) + (upper >> 8U));
        exfat_put16(set + (2U + i / 15U) * 32U + 2U + i % 15U * 2U, units[i]);
    }
    exfat_put16(set + 36U, hash);
    for (u32 i = 2U; i < count; i++) set[i * 32U] = 0xc1U;
    for (u32 i = 0U; i < count * 32U; i++)
        if (i != 2U && i != 3U) checksum = (u16)(fixture_rotate(checksum) + set[i]);
    exfat_put16(set + 2U, checksum);
    return count;
}

static void write_root_fixture(u64 offset, const u8 *bytes, u32 length)
{
    while (length) {
        u64 lba = offset < 512U ? 120U : 138U + (offset - 512U) / 512U;
        u32 at = offset % 512U, amount = 512U - at;
        if (amount > length) amount = length;
        memcpy(media[lba] + at, bytes, amount);
        offset += amount; bytes += amount; length -= amount;
    }
}

static u64 seed_imported(const u16 *units, u16 length, bool directory)
{
    u8 set[640];
    u32 count = imported_set(units, length, directory ? 0x10U : 0U,
                             30U, 512U, set);
    mark_used(30U);
    write_root_fixture(480U, set, count * 32U);
    const u16 later[] = {'l','a','t','e','r','.','t','x','t'};
    u64 offset = 480U + count * 32U;
    count = imported_set(later, 9U, 0U, 31U, 1U, set);
    mark_used(31U); media[149][0] = 'L';
    write_root_fixture(offset, set, count * 32U);
    if (directory) {
        const u16 child[] = {'c','h','i','l','d'};
        count = imported_set(child, 5U, 0U, 32U, 1U, set);
        memcpy(media[148], set, count * 32U);
        mark_used(32U); media[150][0] = 'C';
    }
    writes = reads = flushes = 0U;
    return offset;
}

static void names_regression(void)
{
    for (unsigned kind = 0U; kind < 6U; kind++) {
        imported_setup();
        u16 units[255]; u16 length = kind == 0U ? 128U : 255U;
        for (u16 i = 0U; i < length; i++) units[i] = kind == 2U ? 0x4e00U : 0xe9U;
        if (kind == 3U) {
            for (u16 i = 0U; i < 254U; i += 2U) {
                units[i] = 0xd83dU; units[i + 1U] = 0xde00U;
            }
            units[254] = 'X';
        }
        if (kind == 4U) { length = 128U; units[127] = 'x'; }
        if (kind == 5U) { length = 255U; units[14] = 0xd83dU; units[15] = 0xde00U; }
        u64 later_offset = seed_imported(units, length, kind == 1U);
        exfat_entry_info_t entry;
        assert(exfat_entry_set_read(&fs, 2U, false, 480U, &entry));
        assert(entry.name_length == length && !memcmp(entry.name_units, units, length * 2U));
        assert(strlen(entry.name) == (kind == 0U ? 256U : kind == 2U ? 765U :
            kind == 3U ? 509U : kind == 4U ? 255U : 510U));
        u64 offset = 160U; bool error = true;
        exfat_entry_info_t walked;
        assert(exfat_directory_next(&fs, 2U, false, 0U, &offset, &walked, &error));
        assert(!error && offset == later_offset && !strcmp(entry.name, walked.name));
        assert(exfat_directory_next(&fs, 2U, false, 0U, &offset, &walked, &error));
        assert(!strcmp(walked.name, "later.txt"));
        vfs_node_t *later = exfat_finddir(&root, "LATER.TXT");
        assert(later && exfat_node_read(later, 0U, 1U, &(u8){0}) == 1);
        assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
        vfs_dirent_t dirent, before; memset(&dirent, 0x55, sizeof(dirent)); before = dirent;
        int result = exfat_readdir_checked(&root, 1U, &dirent);
        assert(result == (kind == 4U ? 1 : VFS_ERR_NAME_TOO_LONG));
        if (result < 0) assert(!memcmp(&dirent, &before, sizeof(dirent)));
        assert(exfat_readdir_checked(&root, 2U, &dirent) == 1 && !strcmp(dirent.name, "later.txt"));
        assert(!exfat_readdir_checked(&root, 3U, &dirent));
        void *cursor = NULL; assert(exfat_readdir_open(&root, &cursor));
        unsigned visible = 0U, capacity_errors = 0U;
        for (;;) {
            before = dirent; result = exfat_readdir_next_checked(cursor, &dirent);
            if (!result) break;
            if (result == VFS_ERR_NAME_TOO_LONG) { capacity_errors++; assert(!memcmp(&before, &dirent, sizeof(dirent))); }
            else { assert(result == 1); visible++; }
        }
        assert(visible == (kind == 4U ? 3U : 2U) && capacity_errors == (kind == 4U ? 0U : 1U));
        exfat_readdir_close(cursor);
        /* A hidden name and its directory descendants still own clusters. */
        mark_used(30U); media[149][0] = 'L';
        u8 duplicate[640]; const u16 duplicate_name[] = {'d','u','p'};
        u32 count = imported_set(duplicate_name, 3U, 0U, kind == 1U ? 32U : 30U, 1U, duplicate);
        write_root_fixture(later_offset + 96U, duplicate, count * 32U);
        assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
        assert(writes == 0U && !pin_active);
        cleanup();
    }
}

static void malformed_regression(void)
{
    const u16 invalid[][3] = {{0xd800U, 'x', 0U}, {0xdc00U, 'x', 0U},
        {'x', 0U, 'y'}, {'x', '/', 'y'}, {'x', 0x1fU, 'y'}};
    for (unsigned i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        imported_setup(); seed_imported(invalid[i], 3U, false);
        assert(!exfat_entry_set_read(&fs, 2U, false, 480U, &(exfat_entry_info_t){0}));
        vfs_dirent_t out, old; memset(&out, 0x55, sizeof(out)); old = out;
        assert(exfat_readdir_checked(&root, 1U, &out) == VFS_ERR_IO && !memcmp(&out, &old, sizeof(out)));
        void *cursor; assert(exfat_readdir_open(&root, &cursor));
        assert(exfat_readdir_next_checked(cursor, &out) == 1); old = out;
        u64 before = ((exfat_directory_cursor_t *)cursor)->offset;
        assert(exfat_readdir_next_checked(cursor, &out) == VFS_ERR_IO &&
               ((exfat_directory_cursor_t *)cursor)->offset == before && !memcmp(&out, &old, sizeof(out)));
        exfat_readdir_close(cursor);
        assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
        assert(!exfat_finddir(&root, "later.txt") && !writes && !pin_active);
        cleanup();
    }
    imported_setup(); const u16 valid[] = {0x7fU, 0x80U, 0x9fU};
    seed_imported(valid, 3U, false);
    assert(exfat_entry_set_read(&fs, 2U, false, 480U, &(exfat_entry_info_t){0}));
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK); cleanup();
    for (unsigned fault = 0U; fault < 4U; fault++) {
        imported_setup(); u16 units[255]; for (unsigned i = 0; i < 255U; i++) units[i] = 0x4e00U;
        seed_imported(units, 255U, false);
        if (fault == 0U) media[120][482U] ^= 1U;
        if (fault == 1U) { fail_read_lba = 138U; }
        if (fault == 2U) { fail_allocation = allocations + 1U; }
        if (fault == 3U) { replace_after_reads = reads + 1U; }
        int result = exfat_ownership_preflight_locked(&root);
        assert(result != VFS_OK && !writes && !pin_active); cleanup();
    }
    for (unsigned zero_length = 0; zero_length < 2; zero_length++) {
        imported_setup(); const u16 name[] = {'v','a','l','i','d'}; seed_imported(name, 5U, false);
        u8 *set = media[120] + 480U;
        /* Assemble across the fixture boundary without production writers. */
        u8 copy[640]; memcpy(copy, set, 32U); memcpy(copy + 32U, media[138], 64U);
        if (zero_length) copy[35] = 0; else copy[36] ^= 1U;
        u16 checksum = 0;
        for (unsigned i = 0; i < 96U; i++) if (i != 2U && i != 3U) checksum = fixture_rotate(checksum) + copy[i];
        exfat_put16(copy + 2U, checksum); write_root_fixture(480U, copy, 96U);
        assert(!exfat_entry_set_read(&fs, 2U, false, 480U, &(exfat_entry_info_t){0}));
        assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT && !writes); cleanup();
    }
    /* Small conversion buffers must fail without unsigned underflow. */
    const u16 bmp[] = {0x4e00U}, pair[] = {0xd83dU, 0xde00U};
    for (usize capacity = 1U; capacity < 5U; capacity++) {
        char buffer[5]; memset(buffer, 0x55, sizeof(buffer));
        assert(exfat_utf16_to_utf8(bmp, 1U, buffer, capacity) == (capacity >= 4U));
        assert(exfat_utf16_to_utf8(pair, 2U, buffer, capacity) == (capacity >= 5U));
        assert((u8)buffer[capacity] == 0x55U);
    }
}

static void collision_regression(void)
{
    imported_setup();
    /* A validated volume may include the Unicode simple-uppercase mapping
     * U+2C65 -> U+023A. Its two-byte query matches a three-byte disk spelling. */
    static u8 dense[0x20000U];
    for (u32 i = 0U; i < 65536U; i++) exfat_put16(dense + i * 2U,
        i >= 'a' && i <= 'z' ? i - 'a' + 'A' : i);
    exfat_put16(dense + 0x2c65U * 2U, 0x23aU);
    fs.upcase = dense; fs.upcase_length = sizeof(dense); fs.upcase_cluster = 1000U;
    exfat_put32(media[120] + 128U + 20U, fs.upcase_cluster);
    exfat_put64(media[120] + 128U + 24U, fs.upcase_length);
    for (u32 i = 1000U; i < 1256U; i++) {
        ((u32 *)media[24])[i] = i < 1255U ? i + 1U : EXFAT_CLUSTER_EOC;
        mark_used(i);
    }
    assert(exfat_upcase_decode_valid(fs.upcase, fs.upcase_length));
    u16 units[127]; for (unsigned i = 0U; i < 127U; i++) units[i] = 0x2c65U;
    u64 later_offset = seed_imported(units, 127U, false);
    char query[255]; for (unsigned i = 0; i < 254U; i += 2U) {
        query[i] = (char)0xc8U; query[i + 1U] = (char)0xbaU;
    }
    query[254] = 0;
    vfs_node_t *node = exfat_finddir(&root, query);
    assert(node && strlen(((exfat_node_t *)node->fs_data)->name) == 381U);
    assert(exfat_ownership_preflight_locked(node) == VFS_OK);
    vfs_file_facts_t current; assert(exfat_file_facts(node, &current) == VFS_OK);
    u8 contents[512]; mg_file_lines_state_t snapshot;
    assert(exfat_snapshot_stream(node, sizeof(contents), sink, contents, &snapshot) == VFS_OK);
    assert(snapshot.identifier == node->inode && snapshot.length == 512U && fs.version_confident);
    vfs_node_t spare = {0}; vfs_file_facts_t facts; u64 pin = 0U;
    assert(exfat_create_contents_locked(&root, &spare, query, 0U, NULL, NULL,
        &facts, &pin) == VFS_ERR_ALREADY_EXISTS && !pin && !writes);
    /* Full comparison detects a second folded match, without truncated names. */
    u8 set[640]; for (unsigned i = 0U; i < 127U; i++) units[i] = 0x23aU;
    u32 count = imported_set(units, 127U, 0U, 0U, 0U, set);
    write_root_fixture(later_offset + 96U, set, count * 32U);
    bool found = false; exfat_entry_info_t entry;
    assert(exfat_find_entry_locked_view(&fs, 2U, false, 0U, query, &entry, &found) ==
        VFS_ERR_BAD_FORMAT && !fs.version_confident && !writes && !pin_active);
    cleanup();
}


static void external_names_image(const char *path)
{
    setup(); device.sector_count = 12000U;
    FILE *image = fopen(path, "rb"); assert(image && fread(media, sizeof(media), 1U, image) == 1U); fclose(image);
    exfat_fs_t *mounted = exfat_new_state(&device); assert(mounted && mounted->cluster_bytes == 4096U);
    fs = *mounted; kfree(mounted); fs.next_inode = 1U; fs.nodes = &root_state;
    root_state.first_cluster = fs.root_cluster;
    root_state.data_length = root_state.valid_data_length = root.size = fs.cluster_bytes;
    unsigned before_reads = reads, before_allocations = allocations;
    clock_t begin = clock();
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    unsigned proof_reads = reads - before_reads;
    double milliseconds = (double)(clock() - begin) * 1000.0 / CLOCKS_PER_SEC;
    printf("Independent exFAT ownership: reads=%u allocations=%u peak_requested=%llu cpu_ms=%.3f\n",
           proof_reads, allocations - before_allocations, (unsigned long long)peak_requested, milliseconds);
    vfs_node_t *later = exfat_finddir(&root, "LATER.TXT"); u8 byte;
    assert(later && exfat_node_read(later, 0U, 1U, &byte) == 1U && byte == 'L');
    char maximum[256]; memset(maximum, 'A', 255U); maximum[255] = 0;
    vfs_node_t *ascii = exfat_finddir(&root, maximum);
    assert(ascii && exfat_node_read(ascii, 0U, 1U, &byte) == 1U && byte == 'E');
    void *cursor = NULL; assert(exfat_readdir_open(&root, &cursor));
    unsigned reported = 0U, errors = 0U; vfs_dirent_t entry, old;
    memset(&entry, 0x55, sizeof(entry));
    for (;;) {
        old = entry; int result = exfat_readdir_next_checked(cursor, &entry);
        if (!result) break;
        if (result == VFS_ERR_NAME_TOO_LONG) { errors++; assert(!memcmp(&entry, &old, sizeof(entry))); }
        else { assert(result == 1); reported++; }
    }
    assert(reported == 3U && errors == 4U && !strcmp(entry.name, "later.txt"));
    exfat_readdir_close(cursor);
    for (unsigned i = 0; i < 7U; i++) {
        old = entry; int result = exfat_readdir_checked(&root, i, &entry);
        assert(result == (i == 3U || i == 4U || i == 6U ? 1 : VFS_ERR_NAME_TOO_LONG));
        if (result < 0) assert(!memcmp(&entry, &old, sizeof(entry)));
    }
    assert(!exfat_readdir_checked(&root, 7U, &entry));
    /* The oversized imported directory still participates in the proof and
     * its child is discoverable by the production internal mapping. */
    u64 offset = 0U; exfat_entry_info_t info; bool error;
    for (unsigned i = 0; i < 6U; i++)
        assert(exfat_directory_next(&fs, fs.root_cluster, false, 0U, &offset, &info, &error));
    assert(info.attributes & EXFAT_ATTR_DIRECTORY);
    vfs_node_t *directory = exfat_make_node(&root, &info); assert(directory);
    vfs_node_t *inside = exfat_finddir(directory, "inside.txt");
    assert(inside && exfat_node_read(inside, 0U, 1U, &byte) == 1U && byte == 'Z');
    assert(!writes && !flushes && !pin_active);
    printf("Independent exFAT readback: reported=%u name_errors=%u reads=%u writes=%u flushes=%u peak_requested=%llu\n",
        reported, errors, reads, writes, flushes, (unsigned long long)peak_requested);
    kfree(fs.upcase); fs.upcase = NULL; cleanup();
}

int main(int argc, char **argv)
{
    if (argc == 2) { external_names_image(argv[1]); return 0; }
    names_regression(); malformed_regression(); collision_regression();
    printf("exFAT full imported names: entry=%zu node=%zu, no parser heap allocations\n",
        sizeof(exfat_entry_info_t), sizeof(exfat_node_t));
    puts("exFAT imported Unicode, later visibility and complete ownership passed");
}
