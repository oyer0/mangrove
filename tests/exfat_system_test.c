/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production driver/VFS; controlled credentials, locks, heap, device and pins. */
#define main exfat_identity_fixture_main
#define vfs_check_access visibility_driver_access
#include "exfat_mount_identity_test.c"
#undef vfs_check_access
#undef main

static bool permit_read = true;
bool vfs_check_access(const vfs_node_t *node, u32 access)
{
    if (!node || ((access & VFS_ACCESS_READ) && !permit_read) ||
        ((access & VFS_ACCESS_WRITE) && !allow_write)) return false;
    u32 permissions = node->owner_uid == 7U ? node->permissions : node->permissions >> 2U;
    return (!(access & VFS_ACCESS_READ) || (permissions & VFS_PERMISSION_OWNER_READ)) &&
           (!(access & VFS_ACCESS_WRITE) || (permissions & VFS_PERMISSION_OWNER_WRITE));
}
#include "vfs_system_visibility.inc"

static void mark_cluster(u32 cluster)
{
    u32 bit = cluster - 2U;
    media[121U + bit / 4096U][bit / 8U % 512U] |= (u8)(1U << (bit % 8U));
}
static void store_entry(u64 *offset, const char *name, u16 attributes,
                        u32 first, u64 size, bool chained, u8 *directory)
{
    u8 set[640], count;
    char placeholder[129]; memset(placeholder, 'E', 128U); placeholder[128] = 0;
    bool imported = strlen(name) > 255U;
    assert(exfat_build_file_set(&fs, imported ? placeholder : name, attributes, first, size, size, !chained, set, &count));
    if (imported) {
        u16 units[128];
        for (u32 i = 0; i < 128U; i++) {
            units[i] = 0x00e9U;
            exfat_put16(set + (2U+i/15U)*32U + 2U + (i%15U)*2U, units[i]);
        }
        exfat_put16(set + 36U, exfat_name_hash(&fs, units, 128U));
    }
    for (u32 at = 8U; at <= 16U; at += 4U)
        exfat_put32(set + at, (46U << 25) | (10U << 21) | (14U << 16));
    set[22] = set[23] = set[24] = 0x80U;
    exfat_put16(set + 2U, exfat_entry_checksum(set, count));
    if (directory) memcpy(directory, set, (usize)count * 32U);
    else for (u32 at = 0; at < count * 32U; at++) {
        u64 position = *offset + at, cluster = position / 512U;
        u64 lba = cluster ? 138U + cluster - 1U : 120U;
        media[lba][position % 512U] = set[at];
    }
    *offset += (u64)count * 32U;
}
static void internal_image(void)
{
    setup_large();
    fs.upcase = (u8 *)exfat_default_upcase; fs.upcase_length = sizeof(exfat_default_upcase);
    exfat_put64(media[120] + 128U + 24U, fs.upcase_length);
    for (u32 index = 0; index < (fs.upcase_length + 511U) / 512U; index++) {
        u32 cluster = 6U + index; mark_cluster(cluster);
        ((u32 *)media[24])[cluster] = index + 1U < (fs.upcase_length + 511U) / 512U ? cluster + 1U : EXFAT_CLUSTER_EOC;
    }
    memset(media[120], 0x05, 96U); /* Retired slots, preserve real system entries. */
    for (u32 at = 160U; at < 480U; at += 32U) media[120][at] = 0x05U;
    ((u32 *)media[24])[2] = 20U;
    for (u32 cluster = 20U; cluster <= 24U; cluster++) {
        mark_cluster(cluster);
        ((u32 *)media[24])[cluster] = cluster == 24U ? EXFAT_CLUSTER_EOC : cluster + 1U;
    }
    root.size = root_state.data_length = root_state.valid_data_length = 3072U;
    vfs_node_set_security(&root, VFS_UID_SYSTEM, EXFAT_VOLUME_PERMISSIONS);
    u32 next = 40U; u64 offset = 480U;
    for (unsigned directory = 0; directory < 2U; directory++)
        for (unsigned combination = 0; combination < 8U; combination++) {
            char name[64];
            if (!combination) strcpy(name, directory ? "Syst\xc3\xa8me" : "Syst\xc3\xa8me.txt");
            else snprintf(name, sizeof(name), "%s%u", directory ? "dir" : "file", combination);
            u16 attributes = EXFAT_ATTR_SYSTEM | (directory ? EXFAT_ATTR_DIRECTORY : 0U) |
                (combination & 3U) | ((combination & 4U) ? EXFAT_ATTR_ARCHIVE : 0U);
            u32 first = next++; mark_cluster(first); media[exfat_cluster_lba(&fs, first)][0] = 'A' + combination;
            bool chained = directory && (combination & 2U);
            if (directory) {
                u64 child_offset = 0U; u32 parent = next++; mark_cluster(parent);
                store_entry(&child_offset, "R\xc3\xa9sum\xc3\xa9.txt", attributes & ~EXFAT_ATTR_DIRECTORY,
                    first, 1U, false, media[exfat_cluster_lba(&fs, parent)]);
                first = parent;
                if (chained) ((u32 *)media[24])[first] = EXFAT_CLUSTER_EOC;
            }
            store_entry(&offset, name, attributes, first, directory ? 512U : 1U, chained, NULL);
        }
    char long_name[257];
    for (unsigned i = 0; i < 128U; i++) { long_name[2U*i] = (char)0xc3; long_name[2U*i+1U] = (char)0xa9; }
    long_name[256] = 0;
    mark_cluster(next); store_entry(&offset, long_name, EXFAT_ATTR_SYSTEM, next++, 1U, false, NULL);
    mark_cluster(next); media[exfat_cluster_lba(&fs, next)][0] = 'L';
    store_entry(&offset, "later.txt", 0U, next, 1U, false, NULL);
}
static void load_image(const char *path)
{
    if (!path) {
        internal_image(); permit_read = true; super.reference_count = 1U;
        reads = allocations = 0U; peak_requested = live_requested; return;
    }
    setup(); permit_read = true; device.sector_count = 12000U;
    FILE *image = fopen(path, "rb");
    assert(image && fread(media, sizeof(media), 1U, image) == 1U && !fclose(image));
    vfs_super_t *mounted;
    assert(exfat_mount(NULL, &device, &mounted, false) == VFS_OK && !writes);
    exfat_fs_t *loaded = mounted->private_data;
    fs = *loaded; root = *mounted->root_node;
    root_state = *(exfat_node_t *)root.fs_data;
    kfree(root.fs_data); kfree(mounted->root_node); kfree(loaded); kfree(mounted);
    root.fs_data = &root_state; root.super = &super;
    root_state.vfs_node = &root; fs.nodes = &root_state;
    super.private_data = &fs; super.root_node = &root;
    super.reference_count = 1U;
    reads = allocations = 0U; peak_requested = live_requested;
}
static void release_image(void)
{
    assert(!operation_held && !offset_held && super.reference_count == 1U);
    if (pin_unresolved) discard_unresolved_fixture_pin();
    if (fs.upcase != exfat_default_upcase) kfree(fs.upcase);
    fs.upcase = NULL; cleanup();
}
static void enumerate(vfs_node_t *directory, unsigned count, unsigned oversized)
{
    vfs_dirent_t out, saved; memset(&out, 0xa5, sizeof(out)); saved = out;
    unsigned entries = 0U, errors = 0U;
    for (u32 index = 0; ; index++) {
        int result = vfs_readdir_checked(directory, index, &out);
        if (!result) break;
        if (result == VFS_ERR_NAME_TOO_LONG) {
            assert(!memcmp(&out, &saved, sizeof(out))); errors++; continue;
        }
        assert(result == 1);
        vfs_node_t *node = vfs_finddir(directory, out.name);
        assert(node && out.inode == node->inode && out.type == node->type);
        entries++; saved = out;
    }
    assert(entries == count && errors == oversized);
    assert(vfs_super_operation_lock(&super));
    void *cursor; assert(directory->ops->readdir_open(directory, &cursor));
    entries = errors = 0; memset(&out, 0xa5, sizeof(out)); saved = out;
    int result;
    while ((result = directory->ops->readdir_next_checked(cursor, &out)) != 0) {
        if (result == VFS_ERR_NAME_TOO_LONG) {
            assert(!memcmp(&out, &saved, sizeof(out))); errors++;
        } else { assert(result == 1); entries++; saved = out; }
    }
    directory->ops->readdir_close(cursor); vfs_super_operation_unlock(&super);
    assert(entries == count && errors == oversized);
}
static void visibility(const char *path, const char *output)
{
    load_image(path); usize baseline = live_requested; clock_t start = clock();
    enumerate(&root, 17U, 1U);
    for (unsigned directory = 0; directory < 2U; directory++)
        for (unsigned combination = 0; combination < 8U; combination++) {
            char name[64], pathname[96];
            if (!combination) strcpy(name, directory ? "Syst\xc3\xa8me" : "Syst\xc3\xa8me.txt");
            else snprintf(name, sizeof(name), "%s%u", directory ? "dir" : "file", combination);
            snprintf(pathname, sizeof(pathname), "/%s", name);
            vfs_node_t *node; assert(vfs_lookup(pathname, &node) == VFS_OK);
            u16 expected = EXFAT_ATTR_SYSTEM | (directory ? EXFAT_ATTR_DIRECTORY : 0U) |
                ((combination & 1U) ? EXFAT_ATTR_READ_ONLY : 0U) |
                ((combination & 2U) ? EXFAT_ATTR_HIDDEN : 0U) |
                ((combination & 4U) ? EXFAT_ATTR_ARCHIVE : 0U);
            assert(((exfat_node_t *)node->fs_data)->attributes == expected);
            if (directory) {
                enumerate(node, 1U, 0U);
                strncat(pathname, "/R\xc3\xa9sum\xc3\xa9.txt", sizeof(pathname)-strlen(pathname)-1U);
                assert(vfs_lookup(pathname, &node) == VFS_OK);
            }
            vfs_file_handle_t *handle;
            assert(vfs_open(pathname, VFS_OPEN_READ, &handle) == VFS_OK);
            char byte; assert(vfs_file_read(handle, 1U, &byte) == 1U && byte == (char)('A' + combination));
            assert(handle->save_expectation_valid && vfs_close(handle) == VFS_OK);
            unsigned before = writes;
            int status = vfs_open(pathname, VFS_OPEN_RDWR, &handle);
            if (combination & 1U) {
                assert(status == VFS_ERR_ACCESS_DENIED && !handle && writes == before);
                assert(vfs_write(node, 0U, 1U, "X") == 0U && writes == before);
            } else {
                assert(status == VFS_OK); u64 written;
                assert(vfs_file_write_status(handle, 1U, "X", &written) == VFS_OK && written == 1U);
                assert(vfs_close(handle) == VFS_OK);
                assert(((exfat_node_t *)node->fs_data)->attributes ==
                    (expected & ~EXFAT_ATTR_DIRECTORY));
            }
        }
    vfs_node_t *unicode, *later, *parent;
    assert(vfs_lookup("/SYST\xc3\x88ME.TXT", &unicode) == VFS_OK);
    assert(vfs_lookup("/later.txt", &later) == VFS_OK);
    assert(vfs_lookup("/SYST\xc3\x88ME", &parent) == VFS_OK);
    vfs_node_t *created = (void *)1; unsigned before = writes;
    assert(vfs_create(&root, "SYST\xc3\x88ME.TXT", &created) == VFS_ERR_ALREADY_EXISTS && !created && writes == before);
    assert(vfs_create(&root, "SYST\xc3\x88ME", &created) == VFS_ERR_ALREADY_EXISTS && !created && writes == before);
    assert(vfs_create(parent, "R\xc3\x89SUM\xc3\x89.TXT", &created) == VFS_ERR_ALREADY_EXISTS && !created && writes == before);
    assert(vfs_create(parent, "new", &created) == VFS_OK && vfs_finddir(parent, "NEW") == created);
    assert(vfs_unlink(parent, "new") == VFS_OK);
    /* Metadata classification is by type: a label does not reserve a filename. */
    assert(vfs_create(&root, "META", &created) == VFS_OK && vfs_finddir(&root, "meta") == created);
    assert(vfs_unlink(&root, "META") == VFS_OK);
    /* Newly visible objects still obey credentials and retained mount policy. */
    permit_read = false;
    assert(vfs_lookup("/Syst\xc3\xa8me.txt", &created) == VFS_ERR_ACCESS_DENIED);
    permit_read = true; super.read_only = true; before = writes;
    assert(vfs_write(unicode, 0U, 1U, "Z") == 0U && writes == before);
    assert(vfs_unlink(&root, "Syst\xc3\xa8me.txt") == VFS_ERR_ACCESS_DENIED && writes == before);
    super.read_only = false;
    assert(exfat_ownership_preflight_locked(parent) == VFS_OK);
    /* Retained identity and SYSTEM metadata survive conditional replacement. */
    u64 pin; u64 generation = ((exfat_node_t *)unicode->fs_data)->generation;
    assert(vfs_super_operation_lock(&super));
    assert(unicode->ops->replace(&super, unicode->inode, generation, 3U,
        replace_source, "SYS", &pin) == VFS_OK && pin);
    vfs_super_operation_unlock(&super); assert(vfs_operation_finish_pin(&super, VFS_OK, pin) == VFS_OK);
    char replaced[3];
    assert(vfs_read(unicode, 0U, sizeof(replaced), replaced) == sizeof(replaced) && !memcmp(replaced, "SYS", sizeof(replaced)));
    assert(((exfat_node_t *)unicode->fs_data)->attributes == EXFAT_ATTR_SYSTEM);
    assert(vfs_super_operation_lock(&super));
    assert(root.ops->rename_pinned(&super, &root, "Syst\xc3\xa8me.txt", &root, "moved.txt", &pin) == VFS_OK);
    vfs_super_operation_unlock(&super); assert(vfs_operation_finish_pin(&super, VFS_OK, pin) == VFS_OK);
    assert(vfs_finddir(&root, "moved.txt") == unicode && !vfs_finddir(&root, "Syst\xc3\xa8me.txt"));
    assert(vfs_unlink(&root, "moved.txt") == VFS_OK && !((exfat_node_t *)unicode->fs_data)->alive);
    assert(vfs_finddir(&root, "later.txt") == later && exfat_ownership_preflight_locked(later) == VFS_OK);
    printf("SYSTEM namespace/VFS lifecycle: peak=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
        (size_t)(peak_requested-baseline), reads, writes, flushes,
        1000.0*(double)(clock()-start)/CLOCKS_PER_SEC);
    if (output) {
        FILE *image = fopen(output, "wb");
        assert(image && fwrite(media, sizeof(media), 1U, image) == 1U && !fclose(image));
    }
    release_image();
}
static void adverse(const char *path)
{
    /* Corrupt SYSTEM files are errors, not hidden entries or reusable names. */
    load_image(path); u64 lba = exfat_cluster_lba(&fs, fs.root_cluster);
    media[lba][482] ^= 1U;
    vfs_dirent_t out, saved; memset(&out, 0xa5, sizeof(out)); saved = out;
    assert(vfs_readdir_checked(&root, 0U, &out) == VFS_ERR_IO && !memcmp(&out, &saved, sizeof(out)));
    assert(!vfs_finddir(&root, "Syst\xc3\xa8me.txt"));
    vfs_node_t *created;
    assert(vfs_create(&root, "new", &created) == VFS_ERR_BAD_FORMAT && !created && !writes);
    release_image();
    load_image(path); lba = exfat_cluster_lba(&fs, fs.root_cluster);
    /* A bitmap naming the unsupported second FAT is invalid metadata, not a file. */
    for (unsigned at = 0; at < 480U; at += 32U)
        if (media[lba][at] == EXFAT_ENTRY_BITMAP) media[lba][at+1] = 1U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT && !writes);
    release_image();
    load_image(path); vfs_node_t *file, *directory;
    assert(vfs_lookup("/Syst\xc3\xa8me.txt", &file) == VFS_OK);
    assert(vfs_lookup("/Syst\xc3\xa8me", &directory) == VFS_OK);
    u64 child_sector = exfat_cluster_lba(&fs, ((exfat_node_t *)directory->fs_data)->first_cluster);
    u8 *child = media[child_sector];
    exfat_put32(child + 52U, ((exfat_node_t *)file->fs_data)->first_cluster);
    exfat_put16(child + 2U, exfat_entry_checksum(child, 3U));
    assert(exfat_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT && !writes);
    release_image();
    /* Uncertainty remains explicit on writes through a newly visible handle. */
    load_image(path); vfs_file_handle_t *handle;
    assert(vfs_open("/Syst\xc3\xa8me.txt", VFS_OPEN_RDWR, &handle) == VFS_OK);
    u64 bytes; assert(vfs_file_write_status(handle, 1U, "X", &bytes) == VFS_OK && bytes == 1U);
    unsigned boundaries = writes, flush_boundaries = flushes;
    assert(vfs_close(handle) == VFS_OK); release_image();
    for (unsigned mode = 0; mode < 5U; mode++)
        for (unsigned at = 1; at <= (mode == 3U ? flush_boundaries : boundaries); at++) {
            load_image(path);
            assert(vfs_open("/Syst\xc3\xa8me.txt", VFS_OPEN_RDWR, &handle) == VFS_OK);
            if (mode < 3U) { fail_write_number = at; fail_write_copy_bytes = mode == 0U ? 0U : mode == 1U ? 128U : 512U; }
            if (mode == 3U) fail_flush_number = at;
            if (mode == 4U) remove_after_write_number = at;
            assert(vfs_file_write_status(handle, 1U, "X", &bytes) == VFS_ERR_UNCERTAIN && !bytes &&
                !handle->offset && pin_unresolved && !fs.version_confident);
            assert(vfs_close(handle) == VFS_OK); release_image();
        }
    printf("SYSTEM handle fault boundaries: %u writes, %u flushes; failed/torn/removal outcomes retained\n", boundaries, flush_boundaries);
}
static void readback(const char *path)
{
    load_image(path); enumerate(&root, 16U, 1U);
    vfs_node_t *node;
    assert(vfs_lookup("/Syst\xc3\xa8me.txt", &node) == VFS_ERR_NOT_FOUND);
    assert(vfs_lookup("/moved.txt", &node) == VFS_ERR_NOT_FOUND);
    assert(vfs_lookup("/SYST\xc3\x88ME/R\xc3\x89SUM\xc3\x89.TXT", &node) == VFS_OK);
    char byte; assert(vfs_read(node, 0U, 1U, &byte) == 1U && byte == 'X');
    assert(((exfat_node_t *)node->fs_data)->attributes == EXFAT_ATTR_SYSTEM);
    assert(vfs_lookup("/file3", &node) == VFS_OK &&
        ((exfat_node_t *)node->fs_data)->attributes ==
        (EXFAT_ATTR_SYSTEM | EXFAT_ATTR_READ_ONLY | EXFAT_ATTR_HIDDEN));
    assert(vfs_lookup("/later.txt", &node) == VFS_OK && vfs_read(node, 0U, 1U, &byte) == 1U && byte == 'L');
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK && !writes && !flushes);
    release_image(); puts("Fresh-mount SYSTEM namespace, preserved attributes and contents readback passed without writes");
}
int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 3); setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--readback")) { readback(argv[2]); return 0; }
    const char *path = argc == 3 ? argv[1] : NULL;
    visibility(path, argc == 3 ? argv[2] : NULL); adverse(path);
    puts("Production SYSTEM lookup, enumeration, descendant access, collisions and recovery passed");
}
