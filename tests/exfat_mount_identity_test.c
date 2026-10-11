/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production exFAT functions with an in-memory mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/storage/exfat.c"

static u8 media[12000][512];
static block_device_t device;
static exfat_fs_t fs;
static vfs_super_t super;
static vfs_node_t root;
static exfat_node_t root_state;
static bool online;
static bool fail_write;
static bool fail_flush;
static u64 fail_write_lba;
static unsigned fail_bitmap_write_number, bitmap_writes;
static unsigned writes;
static unsigned allocations, fail_allocation;
static usize live_requested, peak_requested;
typedef union { long double align; usize bytes; } allocation_header_t;
static u64 fail_read_lba;
static unsigned reads;
static unsigned replace_after_reads;
static bool pin_active, pin_unresolved, fail_pin_begin;
static vfs_recovery_handoff_t pin_handoff;
static void *pin_context;
static u64 next_pin_id;

int vfs_recovery_pin_begin_handoff(vfs_super_t *sb,
                                   vfs_recovery_handoff_t callback,
                                   void *context, u64 *out_id)
{
    assert(sb == &super && !pin_active);
    if (fail_pin_begin) return VFS_ERR_NO_SPACE;
    pin_active = true;
    pin_handoff = callback;
    pin_context = context;
    *out_id = ++next_pin_id;
    return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 id)
{
    assert(pin_active && id == next_pin_id);
    pin_unresolved = true;
    return VFS_OK;
}
static void finish_pin(void)
{
    assert(pin_active && !pin_unresolved && pin_handoff);
    assert(pin_handoff(&super, pin_context));
    pin_active = false;
    pin_handoff = NULL;
    pin_context = NULL;
}
static void discard_unresolved_fixture_pin(void)
{
    assert(pin_active && pin_unresolved && fs.replace_transaction);
    exfat_replace_free(fs.replace_transaction);
    fs.replace_transaction = NULL;
    pin_active = pin_unresolved = false;
    pin_handoff = NULL;
    pin_context = NULL;
}

void *kmalloc(usize size)
{
    allocation_header_t *allocation;
    if (++allocations == fail_allocation) return NULL;
    allocation = malloc(sizeof(*allocation) + size);
    if (!allocation) return NULL;
    allocation->bytes = size;
    live_requested += size;
    if (live_requested > peak_requested) peak_requested = live_requested;
    return allocation + 1;
}
void kfree(void *ptr)
{
    if (!ptr) return;
    allocation_header_t *allocation = (allocation_header_t *)ptr - 1;
    assert(live_requested >= allocation->bytes);
    live_requested -= allocation->bytes;
    free(allocation);
}
int vfs_register_fs(vfs_fs_type_t *type) { (void)type; return VFS_OK; }
bool vfs_current_uid(u32 *out) { *out = 7; return true; }
void vfs_node_set_security(vfs_node_t *node, u32 owner, u32 permissions)
{ node->owner_uid = owner; node->permissions = permissions; }
void vfs_node_register(vfs_node_t *node) { assert(node->super == &super); }
void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size)
{
    assert(sb == &super);
    for (exfat_node_t *state = fs.nodes; state; state = state->next)
        if (state->vfs_node->inode == inode) state->vfs_node->size = size;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                       void *buffer)
{
    if (!online || dev != &device || id != device.id ||
        lba >= device.sector_count || count != 1 || lba == fail_read_lba)
        return false;
    reads++;
    memcpy(buffer, media[lba], 512);
    if (reads == replace_after_reads) device.id++;
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
    if (!online || dev != &device || id != device.id ||
        lba >= device.sector_count || count != 1) return false;
    writes++;
    if (lba == fs.boot.heap_offset + 1U) bitmap_writes++;
    if (fail_write || lba == fail_write_lba ||
        (lba == fs.boot.heap_offset + 1U && fail_bitmap_write_number &&
         bitmap_writes == fail_bitmap_write_number)) {
        memcpy(media[lba], buffer, 128);
        return false;
    }
    memcpy(media[lba], buffer, 512);
    return true;
}
bool block_flush_for_id(block_device_t *dev, u64 id)
{ return !fail_flush && online && dev == &device && id == device.id; }
bool block_device_id_is_live(u64 id)
{ return online && id == device.id; }

static void setup(void)
{
    memset(media, 0, sizeof(media));
    memset(&fs, 0, sizeof(fs));
    memset(&super, 0, sizeof(super));
    memset(&root, 0, sizeof(root));
    memset(&root_state, 0, sizeof(root_state));
    device.id = 93;
    device.sector_count = 128;
    online = true;
    fail_write = false;
    fail_flush = false;
    fail_write_lba = ~(u64)0;
    fail_bitmap_write_number = bitmap_writes = 0;
    allocations = fail_allocation = 0;
    live_requested = peak_requested = 0;
    fail_read_lba = ~(u64)0;
    writes = 0;
    reads = 0;
    replace_after_reads = 0;
    pin_active = pin_unresolved = fail_pin_begin = false;
    pin_handoff = NULL;
    pin_context = NULL;
    fs.dev = &device;
    fs.device_id = device.id;
    fs.boot.cluster_count = 64;
    fs.boot.heap_offset = 40;
    fs.boot.fat_offset = 24;
    fs.boot.fat_length = 1;
    fs.boot.fat_count = 1;
    fs.boot.volume_length = 128;
    fs.sector_size = 512;
    fs.sectors_per_cluster = 1;
    fs.cluster_bytes = 512;
    fs.root_cluster = 2;
    fs.bitmap_cluster = 3;
    fs.bitmap_length = 512;
    fs.bitmap_no_fat = true;
    fs.upcase_no_fat = true;
    fs.version_confident = true;
    fs.next_inode = 1;
    fs.nodes = &root_state;
    super.dev = &device;
    super.private_data = &fs;
    super.root_node = &root;
    root.inode = 1;
    root.type = VFS_TYPE_DIRECTORY;
    root.size = 512;
    root.super = &super;
    root.fs_data = &root_state;
    root.ops = &exfat_node_ops;
    root_state.vfs_node = &root;
    root_state.first_cluster = 2;
    root_state.data_length = 512;
    root_state.valid_data_length = 512;
    root_state.generation = 1;
    root_state.alive = true;
    root_state.version_valid = true;
    ((u32 *)media[24])[2] = EXFAT_CLUSTER_EOC;
    ((u32 *)media[24])[3] = EXFAT_CLUSTER_EOC;
    media[41][0] = 3; /* root and bitmap clusters allocated */
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "old.txt", 0, 0, 0, 0, false,
                                set, &count));
    memcpy(media[40], set, count * EXFAT_ENTRY_BYTES);
}

static int sink(void *context, u64 offset, const u8 *data, usize length)
{
    memcpy((u8 *)context + offset, data, length);
    return VFS_OK;
}

static u32 replace_source(void *context, u64 offset, u8 *buffer, u32 requested)
{
    memcpy(buffer, (const u8 *)context + offset, requested);
    return requested;
}
static u32 short_source(void *context, u64 offset, u8 *buffer, u32 requested)
{
    (void)context; (void)offset; (void)buffer;
    return requested ? requested - 1U : 0U;
}

static void add_system_entries(void);
static void setup_large(void);
static void add_entry(u64 offset, const char *name, u16 attributes,
                      u32 first, u64 length, bool contiguous);
static void cleanup(void);

static void replacement_regressions(void)
{
    setup();
    add_system_entries();
    vfs_node_t *file = exfat_finddir(&root, "old.txt");
    assert(file && file->inode != root.inode);
    assert(exfat_finddir(&root, "old.txt") == file);
    u64 pin = 0;
    unsigned before = writes;
    assert(exfat_replace_locked(&super, file->inode, 1U, 3U,
                                replace_source, "new", &pin) == VFS_OK);
    assert(pin && pin_active && !pin_unresolved &&
           fs.replace_transaction && writes > before && file->size == 3U);
    assert(!fs.version_confident);
    assert(!memcmp(fs.replace_transaction->old_sector + 2U * EXFAT_ENTRY_BYTES,
                   fs.replace_transaction->proposed_sector +
                       2U * EXFAT_ENTRY_BYTES,
                   EXFAT_SECTOR_BYTES - 2U * EXFAT_ENTRY_BYTES));
    assert(exfat_entry_checksum(fs.replace_transaction->entry_set,
                                 ((exfat_node_t *)file->fs_data)->entry_count) ==
           exfat_le16(fs.replace_transaction->proposed_sector + 2U));
    assert(exfat_le32(fs.replace_transaction->proposed_sector +
                      EXFAT_ENTRY_BYTES + 20U) ==
           ((exfat_node_t *)file->fs_data)->first_cluster);
    finish_pin();
    assert(!fs.replace_transaction && fs.version_confident);
    vfs_file_facts_t facts;
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 2U && facts.size == 3U);
    u8 data[4] = {0};
    assert(exfat_node_read(file, 0, 3U, data) == 3U &&
           !memcmp(data, "new", 3U));
    before = writes;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_STALE_GENERATION);
    assert(writes == before);
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 2U, 0U,
                                NULL, NULL, &pin) == VFS_OK);
    assert(pin && file->size == 0U &&
           fs.replace_transaction->old_count == 1U);
    finish_pin();
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 3U && facts.size == 0U);
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    pin = 0;
    before = writes;
    assert(exfat_replace_locked(&super, file->inode, 1U, 3U,
                                short_source, NULL, &pin) == VFS_ERR_IO);
    assert(pin && pin_active && !pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_HANDOFF_CANCELLED);
    finish_pin();
    assert(!fs.replace_transaction && fs.version_confident &&
           file->size == 0U && writes > before);
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_allocation = allocations + 1U;
    before = writes;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) == VFS_ERR_NO_MEM);
    assert(!pin && !pin_active && writes == before && fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_read_lba = 40U;
    before = writes;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) == VFS_ERR_IO);
    assert(!pin_active && writes == before && fs.version_confident);
    fail_read_lba = ~(u64)0;
    device.id++;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_DEVICE_GONE);
    assert(!pin_active && writes == before);
    cleanup();

    setup();
    add_system_entries();
    add_entry(0U, "old.txt", 0, 5U, 1U, true);
    media[41][0] |= (u8)(1U << (5U - 2U));
    media[43][0] = 'o';
    add_entry(160U, "shared", 0, 5U, 1U, true);
    file = exfat_finddir(&root, "old.txt");
    before = writes;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_BAD_FORMAT);
    assert(!pin_active && writes == before && !fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    /* Primary at the end of one sector, stream in the next cluster. */
    u8 crossing_set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 crossing_count;
    assert(exfat_build_file_set(&fs, "old.txt", 0, 0, 0, 0, false,
                                crossing_set, &crossing_count));
    memset(media[40], 1, EXFAT_SECTOR_BYTES);
    memset(media[40] + 96U, 0, 2U * EXFAT_ENTRY_BYTES);
    add_system_entries();
    memcpy(media[40] + 480U, crossing_set, EXFAT_ENTRY_BYTES);
    memcpy(media[45], crossing_set + EXFAT_ENTRY_BYTES,
           ((usize)crossing_count - 1U) * EXFAT_ENTRY_BYTES);
    ((u32 *)media[24])[2] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[41][0] |= (u8)(1U << (7U - 2U));
    root_state.data_length = root_state.valid_data_length = root.size = 1024U;
    file = exfat_finddir(&root, "old.txt");
    assert(file);
    before = writes;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_UNSUPPORTED);
    assert(!pin && !pin_active && writes == before && fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_bitmap_write_number = 1U;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_STAGING &&
           fs.replace_transaction->fresh_count == 1U &&
           fs.replace_transaction->metadata_pending &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_BITMAP);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_NO_SPACE);
    assert(pin && pin_active && !pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->fresh_count == 0U &&
           fs.replace_transaction->phase == EXFAT_REPLACE_HANDOFF_CANCELLED);
    finish_pin();
    assert(file->size == 0U && fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_write_lba = 43U; /* First fresh data sector, torn callback. */
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_STAGING &&
           fs.replace_transaction->fresh_count == 1U && file->size == 0U);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    add_entry(0U, "old.txt", 0, 5U, 1U, true);
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    media[41][0] &= (u8)~(1U << (6U - 2U));
    media[41][0] &= (u8)~(1U << (8U - 2U));
    media[43][0] = 'o';
    fs.boot.fat_count = 2U;
    memcpy(media[25], media[24], EXFAT_SECTOR_BYTES);
    file = exfat_finddir(&root, "old.txt");
    u8 linked_content[1024];
    memset(linked_content, 'L', sizeof(linked_content));
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U,
                                sizeof(linked_content), replace_source,
                                linked_content, &pin) == VFS_OK);
    assert(!fs.replace_transaction->new_no_fat &&
           fs.replace_transaction->old_count == 1U);
    finish_pin();
    assert(exfat_node_read(file, 1023U, 1U, data) == 1U && data[0] == 'L');
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 2U, 1U,
                                replace_source, "C", &pin) == VFS_OK);
    assert(fs.replace_transaction->new_no_fat &&
           fs.replace_transaction->old_count == 2U);
    finish_pin();
    assert(exfat_node_read(file, 0U, 1U, data) == 1U && data[0] == 'C');
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    u8 partial[513];
    memset(partial, 'p', sizeof(partial));
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U,
                                sizeof(partial), replace_source, partial,
                                &pin) == VFS_OK);
    u32 last_cluster = fs.replace_transaction->fresh_clusters[1U];
    assert(media[exfat_cluster_lba(&fs, last_cluster)][0] == 'p');
    for (u32 i = 1U; i < EXFAT_SECTOR_BYTES; i++)
        assert(media[exfat_cluster_lba(&fs, last_cluster)][i] == 0U);
    finish_pin();
    cleanup();

    setup();
    add_system_entries();
    /* Only two nonadjacent free clusters: require a FAT-linked stream. */
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    media[41][0] &= (u8)~(1U << (6U - 2U));
    media[41][0] &= (u8)~(1U << (8U - 2U));
    fs.boot.fat_count = 2U;
    memcpy(media[25], media[24], EXFAT_SECTOR_BYTES);
    file = exfat_finddir(&root, "old.txt");
    fail_write_lba = 25U;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1024U,
                                replace_source, "", &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_STAGING &&
           fs.replace_transaction->fat_copy_progress == 1U &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_FAT &&
           fs.replace_transaction->fresh_count == 1U && file->size == 0U);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_flush = true;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_FLUSHING &&
           file->size == 1U && !fs.version_confident);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_pin_begin = true;
    before = writes;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) == VFS_ERR_NO_SPACE);
    assert(!pin && !pin_active && writes == before && fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    file = exfat_finddir(&root, "old.txt");
    fail_write_lba = 40U; /* Torn publication, after fresh data write. */
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "x", &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_PUBLISHING &&
           fs.replace_transaction->fresh_count == 1U &&
           fs.replace_transaction->reclamation_cursor == 0U &&
           !fs.version_confident);
    assert(exfat_node_read(file, 0, 1U, data) == 0U);
    device.id++; /* Forced removal cannot free pinned transaction memory. */
    assert(fs.replace_transaction->old_sector[0] == EXFAT_ENTRY_FILE);
    discard_unresolved_fixture_pin();
    cleanup();

    setup_large();
    file = exfat_finddir(&root, "old.txt");
    assert(file);
    u8 *half_meg = malloc(512U * 1024U);
    assert(half_meg);
    memset(half_meg, 'h', 512U * 1024U);
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 512U * 1024U,
                                replace_source, half_meg, &pin) == VFS_OK);
    assert(fs.replace_transaction->fresh_count == 1024U);
    finish_pin();
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.size == 512U * 1024U);
    free(half_meg);
    cleanup();

    setup_large();
    file = exfat_finddir(&root, "old.txt");
    assert(file);
    usize baseline_requested = live_requested;
    peak_requested = baseline_requested;
    u8 *large = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(large);
    memset(large, 'L', MG_FILE_CONTENT_MAX_BYTES);
    large[MG_FILE_CONTENT_MAX_BYTES - 1U] = 'Z';
    before = writes;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U,
                                MG_FILE_CONTENT_MAX_BYTES + 1ULL,
                                replace_source, large, &pin) == VFS_ERR_NO_SPACE);
    assert(!pin && writes == before);
    assert(exfat_replace_locked(&super, file->inode, 1U,
                                MG_FILE_CONTENT_MAX_BYTES,
                                replace_source, large, &pin) == VFS_OK);
    assert(pin_active && fs.replace_transaction &&
           fs.replace_transaction->fresh_count == 4096U &&
           fs.replace_transaction->new_no_fat);
    printf("exFAT contiguous 2 MiB replacement: peak=%zu retained=%zu bytes, %u reads, %u writes\n",
           (size_t)(peak_requested - baseline_requested),
           (size_t)(live_requested - baseline_requested), reads, writes);
    finish_pin();
    assert(file->size == MG_FILE_CONTENT_MAX_BYTES);
    assert(exfat_node_read(file, MG_FILE_CONTENT_MAX_BYTES - 1U, 1U,
                           data) == 1U && data[0] == 'Z');
    u32 previous_first = ((exfat_node_t *)file->fs_data)->first_cluster;
    peak_requested = live_requested;
    baseline_requested = live_requested;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 2U,
                                MG_FILE_CONTENT_MAX_BYTES,
                                replace_source, large, &pin) == VFS_OK);
    assert(fs.replace_transaction->old_count == 4096U &&
           fs.replace_transaction->fresh_count == 4096U);
    printf("exFAT 2 MiB old/new replacement: peak=%zu retained=%zu bytes\n",
           (size_t)(peak_requested - baseline_requested),
           (size_t)(live_requested - baseline_requested));
    finish_pin();
    bool old_used;
    assert(exfat_bitmap_bit(&fs, previous_first, &old_used) && !old_used);
    assert(exfat_file_facts(file, &facts) == VFS_OK && facts.generation == 3U);
    free(large);
    cleanup();

    setup_large();
    for (u32 cluster = 8U; cluster <= 10001U; cluster += 2U) {
        u32 bit = cluster - 2U;
        media[121U + bit / (EXFAT_SECTOR_BYTES * 8U)]
             [(bit / 8U) % EXFAT_SECTOR_BYTES] |=
             (u8)(1U << (bit % 8U));
    }
    file = exfat_finddir(&root, "old.txt");
    assert(file);
    large = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(large);
    memset(large, 'F', MG_FILE_CONTENT_MAX_BYTES);
    baseline_requested = live_requested;
    peak_requested = baseline_requested;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U,
                                MG_FILE_CONTENT_MAX_BYTES,
                                replace_source, large, &pin) == VFS_OK);
    assert(pin_active && fs.replace_transaction &&
           fs.replace_transaction->fresh_count == 4096U &&
           !fs.replace_transaction->new_no_fat);
    printf("exFAT fragmented 2 MiB replacement: peak=%zu retained=%zu bytes, %u reads, %u writes\n",
           (size_t)(peak_requested - baseline_requested),
           (size_t)(live_requested - baseline_requested), reads, writes);
    finish_pin();
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 2U && facts.size == MG_FILE_CONTENT_MAX_BYTES);
    assert(exfat_node_read(file, MG_FILE_CONTENT_MAX_BYTES - 1U,
                           1U, data) == 1U && data[0] == 'F');
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 2U, 1U,
                                replace_source, "N", &pin) == VFS_OK);
    assert(fs.replace_transaction->new_no_fat);
    finish_pin();
    assert(exfat_node_read(file, 0U, 1U, data) == 1U && data[0] == 'N');
    free(large);
    cleanup();

    setup();
    add_system_entries();
    add_entry(0, "old.txt", 0, 5, 1, true);
    media[41][0] |= 0x08U;
    media[43][0] = 'o';
    file = exfat_finddir(&root, "old.txt");
    fail_bitmap_write_number = 2U;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
                                replace_source, "n", &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved &&
           fs.replace_transaction &&
           fs.replace_transaction->phase == EXFAT_REPLACE_RECLAIMING &&
           fs.replace_transaction->old_count == 1U &&
           fs.replace_transaction->fresh_count == 1U &&
           fs.replace_transaction->reclamation_cursor == 0U &&
           file->size == 1U && !fs.version_confident);
    discard_unresolved_fixture_pin();
    cleanup();
}

static void cleanup(void)
{
    for (exfat_node_t *state = fs.nodes; state;) {
        exfat_node_t *next = state->next;
        if (state != &root_state) {
            kfree(state->vfs_node);
            kfree(state);
        }
        state = next;
    }
    assert(live_requested == 0U);
}

static void add_system_entries(void)
{
    u8 *bitmap = media[40] + 96U;
    u8 *upcase = media[40] + 128U;
    bitmap[0] = EXFAT_ENTRY_BITMAP;
    exfat_put32(bitmap + 20U, 3U);
    exfat_put64(bitmap + 24U, 512U);
    upcase[0] = EXFAT_ENTRY_UPCASE;
    exfat_put32(upcase + 20U, 4U);
    exfat_put64(upcase + 24U, 512U);
    fs.upcase_cluster = 4U;
    fs.upcase_length = 512U;
    media[41][0] |= 4U;
}

static void setup_large(void)
{
    setup();
    device.sector_count = 12000U;
    fs.boot.cluster_count = 10000U;
    fs.boot.volume_length = device.sector_count;
    fs.boot.fat_length = 79U;
    fs.boot.heap_offset = 120U;
    fs.bitmap_length = 1536U;
    fs.upcase_cluster = 6U;
    fs.upcase_length = 512U;
    memset(media[40], 0, EXFAT_SECTOR_BYTES);
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "old.txt", 0, 0, 0, 0, false,
                                set, &count));
    memcpy(media[120], set, count * EXFAT_ENTRY_BYTES);
    u8 *bitmap = media[120] + 96U;
    u8 *upcase = media[120] + 128U;
    bitmap[0] = EXFAT_ENTRY_BITMAP;
    exfat_put32(bitmap + 20U, 3U);
    exfat_put64(bitmap + 24U, fs.bitmap_length);
    upcase[0] = EXFAT_ENTRY_UPCASE;
    exfat_put32(upcase + 20U, 6U);
    exfat_put64(upcase + 24U, fs.upcase_length);
    media[121][0] = 0x1fU; /* root, three bitmap clusters, upcase */
    ((u32 *)media[24])[2] = EXFAT_CLUSTER_EOC;
}

static void add_entry(u64 offset, const char *name, u16 attributes,
                      u32 first, u64 length, bool contiguous)
{
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, name, attributes, first, length,
                                length, contiguous, set, &count));
    assert(offset + (u64)count * EXFAT_ENTRY_BYTES <= 512U);
    memcpy(media[40] + offset, set, count * EXFAT_ENTRY_BYTES);
}

static void ownership_regressions(void)
{
    unsigned initial;
    setup();
    add_system_entries();
    initial = writes;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    printf("exFAT minimal ownership: %u requested bytes, %u reads\n",
           (unsigned)(((fs.boot.cluster_count + 7U) / 8U) +
           sizeof(exfat_scan_directory_t) * 16U), reads);
    vfs_node_t *owned_file = exfat_finddir(&root, "old.txt");
    assert(owned_file && exfat_ownership_preflight_locked(owned_file) == VFS_OK);
    ((exfat_node_t *)owned_file->fs_data)->entry_offset = 256U;
    assert(exfat_ownership_preflight_locked(owned_file) == VFS_ERR_NOT_FOUND);
    ((exfat_node_t *)owned_file->fs_data)->entry_offset = 0U;
    assert(writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "one", 0, 5, 600, false);
    ((u32 *)media[24])[5] = 9U;
    ((u32 *)media[24])[9] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x88U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    add_entry(256, "two", 0, 9, 1, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "short", 0, 5, 1024, false);
    media[41][0] |= 0x08U;
    ((u32 *)media[24])[5] = EXFAT_CLUSTER_EOC;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "invalid", 0, 5, 1024, false);
    media[41][0] |= 0x08U;
    ((u32 *)media[24])[5] = 100U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "folder", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    media[41][0] |= 0x08U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    add_entry(256, "alias", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "folder", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    add_entry(256, "file", 0, 5, 1, true);
    media[41][0] |= 0x08U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "folder", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    media[41][0] |= 0x18U;
    u8 nested[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 nested_count;
    assert(exfat_build_file_set(&fs, "inside", 0, 6, 1, 1, true,
                                nested, &nested_count));
    memcpy(media[43], nested, nested_count * EXFAT_ENTRY_BYTES);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "linked", EXFAT_ATTR_DIRECTORY, 5, 1024, false);
    ((u32 *)media[24])[5] = 6U;
    ((u32 *)media[24])[6] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x38U;
    u8 linked_child[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 linked_count;
    assert(exfat_build_file_set(&fs, "child", 0, 7, 1, 1, true,
                                linked_child, &linked_count));
    memcpy(media[43], linked_child, linked_count * EXFAT_ENTRY_BYTES);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    add_system_entries();
    ((u32 *)media[24])[2] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x28U; /* root tail and nested directory */
    for (u32 offset = 160U; offset < 448U; offset += EXFAT_ENTRY_BYTES)
        media[40][offset] = EXFAT_ENTRY_DELETED;
    u8 crossing[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 crossing_count;
    assert(exfat_build_file_set(&fs, "cross", EXFAT_ATTR_DIRECTORY,
                                5, 512, 512, true, crossing, &crossing_count));
    assert(crossing_count == 3U);
    memcpy(media[40] + 448U, crossing, 64U);
    memcpy(media[45], crossing + 64U, 32U);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    printf("exFAT linked-root crossing entry: %u reads, no writes\n", reads);
    assert(exfat_finddir(&root, "cross") != NULL);
    cleanup();

    setup();
    add_system_entries();
    ((u32 *)media[24])[2] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x20U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "meta", 0, 4, 1, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    exfat_put32(media[40] + 128U + 20U, 3U);
    fs.upcase_cluster = 3U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "range", 0, 64U, 1024, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "free", 0, 5, 1, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "loop", 0, 5, 1024, false);
    media[41][0] |= 0x18U;
    ((u32 *)media[24])[5] = 6U;
    ((u32 *)media[24])[6] = 5U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    media[40][2] ^= 1U; /* Primary entry-set checksum. */
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_allocation = 1;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_NO_MEM);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_allocation = 2; /* Directory queue after ownership bitmap. */
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_NO_MEM);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fs.boot.cluster_count = EXFAT_SCAN_MAX_BITMAP_BYTES * 8U + 1U;
    assert(exfat_ownership_preflight_locked(&root) ==
           VFS_ERR_BUFFER_TOO_SMALL);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_read_lba = 40;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_IO);
    assert(fs.version_confident && writes == initial);
    fail_read_lba = ~(u64)0;
    device.id++;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_DEVICE_GONE);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_read_lba = 41; /* Allocation bitmap, before namespace traversal. */
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_IO);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    replace_after_reads = 4;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_IO);
    assert(fs.version_confident && writes == initial);
    cleanup();
}

int main(void)
{
    setup();
    vfs_node_t *file = exfat_finddir(&root, "old.txt");
    assert(file && file->inode != root.inode);
    assert(exfat_finddir(&root, "old.txt") == file);
    vfs_file_facts_t facts;
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 1 && facts.size == 0);
    assert(exfat_node_write(file, 0, 5, "hello") == 5);
    assert(file->size == 5 && exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 2);
    u8 data[8] = {0};
    mg_file_lines_state_t snap;
    assert(exfat_snapshot_stream(file, sizeof(data), sink, data, &snap) == VFS_OK);
    assert(snap.identifier == file->inode && snap.revision == 2 &&
           snap.length == 5 && !memcmp(data, "hello", 5));
    assert(exfat_rename(&root, "old.txt", &root, "new.txt") == VFS_OK);
    assert(exfat_finddir(&root, "new.txt") == file);
    assert(exfat_file_facts(file, &facts) == VFS_OK && facts.generation == 3);
    assert(exfat_truncate(file) == VFS_OK && file->size == 0);
    assert(exfat_file_facts(file, &facts) == VFS_OK && facts.generation == 4);
    assert(exfat_unlink(&root, "new.txt") == VFS_OK);
    assert(exfat_file_facts(file, &facts) == VFS_ERR_NOT_FOUND);
    vfs_node_t *new_file = NULL;
    assert(exfat_create(&root, "again.txt", &new_file) == VFS_OK);
    assert(new_file && new_file->inode != file->inode);
    assert(exfat_finddir(&root, "again.txt") == new_file);
    vfs_node_t *second_empty = NULL;
    assert(exfat_create(&root, "second.txt", &second_empty) == VFS_OK);
    assert(second_empty && second_empty->inode != new_file->inode &&
           exfat_finddir(&root, "second.txt") == second_empty);
    assert(exfat_node_read(file, 0, 1, data) == 0);
    fail_write = true;
    assert(exfat_node_write(new_file, 0, 1, "x") == 0);
    assert(!fs.version_confident);
    assert(exfat_file_facts(new_file, &facts) == VFS_ERR_UNCERTAIN);
    unsigned attempted = writes;
    device.id++;
    assert(!exfat_read(&fs, 40, 1, data));
    assert(writes == attempted);
    cleanup();
    setup();
    exfat_entry_info_t first = {0}, second = {0};
    first.parent_cluster = second.parent_cluster = 2;
    first.entry_count = second.entry_count = 3;
    first.first_cluster = second.first_cluster = 4;
    first.data_length = second.data_length = 1;
    second.entry_offset = 96;
    strcpy(first.name, "one");
    strcpy(second.name, "two");
    assert(exfat_make_node(&root, &first));
    assert(!exfat_make_node(&root, &second));
    assert(!fs.version_confident);
    cleanup();
    setup();
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "bad.txt", 0, 4, 1, 1, true,
                                set, &count));
    memcpy(media[40], set, count * EXFAT_ENTRY_BYTES);
    vfs_node_t *bad = exfat_finddir(&root, "bad.txt");
    assert(bad && exfat_file_facts(bad, &facts) == VFS_ERR_BAD_FORMAT &&
           !fs.version_confident); /* cluster 4 is marked free */
    cleanup();
    setup();
    assert(exfat_build_file_set(&fs, "loop.txt", 0, 4, 1024, 1024, false,
                                set, &count));
    memcpy(media[40], set, count * EXFAT_ENTRY_BYTES);
    media[41][0] |= 0x0c; /* clusters 4 and 5 allocated */
    ((u32 *)media[24])[4] = 5;
    ((u32 *)media[24])[5] = 4;
    vfs_node_t *loop = exfat_finddir(&root, "loop.txt");
    assert(loop && exfat_file_facts(loop, &facts) == VFS_ERR_BAD_FORMAT);
    cleanup();
    setup();
    vfs_node_t *retry = exfat_finddir(&root, "old.txt");
    fail_read_lba = 40;
    assert(exfat_file_facts(retry, &facts) == VFS_ERR_IO &&
           fs.version_confident);
    fail_read_lba = ~(u64)0;
    assert(exfat_file_facts(retry, &facts) == VFS_OK);
    cleanup();
    setup();
    unsigned before = writes;
    fail_allocation = 1; /* identity reservation before volume dirtying */
    vfs_node_t *unpublished = NULL;
    assert(exfat_create(&root, "new.txt", &unpublished) == VFS_ERR_NO_MEM);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_read_lba = 40;
    assert(exfat_create(&root, "new.txt", &unpublished) == VFS_ERR_IO);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_write = true; /* Torn dirty-bit write before namespace mutation. */
    assert(exfat_create(&root, "new.txt", &unpublished) ==
           VFS_ERR_UNCERTAIN);
    assert(!unpublished && writes == before + 1U &&
           !fs.version_confident);
    cleanup();
    ownership_regressions();
    replacement_regressions();
    puts("exFAT mount identities, generations, snapshots and ownership passed");
    return 0;
}
