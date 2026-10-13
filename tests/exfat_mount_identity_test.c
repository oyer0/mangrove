/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production exFAT functions with an in-memory mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../kernel/src/storage/exfat.c"

static u8 media[12000][512];
static u8 upcase_fixture[512];
static block_device_t device;
static exfat_fs_t fs;
static vfs_super_t super;
static vfs_node_t root;
static exfat_node_t root_state;
static bool online;
static bool fail_write;
static bool fail_flush;
static unsigned fail_flush_number, remove_after_write_number;
static u64 fail_write_lba;
static unsigned fail_write_number;
static unsigned fail_bitmap_write_number, bitmap_writes;
static unsigned writes;
static unsigned flushes;
static exfat_replace_phase_t fail_write_phase;
static usize fail_write_copy_bytes;
static unsigned allocations, fail_allocation;
static usize live_requested, peak_requested;
typedef union { long double align; usize bytes; } allocation_header_t;
static u64 fail_read_lba;
static unsigned reads;
static unsigned replace_after_reads;
static bool pin_active, pin_unresolved, fail_pin_begin;
static bool allow_write = true;
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
bool vfs_check_access(const vfs_node_t *node, u32 permission)
{ return node && allow_write && permission == VFS_ACCESS_WRITE; }
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
        !count || lba >= device.sector_count ||
        count > device.sector_count - lba)
        return false;
    for (u32 index = 0; index < count; index++) {
        if (lba + index == fail_read_lba) return false;
        reads++;
        memcpy((u8 *)buffer + (usize)index * 512U,
               media[lba + index], 512U);
        if (reads == replace_after_reads) device.id++;
    }
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
    if (!online || dev != &device || id != device.id ||
        !count || lba >= device.sector_count ||
        count > device.sector_count - lba) return false;
    for (u32 index = 0; index < count; index++) {
        u64 sector = lba + index;
        const u8 *bytes = (const u8 *)buffer + (usize)index * 512U;
        writes++;
        if (sector == fs.boot.heap_offset + 1U) bitmap_writes++;
        if (fail_write || sector == fail_write_lba ||
            writes == fail_write_number ||
            (fail_write_phase && fs.replace_transaction &&
             fs.replace_transaction->phase == fail_write_phase &&
             (fail_write_phase != EXFAT_CREATE_DIRECTORY_PREPARING ||
              (fs.replace_transaction->directory_growth &&
               fs.replace_transaction->directory_growth->target_attempted))) ||
            (sector == fs.boot.heap_offset + 1U &&
             fail_bitmap_write_number &&
             bitmap_writes == fail_bitmap_write_number)) {
            memcpy(media[sector], bytes, fail_write_copy_bytes);
            return false;
        }
        memcpy(media[sector], bytes, 512U);
        if (writes == remove_after_write_number) device.id++;
    }
    return true;
}
bool block_flush_for_id(block_device_t *dev, u64 id)
{ flushes++; return !fail_flush && flushes != fail_flush_number &&
    online && dev == &device && id == device.id; }
bool block_device_id_is_live(u64 id)
{ return online && id == device.id; }

/* Formatter/geometry paths use the public block callbacks; the backing media
 * is still disposable host memory, not a real device or a guest. */
bool block_read(block_device_t *dev, u64 lba, u32 count, void *buffer)
{
    for (u32 i = 0; i < count; i++)
        if (!block_read_for_id(dev, dev->id, lba + i,
                               1U, (u8 *)buffer + i * 512U)) return false;
    return true;
}
bool block_write(block_device_t *dev, u64 lba, u32 count, const void *buffer)
{
    for (u32 i = 0; i < count; i++)
        if (!block_write_for_id(dev, dev->id, lba + i,
                                1U, (const u8 *)buffer + i * 512U)) return false;
    return true;
}
bool block_device_is_live(const block_device_t *dev)
{ return online && dev == &device; }
bool block_flush(block_device_t *dev)
{ return block_flush_for_id(dev, dev->id); }
bool entropy_random_u64(u64 *out) { *out = 123U; return true; }
u64 timekeeping_boot_id(void) { return 1U; }
u64 timekeeping_monotonic_ms(void) { return 0U; }

static void setup(void)
{
    memset(media, 0, sizeof(media));
    memset(&fs, 0, sizeof(fs));
    memset(&super, 0, sizeof(super));
    memset(&root, 0, sizeof(root));
    memset(&root_state, 0, sizeof(root_state));
    device.id = 93;
    device.sector_size = 512U;
    device.sector_count = 128;
    online = true;
    fail_write = false;
    fail_write_number = 0;
    fail_flush = false;
    fail_flush_number = remove_after_write_number = 0;
    fail_write_lba = ~(u64)0;
    fail_bitmap_write_number = bitmap_writes = 0;
    allocations = fail_allocation = 0;
    live_requested = peak_requested = 0;
    fail_read_lba = ~(u64)0;
    writes = 0;
    flushes = 0;
    fail_write_phase = 0;
    fail_write_copy_bytes = 128U;
    reads = 0;
    replace_after_reads = 0;
    pin_active = pin_unresolved = fail_pin_begin = false;
    allow_write = true;
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
    for (u32 i = 0; i < 128U; i++)
        exfat_put16(upcase_fixture + i * 2U,
                    (u16)(i >= 'a' && i <= 'z' ? i - 'a' + 'A' : i));
    fs.upcase = upcase_fixture;
    fs.upcase_length = sizeof(upcase_fixture);
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

static vfs_node_t *prepared_created_node(void)
{
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(*node));
    assert(node);
    memset(node, 0, sizeof(*node));
    return node;
}

static void bind_created_node(vfs_node_t *node,
                              const vfs_file_facts_t *facts)
{
    assert(node->fs_data && facts->record_id == node->inode);
    node->type = VFS_TYPE_FILE;
    node->size = facts->size;
    node->owner_uid = facts->owner_uid;
    node->permissions = facts->permissions;
    node->super = &super;
    node->ops = &exfat_node_ops;
}

static void create_contents_case(u32 length)
{
    setup_large();
    vfs_node_t *node = prepared_created_node();
    vfs_file_facts_t facts;
    u8 *input = length ? malloc(length) : NULL;
    if (length) { assert(input); memset(input, 'C', length); }
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned initial_reads = reads, initial_writes = writes;
    u64 pin = 0;
    assert(exfat_create_contents_locked(&root, node, "Created.txt", length,
                                        length ? replace_source : NULL, input,
                                        &facts, &pin) == VFS_OK);
    assert(pin && pin_active && !pin_unresolved &&
           fs.replace_transaction && fs.replace_transaction->creating &&
           fs.replace_transaction->phase == EXFAT_REPLACE_HANDOFF_PUBLISHED &&
           fs.replace_transaction->fresh_count ==
               (length + 511U) / 512U &&
           facts.record_id == node->inode && facts.generation == 1U &&
           facts.size == length && !fs.version_confident);
    printf("exFAT %u-byte create: peak=%zu retained=%zu bytes, %u reads, %u writes\n",
           length, (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline),
           reads - initial_reads, writes - initial_writes);
    bind_created_node(node, &facts);
    finish_pin();
    assert(exfat_finddir(&root, "created.TXT") == node);
    exfat_entry_info_t on_disk;
    assert(exfat_entry_set_read(&fs, root_state.first_cluster, false,
                                ((exfat_node_t *)node->fs_data)->entry_offset,
                                &on_disk) &&
           !strcmp(on_disk.name, "Created.txt") &&
           on_disk.data_length == length &&
           on_disk.valid_data_length == length &&
           (on_disk.stream_flags & EXFAT_STREAM_ALLOCATION_POSSIBLE) &&
           !!(on_disk.stream_flags & EXFAT_STREAM_NO_FAT_CHAIN) ==
               (length != 0U));
    vfs_file_facts_t observed;
    assert(exfat_file_facts(node, &observed) == VFS_OK &&
           observed.record_id == facts.record_id && observed.size == length &&
           observed.generation == 1U);
    if (length) {
        u8 last = 0;
        assert(exfat_node_read(node, length - 1U, 1U, &last) == 1U &&
               last == 'C');
    }
    vfs_node_t spare = {0};
    initial_writes = writes;
    assert(exfat_create_contents_locked(&root, &spare, "CREATED.TXT", 0U,
                                        NULL, NULL, &observed, &pin) ==
           VFS_ERR_ALREADY_EXISTS);
    assert(!spare.fs_data && !pin_active && writes == initial_writes);
    free(input);
    cleanup();
}

static void create_contents_failures(void)
{
    setup();
    add_system_entries();
    vfs_node_t prepared = {0};
    vfs_file_facts_t facts;
    u64 pin = 0;
    unsigned before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt",
               MG_FILE_CONTENT_MAX_BYTES + 1ULL, replace_source, "x",
               &facts, &pin) == VFS_ERR_NO_SPACE);
    assert(exfat_create_contents_locked(&root, &prepared, "old.TXT", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(exfat_create_contents_locked(&root, &prepared, "bad/child", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_INVALID_PARAM);
    allow_write = false;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_ACCESS_DENIED);
    assert(!prepared.fs_data && !pin_active && writes == before);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160U, "shared", 0U, 2U, 1U, true);
    before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_BAD_FORMAT);
    assert(!pin_active && writes == before && !fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160U, "hidden.txt", EXFAT_ATTR_SYSTEM, 0U, 0U, false);
    before = writes;
    assert(!exfat_finddir(&root, "hidden.txt"));
    assert(exfat_create_contents_locked(&root, &prepared, "HIDDEN.TXT", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(!pin_active && writes == before && fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    media[40][2U] ^= 1U; /* Existing entry-set checksum is corrupt. */
    before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_BAD_FORMAT);
    assert(!pin_active && writes == before && !fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    vfs_node_t *unicode_node = prepared_created_node();
    assert(exfat_create_contents_locked(&root, unicode_node, "caf\xc3\xa9.txt",
               0U, NULL, NULL, &facts, &pin) == VFS_OK);
    bind_created_node(unicode_node, &facts);
    finish_pin();
    exfat_entry_info_t unicode_entry;
    assert(exfat_entry_set_read(&fs, root_state.first_cluster, false,
               ((exfat_node_t *)unicode_node->fs_data)->entry_offset,
               &unicode_entry) &&
           !strcmp(unicode_entry.name, "caf\xc3\xa9.txt") &&
           unicode_entry.name_units[3U] == 0x00e9U);
    cleanup();

    setup();
    add_system_entries();
    fail_allocation = allocations + 2U; /* After ownership bitmap. */
    before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_NO_MEM);
    assert(!prepared.fs_data && !pin_active && writes == before);
    cleanup();

    setup();
    add_system_entries();
    fail_pin_begin = true;
    before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_NO_SPACE);
    assert(!prepared.fs_data && !pin_active && writes == before);
    cleanup();

    setup();
    add_system_entries();
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 3U,
               short_source, NULL, &facts, &pin) == VFS_ERR_IO);
    assert(pin_active && !pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->phase == EXFAT_REPLACE_HANDOFF_CANCELLED);
    finish_pin();
    assert(!exfat_finddir(&root, "new.txt") && fs.version_confident);
    cleanup();

    setup();
    add_system_entries();
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_NO_SPACE);
    assert(pin_active && !pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->fresh_count == 0U &&
           media[40][160U] == EXFAT_ENTRY_END);
    finish_pin();
    assert(fs.version_confident && !exfat_finddir(&root, "new.txt") &&
           writes >= before);
    cleanup();

    setup();
    add_system_entries();
    fail_bitmap_write_number = 2U; /* Cancellation bitmap write tears. */
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               short_source, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->phase == EXFAT_REPLACE_CANCELLING &&
           fs.replace_transaction->fresh_count == 1U &&
           fs.replace_transaction->cancellation_cursor == 0U &&
           media[40][160U] == EXFAT_ENTRY_END);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    fail_write_lba = 40U; /* Torn namespace visibility sector. */
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction && fs.replace_transaction->creating &&
           fs.replace_transaction->phase == EXFAT_REPLACE_PUBLISHING &&
           fs.replace_transaction->fresh_count == 1U &&
           fs.replace_transaction->old_sector[160U] == EXFAT_ENTRY_END &&
           fs.replace_transaction->proposed_sector[160U] == EXFAT_ENTRY_FILE);
    device.id++;
    assert(fs.replace_transaction->pin_id == next_pin_id);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    /* The end marker has only two slots left in this sector.  Root growth
     * now stages the entry in a new cluster and releases this marker last. */
    for (u32 offset = 160U; offset < 448U; offset += EXFAT_ENTRY_BYTES)
        media[40][offset] = EXFAT_ENTRY_LABEL;
    before = writes;
    vfs_node_t *grown = prepared_created_node();
    assert(exfat_create_contents_locked(&root, grown, "new.txt", 0U,
               NULL, NULL, &facts, &pin) == VFS_OK);
    assert(pin_active && pin && fs.replace_transaction->directory_growth &&
           fs.replace_transaction->directory_growth->appended &&
           fs.replace_transaction->directory_growth->marker_attempted &&
           writes > before && root_state.data_length == 1024U &&
           exfat_le32(media[24] + 8U) ==
               fs.replace_transaction->directory_growth->candidate);
    bind_created_node(grown, &facts);
    finish_pin();
    assert(exfat_finddir(&root, "new.txt") == grown);
    cleanup();

    setup();
    add_system_entries();
    fail_write_lba = 41U; /* Torn first allocation-bitmap update. */
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->phase == EXFAT_REPLACE_STAGING &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_BITMAP &&
           fs.replace_transaction->fresh_count == 1U &&
           media[40][160U] == EXFAT_ENTRY_END);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    fail_write_lba = 43U; /* Bitmap succeeds; fresh data write tears. */
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->phase == EXFAT_REPLACE_STAGING &&
           fs.replace_transaction->fresh_count == 1U &&
           media[40][160U] == EXFAT_ENTRY_END);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    fail_flush = true;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 1U,
               replace_source, "x", &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->phase == EXFAT_REPLACE_FLUSHING &&
           fs.replace_transaction->proposed_sector[160U] == EXFAT_ENTRY_FILE &&
           media[40][160U] == EXFAT_ENTRY_FILE);
    discard_unresolved_fixture_pin();
    cleanup();

    setup();
    add_system_entries();
    /* No contiguous pair exists, so creation must publish a FAT chain. */
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    media[41][0] &= (u8)~(1U << (6U - 2U));
    media[41][0] &= (u8)~(1U << (8U - 2U));
    u8 linked[1024];
    memset(linked, 'L', sizeof(linked));
    vfs_node_t *linked_node = prepared_created_node();
    assert(exfat_create_contents_locked(&root, linked_node, "linked", 1024U,
               replace_source, linked, &facts, &pin) == VFS_OK);
    assert(pin && fs.replace_transaction &&
           !fs.replace_transaction->new_no_fat &&
           fs.replace_transaction->fresh_count == 2U);
    bind_created_node(linked_node, &facts);
    finish_pin();
    u8 last;
    assert(exfat_node_read(linked_node, 1023U, 1U, &last) == 1U && last == 'L');
    cleanup();

    setup();
    add_system_entries();
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    media[41][0] &= (u8)~(1U << (6U - 2U));
    media[41][0] &= (u8)~(1U << (8U - 2U));
    fs.boot.fat_count = 2U;
    memcpy(media[25], media[24], EXFAT_SECTOR_BYTES);
    unsigned before_unsupported = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "linked", 1024U,
               replace_source, linked, &facts, &pin) == VFS_ERR_UNSUPPORTED);
    assert(!pin && !pin_active && !fs.replace_transaction && !prepared.fs_data &&
           fs.version_confident && writes == before_unsupported);
    cleanup();

    setup();
    add_system_entries();
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    media[41][0] &= (u8)~(1U << (6U - 2U));
    media[41][0] &= (u8)~(1U << (8U - 2U));
    fail_write_lba = 24U; /* Torn write to the sole supported FAT. */
    assert(exfat_create_contents_locked(&root, &prepared, "linked", 1024U,
               replace_source, linked, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !prepared.fs_data &&
           !fs.version_confident && fs.replace_transaction &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_FAT &&
           fs.replace_transaction->fat_copy_progress == 0U);
    discard_unresolved_fixture_pin();
    cleanup();
}

static void prepare_root_growth(void)
{
    setup();
    add_system_entries();
    for (u32 offset = 160U; offset < 448U; offset += EXFAT_ENTRY_BYTES)
        media[40][offset] = EXFAT_ENTRY_LABEL;
}

static vfs_node_t *prepare_nonroot_growth(bool split_parent)
{
    setup();
    add_system_entries();
    ((u32 *)media[24])[5] = EXFAT_CLUSTER_EOC;
    media[41][0] |= (u8)(1U << (5U - 2U));
    if (split_parent) {
        u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
        u8 count;
        ((u32 *)media[24])[2] = 6U;
        ((u32 *)media[24])[6] = EXFAT_CLUSTER_EOC;
        media[41][0] |= (u8)(1U << (6U - 2U));
        root_state.data_length = root_state.valid_data_length = root.size =
            1024U;
        for (u32 at = 160U; at < 480U; at += EXFAT_ENTRY_BYTES)
            media[40][at] = EXFAT_ENTRY_DELETED;
        assert(exfat_build_file_set(&fs, "sub", EXFAT_ATTR_DIRECTORY,
                                    5U, 512U, 512U, false, set, &count) &&
               count == 3U);
        memcpy(media[40] + 480U, set, EXFAT_ENTRY_BYTES);
        memcpy(media[44], set + EXFAT_ENTRY_BYTES,
               2U * EXFAT_ENTRY_BYTES);
    } else {
        add_entry(160U, "sub", EXFAT_ATTR_DIRECTORY, 5U, 512U, false);
    }
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    for (u32 index = 0; index < 3U; index++) {
        char name[32];
        snprintf(name, sizeof(name), "abcdefghijklmnop%u", index);
        assert(exfat_build_file_set(&fs, name, 0U, 0U, 0U, 0U,
                                    false, set, &count) && count == 4U);
        memcpy(media[43] + index * 128U, set, 128U);
    }
    assert(exfat_build_file_set(&fs, "last", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[43] + 384U, set, 96U);
    vfs_node_t *sub = exfat_finddir(&root, "sub");
    assert(sub && sub->type == VFS_TYPE_DIRECTORY &&
           exfat_ownership_preflight_locked(sub) == VFS_OK);
    return sub;
}

static vfs_node_t *prepare_contiguous_directory(void)
{
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    setup();
    add_system_entries();
    add_entry(160U, "sub", EXFAT_ATTR_DIRECTORY, 5U, 1024U, true);
    add_entry(256U, "other", 0U, 7U, 512U, true);
    media[41][0] |= 0x38U; /* clusters 5, 6, and 7 */
    assert(exfat_build_file_set(&fs, "one", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[43], set, count * EXFAT_ENTRY_BYTES);
    for (u32 at = 96U; at < 512U; at += EXFAT_ENTRY_BYTES)
        media[43][at] = EXFAT_ENTRY_DELETED;
    assert(exfat_build_file_set(&fs, "two", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[44], set, count * EXFAT_ENTRY_BYTES);
    /* Cluster 7 belongs to an ordinary file. A stale contiguous cursor
     * must never interpret its bytes as a directory after conversion. */
    assert(exfat_build_file_set(&fs, "wrong", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[45], set, count * EXFAT_ENTRY_BYTES);
    vfs_node_t *sub = exfat_finddir(&root, "sub");
    assert(sub && sub->type == VFS_TYPE_DIRECTORY &&
           exfat_ownership_preflight_locked(sub) == VFS_OK);
    return sub;
}

static void contiguous_directory_mapping_regressions(void)
{
    u64 capacity = 0;
    vfs_dirent_t entry;
    void *cursor;
    unsigned old_reads, old_writes;

    vfs_node_t *sub = prepare_contiguous_directory();
    assert(exfat_directory_capacity(&fs, 5U, true, 1024U, &capacity) &&
           capacity == 1024U);
    old_reads = reads;
    assert(!exfat_directory_capacity(&fs, 5U, true, 0U, &capacity) &&
           !exfat_directory_capacity(&fs, 5U, true, 513U, &capacity) &&
           !exfat_directory_capacity(&fs, 5U, true, 62U * 512U, &capacity) &&
           !exfat_directory_capacity(&fs, 66U, true, 512U, &capacity) &&
           reads == old_reads);
    assert(exfat_directory_capacity(&fs, 5U, true, 61U * 512U,
                                    &capacity) && capacity == 61U * 512U);
    assert(exfat_finddir(sub, "two") &&
           exfat_readdir(sub, 1U, &entry) && !strcmp(entry.name, "two"));
    assert(exfat_readdir_open(sub, &cursor));
    assert(exfat_readdir_next(cursor, &entry) && !strcmp(entry.name, "one"));
    vfs_node_t *opened_child = exfat_finddir(sub, "one");
    assert(opened_child);

    /* Simulate a confirmed, operation-locked conversion that preserves the
     * original contiguous prefix and appends cluster 8 through the FAT. */
    ((u32 *)media[24])[5] = 6U;
    ((u32 *)media[24])[6] = 8U;
    ((u32 *)media[24])[8] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x40U;
    for (u32 at = 96U; at < 512U; at += EXFAT_ENTRY_BYTES)
        media[44][at] = EXFAT_ENTRY_DELETED;
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "three", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[46], set, count * EXFAT_ENTRY_BYTES);
    add_entry(160U, "sub", EXFAT_ATTR_DIRECTORY, 5U, 1536U, false);
    exfat_node_t *state = (exfat_node_t *)sub->fs_data;
    state->no_fat_chain = false;
    state->data_length = state->valid_data_length = sub->size = 1536U;
    exfat_version_advance(state);
    assert(exfat_readdir_next(cursor, &entry) && !strcmp(entry.name, "two") &&
           exfat_readdir_next(cursor, &entry) && !strcmp(entry.name, "three") &&
           !exfat_readdir_next(cursor, &entry));
    assert(exfat_finddir(sub, "three") &&
           exfat_ownership_preflight_locked(opened_child) == VFS_OK);
    exfat_readdir_close(cursor);
    printf("exFAT contiguous-directory cursor conversion: %u reads, %u writes\n",
           reads - old_reads, writes);
    cleanup();

    sub = prepare_contiguous_directory();
    /* Occupy the rest of the first cluster so ordinary creation must use
     * the existing second NoFatChain cluster. */
    char long_name[152];
    memset(long_name, 'a', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    assert(exfat_build_file_set(&fs, long_name, 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 13U);
    memcpy(media[43] + 96U, set, 416U);
    vfs_node_t *created = NULL;
    old_writes = writes;
    assert(exfat_create(sub, "new", &created) == VFS_OK && created &&
           ((exfat_node_t *)created->fs_data)->entry_offset == 512U + 96U &&
           ((exfat_node_t *)sub->fs_data)->data_length == 1024U &&
           exfat_finddir(sub, "new") == created &&
           media[45][0] == EXFAT_ENTRY_FILE && writes > old_writes);
    cleanup();

    sub = prepare_contiguous_directory();
    assert(exfat_readdir_open(sub, &cursor));
    online = false;
    old_reads = reads;
    assert(!exfat_readdir_next(cursor, &entry) && reads == old_reads);
    exfat_readdir_close(cursor);
    cleanup();

    sub = prepare_contiguous_directory();
    assert(exfat_readdir_open(sub, &cursor));
    ((exfat_node_t *)sub->fs_data)->alive = false;
    old_reads = reads;
    assert(!exfat_readdir_next(cursor, &entry) && reads == old_reads);
    exfat_readdir_close(cursor);
    cleanup();

    sub = prepare_contiguous_directory();
    state = (exfat_node_t *)sub->fs_data;
    old_reads = reads;
    state->data_length = 513U;
    assert(!exfat_readdir_open(sub, &cursor) &&
           !exfat_finddir(sub, "two") && reads == old_reads);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160U, "bad", EXFAT_ATTR_DIRECTORY, 5U, 513U, true);
    old_writes = writes;
    assert(!exfat_finddir(&root, "bad") &&
           exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT &&
           writes == old_writes);
    cleanup();

    setup();
    add_system_entries();
    assert(exfat_build_file_set(&fs, "invalid", EXFAT_ATTR_DIRECTORY,
                                5U, 512U, 1024U, true, set, &count));
    memcpy(media[40] + 160U, set, (usize)count * EXFAT_ENTRY_BYTES);
    old_writes = writes;
    assert(!exfat_finddir(&root, "invalid") &&
           exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT &&
           writes == old_writes);
    cleanup();

    sub = prepare_nonroot_growth(false);
    assert(exfat_readdir_open(sub, &cursor));
    for (u32 index = 0; index < 4U; index++)
        assert(exfat_readdir_next(cursor, &entry));
    vfs_node_t *grown = prepared_created_node();
    vfs_file_facts_t facts;
    u64 pin = 0;
    assert(exfat_create_contents_locked(sub, grown, "grown", 0U,
        NULL, NULL, &facts, &pin) == VFS_OK);
    bind_created_node(grown, &facts);
    finish_pin();
    assert(exfat_readdir_next(cursor, &entry) &&
           !strcmp(entry.name, "grown"));
    exfat_readdir_close(cursor);
    cleanup();
}

static vfs_node_t *prepare_contiguous_growth(bool blocked)
{
    vfs_node_t *sub = prepare_contiguous_directory();
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    for (u32 sector = 0; sector < 2U; sector++) {
        for (u32 index = 0; index < 3U; index++) {
            char name[32];
            snprintf(name, sizeof(name), "entry-sector%u-file%u",
                     sector, index);
            assert(exfat_build_file_set(&fs, name, 0U, 0U, 0U, 0U,
                                        false, set, &count) && count == 4U);
            memcpy(media[43U + sector] + 96U + index * 128U,
                   set, 128U);
        }
    }
    media[43][480U] = EXFAT_ENTRY_DELETED;
    if (!blocked) {
        /* Cluster 7 is the adjacent directory candidate. */
        media[41][0] &= (u8)~(1U << (7U - 2U));
        memset(media[40] + 256U, 0, 96U); /* remove "other" */
    }
    assert(exfat_ownership_preflight_locked(sub) == VFS_OK);
    return sub;
}

static void contiguous_growth_regressions(void)
{
    for (u32 blocked = 0; blocked < 2U; blocked++) {
        vfs_node_t *sub = prepare_contiguous_growth(blocked != 0U);
        vfs_node_t *old = exfat_finddir(sub, "one");
        vfs_node_t *created = prepared_created_node();
        vfs_file_facts_t facts;
        vfs_dirent_t entry;
        void *cursor;
        u64 pin = 0;
        usize baseline = live_requested;
        unsigned old_reads = reads, old_writes = writes,
                 old_flushes = flushes;
        clock_t start = clock();
        assert(old && exfat_readdir_open(sub, &cursor) &&
               exfat_readdir_next(cursor, &entry) &&
               !strcmp(entry.name, "one"));
        assert(exfat_create_contents_locked(sub, created, "grown", 1U,
            replace_source, "X", &facts, &pin) == VFS_OK);
        exfat_directory_growth_t *growth =
            fs.replace_transaction->directory_growth;
        assert(growth && growth->appended && growth->marker_written &&
               growth->conversion == (blocked != 0U) &&
               growth->contiguous_extension == (blocked == 0U) &&
               ((exfat_node_t *)sub->fs_data)->no_fat_chain ==
                   (blocked == 0U) &&
               ((exfat_node_t *)created->fs_data)->parent_no_fat_chain ==
                   (blocked == 0U));
        if (blocked)
            assert(growth->conversion_count == 1U &&
                   growth->conversion_written == 1U &&
                   exfat_le32(media[24] + 5U * 4U) == 6U &&
                   exfat_le32(media[24] + 6U * 4U) == growth->candidate &&
                   exfat_le32(media[24] + growth->candidate * 4U) ==
                       EXFAT_CLUSTER_EOC);
        bind_created_node(created, &facts);
        printf("exFAT contiguous growth blocked=%u: peak=%zu retained=%zu "
               "reads=%u writes=%u flushes=%u cpu_ms=%.3f\n", blocked,
               (size_t)(peak_requested - baseline),
               (size_t)(live_requested - baseline), reads - old_reads,
               writes - old_writes, flushes - old_flushes,
               1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC);
        finish_pin();
        assert(exfat_finddir(sub, "grown") == created &&
               exfat_finddir(sub, "one") == old &&
               exfat_finddir(sub, "two") &&
               exfat_ownership_preflight_locked(sub) == VFS_OK);
        bool saw_grown = false, saw_wrong = false;
        while (exfat_readdir_next(cursor, &entry)) {
            if (!strcmp(entry.name, "grown")) saw_grown = true;
            if (!strcmp(entry.name, "wrong")) saw_wrong = true;
        }
        assert(saw_grown && !saw_wrong);
        exfat_readdir_close(cursor);
        vfs_node_t *again = prepared_created_node();
        assert(exfat_create_contents_locked(sub, again, "again", 0U,
            NULL, NULL, &facts, &pin) == VFS_OK);
        bind_created_node(again, &facts);
        finish_pin();
        assert(exfat_finddir(sub, "again") == again);
        assert(exfat_unlink(sub, "again") == VFS_OK &&
               !exfat_finddir(sub, "again") &&
               exfat_ownership_preflight_locked(sub) == VFS_OK);
        cleanup();
    }
}

static void contiguous_conversion_faults(void)
{
    vfs_file_facts_t facts;
    u64 pin = 0;
    for (u32 fault = 0; fault < 7U; fault++) {
        vfs_node_t *sub = prepare_contiguous_growth(true);
        vfs_node_t *created = prepared_created_node();
        unsigned old_writes = writes;
        if (fault == 0U) fail_write_phase = EXFAT_CREATE_DIRECTORY_LINKING;
        if (fault == 1U) fail_flush_number = 2U;
        if (fault == 2U) fail_write_phase = EXFAT_CREATE_PARENT_UPDATING;
        if (fault == 3U) fail_flush_number = 3U;
        if (fault == 4U) fail_write_phase = EXFAT_CREATE_DIRECTORY_REVEALING;
        if (fault == 5U) fail_flush_number = 4U;
        if (fault == 6U) remove_after_write_number = old_writes + 7U;
        assert(exfat_create_contents_locked(sub, created, "failed", 0U,
            NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
        assert(!pin && !created->fs_data && pin_active && pin_unresolved &&
               !fs.version_confident && fs.replace_transaction &&
               fs.replace_transaction->directory_growth &&
               fs.replace_transaction->directory_growth->conversion &&
               fs.replace_transaction->directory_growth->conversion_sectors &&
               fs.replace_transaction->directory_growth->conversion_count == 1U &&
               ((exfat_node_t *)sub->fs_data)->no_fat_chain &&
               ((exfat_node_t *)sub->fs_data)->data_length == 1024U);
        exfat_conversion_sector_t *sector =
            fs.replace_transaction->directory_growth->conversion_sectors;
        if (fault == 0U)
            assert(sector->attempted && !sector->written &&
                   fs.replace_transaction->directory_growth->
                       conversion_written == 0U &&
                   exfat_le32(sector->before + 5U * 4U) == 0U &&
                   exfat_le32(sector->proposed + 5U * 4U) == 6U);
        if (fault >= 1U && fault <= 5U)
            assert(sector->attempted && sector->written &&
                   fs.replace_transaction->directory_growth->
                       conversion_written == 1U);
        if (fault == 6U)
            assert(sector->attempted && sector->written &&
                   !block_device_id_is_live(fs.device_id));
        kfree(created);
        discard_unresolved_fixture_pin();
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_contiguous_growth(true);
        vfs_node_t *created = prepared_created_node();
        unsigned old_writes = writes;
        assert(exfat_create_contents_locked(sub, created, "short", 1U,
            short_source, NULL, &facts, &pin) == VFS_ERR_IO);
        assert(pin_active && !pin_unresolved &&
               fs.replace_transaction->directory_growth->candidate_cancelled &&
               fs.replace_transaction->directory_growth->conversion_written == 0U &&
               exfat_le32(media[24] + 5U * 4U) == 0U &&
               exfat_le32(media[24] + 6U * 4U) == 0U &&
               media[44][480U] == EXFAT_ENTRY_END && writes > old_writes);
        finish_pin();
        kfree(created);
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_contiguous_growth(true);
        vfs_node_t *created = prepared_created_node();
        u8 input[1024];
        memset(input, 'F', sizeof(input));
        memset(media[41], 0xff, 8U);
        for (u32 cluster = 8U; cluster <= 11U; cluster++)
            if (cluster == 8U || cluster == 9U || cluster == 11U)
                media[41][(cluster - 2U) / 8U] &=
                    (u8)~(1U << ((cluster - 2U) % 8U));
        assert(exfat_create_contents_locked(sub, created, "shared", 1024U,
            replace_source, input, &facts, &pin) == VFS_OK);
        assert(fs.replace_transaction->directory_growth->conversion &&
               !fs.replace_transaction->new_no_fat &&
               fs.replace_transaction->directory_growth->candidate == 8U &&
               exfat_le32(media[24] + 5U * 4U) == 6U &&
               exfat_le32(media[24] + 6U * 4U) == 8U &&
               exfat_le32(media[24] + 9U * 4U) == 11U &&
               exfat_le32(media[24] + 11U * 4U) == EXFAT_CLUSTER_EOC);
        bind_created_node(created, &facts);
        finish_pin();
        u8 last = 0;
        assert(exfat_node_read(created, 1023U, 1U, &last) == 1U &&
               last == 'F' && exfat_ownership_preflight_locked(sub) == VFS_OK);
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_contiguous_growth(true);
        vfs_node_t *created = prepared_created_node();
        unsigned old_writes = writes;
        media[41][0] &= (u8)~(1U << (6U - 2U));
        assert(exfat_create_contents_locked(sub, created, "corrupt", 0U,
            NULL, NULL, &facts, &pin) == VFS_ERR_BAD_FORMAT);
        assert(!pin_active && !fs.replace_transaction &&
               writes == old_writes && !created->fs_data);
        kfree(created);
        cleanup();
    }
    for (u32 failure = 1U; failure <= 6U; failure++) {
        vfs_node_t *sub = prepare_contiguous_growth(true);
        vfs_node_t *created = prepared_created_node();
        unsigned old_writes = writes;
        fail_allocation = allocations + failure;
        int result = exfat_create_contents_locked(sub, created, "memory", 0U,
            NULL, NULL, &facts, &pin);
        assert(result == VFS_ERR_NO_MEM && !pin && !pin_active &&
               !fs.replace_transaction && !created->fs_data &&
               writes == old_writes && fs.version_confident);
        kfree(created);
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_contiguous_growth(true);
        exfat_replace_transaction_t tx = {0};
        tx.created_entry_count = 3U;
        unsigned old_writes = writes;
        assert(exfat_create_directory_growth_prepare(&fs,
            (exfat_node_t *)sub->fs_data, false,
            EXFAT_SCAN_MAX_ENTRIES, &tx) == VFS_ERR_BUFFER_TOO_SMALL);
        assert(writes == old_writes && !pin_active &&
               !tx.directory_growth->conversion_sectors);
        kfree(tx.directory_growth);
        cleanup();
    }
}

static vfs_node_t *prepare_split_contiguous_growth(void)
{
    vfs_node_t *sub = prepare_contiguous_growth(true);
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "sub", EXFAT_ATTR_DIRECTORY,
                                5U, 1024U, 1024U, true, set, &count) &&
           count == 3U);
    for (u32 at = 160U; at < 256U; at += EXFAT_ENTRY_BYTES)
        media[40][at] = EXFAT_ENTRY_DELETED;
    for (u32 at = 352U; at < 480U; at += EXFAT_ENTRY_BYTES)
        media[40][at] = EXFAT_ENTRY_DELETED;
    memcpy(media[40] + 480U, set, EXFAT_ENTRY_BYTES);
    memcpy(media[47], set + EXFAT_ENTRY_BYTES, 2U * EXFAT_ENTRY_BYTES);
    exfat_put32(media[24] + 2U * 4U, 9U);
    exfat_put32(media[24] + 9U * 4U, EXFAT_CLUSTER_EOC);
    media[41][0] |= (u8)(1U << (9U - 2U));
    root_state.data_length = root_state.valid_data_length = root.size = 1024U;
    ((exfat_node_t *)sub->fs_data)->entry_offset = 480U;
    assert(exfat_ownership_preflight_locked(sub) == VFS_OK);
    return sub;
}

static void split_contiguous_conversion(void)
{
    vfs_node_t *sub = prepare_split_contiguous_growth();
    vfs_node_t *created = prepared_created_node();
    vfs_file_facts_t facts;
    u64 pin = 0;
    assert(exfat_create_contents_locked(sub, created, "split", 0U,
        NULL, NULL, &facts, &pin) == VFS_OK);
    exfat_directory_growth_t *growth =
        fs.replace_transaction->directory_growth;
    assert(growth && growth->conversion &&
           growth->parent_primary_sector != growth->parent_stream_sector &&
           growth->parent_primary_written && growth->parent_stream_written &&
           exfat_le16(media[40] + 480U + 2U) ==
               exfat_entry_checksum(growth->parent_set_proposed,
                                    ((exfat_node_t *)sub->fs_data)->entry_count) &&
           !(media[47][1U] & EXFAT_STREAM_NO_FAT_CHAIN));
    bind_created_node(created, &facts);
    finish_pin();
    assert(exfat_finddir(&root, "sub") == sub &&
           exfat_finddir(sub, "split") == created &&
           exfat_ownership_preflight_locked(sub) == VFS_OK);
    cleanup();

    sub = prepare_split_contiguous_growth();
    created = prepared_created_node();
    fail_write_phase = EXFAT_CREATE_PARENT_UPDATING;
    assert(exfat_create_contents_locked(sub, created, "failed", 0U,
        NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_active && pin_unresolved && !pin &&
           fs.replace_transaction->directory_growth->conversion_written == 1U &&
           fs.replace_transaction->directory_growth->parent_stream_attempted &&
           !fs.replace_transaction->directory_growth->parent_primary_attempted &&
           ((exfat_node_t *)sub->fs_data)->no_fat_chain);
    kfree(created);
    discard_unresolved_fixture_pin();
    cleanup();
}

static vfs_node_t *prepare_multisector_conversion(void)
{
    setup_large();
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "sub", EXFAT_ATTR_DIRECTORY,
                                200U, 129U * 512U, 129U * 512U,
                                true, set, &count) && count == 3U);
    memcpy(media[120U] + 160U, set, 96U);
    assert(exfat_build_file_set(&fs, "adjacent", 0U, 329U,
                                512U, 512U, true, set, &count) &&
           count == 3U);
    memcpy(media[120U] + 256U, set, 96U);
    for (u32 cluster = 200U; cluster <= 328U; cluster++) {
        u64 lba = 120U + cluster - 2U;
        media[121U + (cluster - 2U) / 4096U]
             [((cluster - 2U) / 8U) % 512U] |=
            (u8)(1U << ((cluster - 2U) % 8U));
        exfat_put32(media[24U + cluster / 128U] +
                    (cluster % 128U) * 4U, 0x12345678U);
        for (u32 entry = 0; entry < (cluster == 328U ? 3U : 4U); entry++) {
            char name[48];
            snprintf(name, sizeof(name), "old-entry-%u-%u-long",
                     cluster - 200U, entry);
            assert(exfat_build_file_set(&fs, name, 0U, 0U, 0U, 0U,
                                        false, set, &count) && count == 4U);
            memcpy(media[lba] + entry * 128U, set, 128U);
        }
    }
    assert(exfat_build_file_set(&fs, "last", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[446U] + 384U, set, 96U);
    media[121U][(329U - 2U) / 8U] |=
        (u8)(1U << ((329U - 2U) % 8U));
    vfs_node_t *sub = exfat_finddir(&root, "sub");
    assert(sub && exfat_ownership_preflight_locked(sub) == VFS_OK);
    return sub;
}

static void multisector_conversion_regressions(void)
{
    for (u32 fault = 0; fault < 2U; fault++) {
        vfs_node_t *sub = prepare_multisector_conversion();
        vfs_node_t *created = prepared_created_node();
        vfs_file_facts_t facts;
        u64 pin = 0;
        usize baseline = live_requested;
        peak_requested = baseline;
        unsigned old_reads = reads, old_writes = writes,
                 old_flushes = flushes;
        clock_t start = clock();
        if (fault) fail_write_lba = 26U; /* second old-range FAT sector */
        int result = exfat_create_contents_locked(sub, created, "new", 0U,
            NULL, NULL, &facts, &pin);
        exfat_directory_growth_t *growth =
            fs.replace_transaction->directory_growth;
        assert(growth && growth->conversion &&
               growth->conversion_count == 2U &&
               growth->conversion_sectors[0].lba == 25U &&
               growth->conversion_sectors[1].lba == 26U);
        printf("exFAT 129-cluster conversion fault=%u: peak=%zu retained=%zu "
               "reads=%u writes=%u flushes=%u cpu_ms=%.3f\n", fault,
               (size_t)(peak_requested - baseline),
               (size_t)(live_requested - baseline), reads - old_reads,
               writes - old_writes, flushes - old_flushes,
               1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC);
        if (!fault)
            printf("exFAT conversion inventory: tx=%zu growth=%zu "
                   "sector=%zu max_sectors=33 max_images=%zu "
                   "max_retained_pin=%zu max_32_pin_retained=%zu "
                   "requested bytes\n",
                   sizeof(exfat_replace_transaction_t),
                   sizeof(exfat_directory_growth_t),
                   sizeof(exfat_conversion_sector_t),
                   33U * sizeof(exfat_conversion_sector_t),
                   sizeof(exfat_replace_transaction_t) +
                       sizeof(exfat_directory_growth_t) +
                       33U * sizeof(exfat_conversion_sector_t) +
                       sizeof(exfat_node_t) +
                       (MG_FILE_CONTENT_MAX_BYTES / EXFAT_SECTOR_BYTES) *
                           sizeof(u32),
                   32U * (sizeof(exfat_replace_transaction_t) +
                       sizeof(exfat_directory_growth_t) +
                       33U * sizeof(exfat_conversion_sector_t) +
                       sizeof(exfat_node_t) +
                       (MG_FILE_CONTENT_MAX_BYTES / EXFAT_SECTOR_BYTES) *
                           sizeof(u32)));
        if (fault) {
            assert(result == VFS_ERR_UNCERTAIN && !pin && pin_unresolved &&
                   growth->conversion_written == 1U &&
                   growth->conversion_sectors[1].attempted &&
                   !growth->conversion_sectors[1].written &&
                   growth->tail_attempted && !growth->tail_written &&
                   ((exfat_node_t *)sub->fs_data)->no_fat_chain &&
                   !created->fs_data);
            kfree(created);
            discard_unresolved_fixture_pin();
        } else {
            assert(result == VFS_OK && growth->conversion_written == 2U &&
                   !((exfat_node_t *)sub->fs_data)->no_fat_chain &&
                   exfat_le32(media[25U] + (200U % 128U) * 4U) == 201U &&
                   exfat_le32(media[26U] + (328U % 128U) * 4U) ==
                       growth->candidate);
            bind_created_node(created, &facts);
            finish_pin();
            assert(exfat_finddir(sub, "old-entry-0-0-long") &&
                   exfat_finddir(sub, "old-entry-128-2-long") &&
                   exfat_finddir(sub, "new") == created &&
                   exfat_ownership_preflight_locked(sub) == VFS_OK);
        }
        cleanup();
    }
}

static void nonroot_growth_regressions(void)
{
    vfs_file_facts_t facts;
    u64 pin;
    for (u32 split = 0; split < 2U; split++) {
        vfs_node_t *sub = prepare_nonroot_growth(split != 0U);
        vfs_node_t *created = prepared_created_node();
        usize baseline = live_requested;
        peak_requested = baseline;
        unsigned old_reads = reads, old_writes = writes, old_flushes = flushes;
        clock_t start = clock();
        assert(exfat_create_contents_locked(sub, created, "grown", 1U,
            replace_source, "X", &facts, &pin) == VFS_OK);
        double cpu_ms = 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
        exfat_directory_growth_t *growth = fs.replace_transaction->directory_growth;
        assert(growth && growth->appended && growth->parent_update &&
               growth->parent_primary_written && growth->marker_written &&
               growth->parent_stream_attempted == (split != 0U) &&
               sub->size == 1024U &&
               ((exfat_node_t *)sub->fs_data)->data_length == 1024U &&
               exfat_le32(media[24] + 5U * 4U) == growth->candidate &&
               exfat_le64(media[split ? 44U : 40U] +
                          (split ? 0U : 192U) + 24U) == 1024U);
        printf("exFAT non-root growth split=%u: peak=%zu retained=%zu "
               "reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
               split, (size_t)(peak_requested - baseline),
               (size_t)(live_requested - baseline), reads - old_reads,
               writes - old_writes, flushes - old_flushes, cpu_ms);
        bind_created_node(created, &facts);
        finish_pin();
        assert(exfat_finddir(sub, "grown") == created &&
               exfat_ownership_preflight_locked(sub) == VFS_OK);
        u8 byte = 0;
        assert(exfat_node_read(created, 0U, 1U, &byte) == 1U && byte == 'X');
        vfs_node_t *next = prepared_created_node();
        assert(exfat_create_contents_locked(sub, next, "again", 0U,
            NULL, NULL, &facts, &pin) == VFS_OK &&
            !fs.replace_transaction->directory_growth);
        bind_created_node(next, &facts);
        finish_pin();
        assert(exfat_finddir(sub, "again") == next &&
               exfat_ownership_preflight_locked(sub) == VFS_OK);
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_nonroot_growth(true);
        memset(media[41], 0xff, 8U);
        const u32 free_clusters[] = {7U, 9U, 11U};
        for (u32 index = 0; index < 3U; index++) {
            u32 bit = free_clusters[index] - 2U;
            media[41][bit / 8U] &= (u8)~(1U << (bit & 7U));
        }
        u8 contents[1024];
        memset(contents, 'F', sizeof(contents));
        vfs_node_t *created = prepared_created_node();
        assert(exfat_create_contents_locked(sub, created, "fragmented",
            sizeof(contents), replace_source, contents,
            &facts, &pin) == VFS_OK);
        assert(fs.replace_transaction->directory_growth->candidate == 7U &&
               !fs.replace_transaction->new_no_fat &&
               exfat_le32(media[24] + 5U * 4U) == 7U &&
               exfat_le32(media[24] + 9U * 4U) == 11U &&
               exfat_le32(media[24] + 11U * 4U) == EXFAT_CLUSTER_EOC);
        bind_created_node(created, &facts);
        finish_pin();
        u8 byte;
        assert(exfat_finddir(sub, "fragmented") == created &&
               exfat_node_read(created, 1023U, 1U, &byte) == 1U &&
               byte == 'F');
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_nonroot_growth(false);
        vfs_node_t *created = prepared_created_node();
        assert(exfat_create_contents_locked(sub, created, "short", 1U,
            short_source, NULL, &facts, &pin) == VFS_ERR_IO);
        assert(pin && pin_active && !pin_unresolved &&
               fs.replace_transaction->directory_growth->candidate_cancelled &&
               ((exfat_node_t *)sub->fs_data)->data_length == 512U &&
               exfat_le32(media[24] + 5U * 4U) == EXFAT_CLUSTER_EOC &&
               media[43][480U] == EXFAT_ENTRY_END);
        finish_pin();
        assert(!exfat_finddir(sub, "short") && fs.version_confident);
        kfree(created);
        cleanup();
    }
    {
        vfs_node_t *sub = prepare_nonroot_growth(false);
        vfs_node_t *created = prepared_created_node();
        fail_write_lba = 41U;
        assert(exfat_create_contents_locked(sub, created, "bitmap", 0U,
            NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
        assert(!pin && pin_active && pin_unresolved &&
               fs.replace_transaction->directory_growth &&
               !fs.replace_transaction->directory_growth->tail_attempted &&
               media[43][480U] == EXFAT_ENTRY_END);
        kfree(created);
        discard_unresolved_fixture_pin();
        cleanup();
    }
    for (u32 fault = 0; fault < 7U; fault++) {
        vfs_node_t *sub = prepare_nonroot_growth(true);
        vfs_node_t *created = prepared_created_node();
        if (fault == 0U) fail_write_phase = EXFAT_CREATE_PARENT_UPDATING;
        if (fault == 1U) fail_write_lba = 40U;
        if (fault == 2U) fail_write_phase = EXFAT_CREATE_DIRECTORY_REVEALING;
        if (fault >= 3U && fault <= 5U) fail_flush_number = fault;
        if (fault == 6U) remove_after_write_number = 9U;
        assert(exfat_create_contents_locked(sub, created, "failed", 0U,
            NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
        assert(fs.replace_transaction);
        exfat_directory_growth_t *growth =
            fs.replace_transaction->directory_growth;
        assert(!pin && !created->fs_data && pin_active && pin_unresolved &&
               fs.replace_transaction && !fs.version_confident &&
               growth && growth->tail_attempted &&
               growth->parent_update &&
               exfat_le64(growth->parent_set_before +
                   EXFAT_ENTRY_BYTES + 24U) == 512U &&
               exfat_le64(growth->parent_set_proposed +
                   EXFAT_ENTRY_BYTES + 24U) == 1024U);
        kfree(created);
        discard_unresolved_fixture_pin();
        cleanup();
    }
}

static void nonroot_growth_maximum(void)
{
    setup_large();
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "sub", EXFAT_ATTR_DIRECTORY, 7U,
                                512U, 512U, false, set, &count) &&
           count == 3U);
    memcpy(media[120] + 160U, set, count * EXFAT_ENTRY_BYTES);
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[121][0] |= (u8)(1U << (7U - 2U));
    for (u32 index = 0; index < 3U; index++) {
        char name[32];
        snprintf(name, sizeof(name), "abcdefghijklmnop%u", index);
        assert(exfat_build_file_set(&fs, name, 0U, 0U, 0U, 0U,
                                    false, set, &count) && count == 4U);
        memcpy(media[125] + index * 128U, set, 128U);
    }
    assert(exfat_build_file_set(&fs, "last", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[125] + 384U, set, 96U);
    vfs_node_t *sub = exfat_finddir(&root, "sub");
    assert(sub && exfat_ownership_preflight_locked(sub) == VFS_OK);
    u8 *input = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(input);
    memset(input, 'M', MG_FILE_CONTENT_MAX_BYTES);
    vfs_node_t *created = prepared_created_node();
    vfs_file_facts_t facts;
    u64 pin;
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned old_reads = reads, old_writes = writes, old_flushes = flushes;
    clock_t start = clock();
    assert(exfat_create_contents_locked(sub, created, "maximum",
        MG_FILE_CONTENT_MAX_BYTES, replace_source, input,
        &facts, &pin) == VFS_OK);
    double cpu_ms = 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
    assert(fs.replace_transaction->fresh_count == 4096U &&
           fs.replace_transaction->directory_growth->appended &&
           sub->size == 1024U);
    printf("exFAT 2 MiB non-root growth: peak=%zu retained=%zu "
           "reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline), reads - old_reads,
           writes - old_writes, flushes - old_flushes, cpu_ms);
    bind_created_node(created, &facts);
    finish_pin();
    u8 last = 0;
    assert(exfat_finddir(sub, "maximum") == created &&
           exfat_node_read(created, MG_FILE_CONTENT_MAX_BYTES - 1U,
                           1U, &last) == 1U && last == 'M');
    free(input);
    cleanup();
}

static void root_growth_regressions(void)
{
    vfs_file_facts_t facts;
    u64 pin;
    usize baseline;
    unsigned initial_reads, initial_writes, initial_flushes;

    prepare_root_growth();
    vfs_node_t *created = prepared_created_node();
    baseline = live_requested;
    peak_requested = baseline;
    initial_reads = reads; initial_writes = writes; initial_flushes = flushes;
    assert(exfat_create_contents_locked(&root, created, "grown", 1U,
        replace_source, "g", &facts, &pin) == VFS_OK);
    assert(pin_active && !pin_unresolved && pin &&
           fs.replace_transaction->directory_growth &&
           fs.replace_transaction->directory_growth->appended &&
           fs.replace_transaction->directory_growth->candidate == 5U &&
           fs.replace_transaction->directory_growth->target_attempted &&
           fs.replace_transaction->directory_growth->target_written &&
           fs.replace_transaction->directory_growth->tail_attempted &&
           fs.replace_transaction->directory_growth->tail_written &&
           fs.replace_transaction->directory_growth->marker_attempted &&
           fs.replace_transaction->directory_growth->marker_written &&
           fs.replace_transaction->directory_growth->flush_progress == 3U &&
           exfat_le32(media[24] + 8U) == 5U &&
           root_state.data_length == 1024U && root.size == 1024U &&
           media[40][448] == EXFAT_ENTRY_DELETED &&
           media[43][0] == EXFAT_ENTRY_FILE);
    printf("exFAT root growth: peak=%zu retained=%zu reads=%u writes=%u flushes=%u\n",
        (size_t)(peak_requested - baseline),
        (size_t)(live_requested - baseline), reads - initial_reads,
        writes - initial_writes, flushes - initial_flushes);
    bind_created_node(created, &facts);
    finish_pin();
    assert(exfat_finddir(&root, "grown") == created);
    u8 byte = 0;
    assert(exfat_node_read(created, 0, 1, &byte) == 1 && byte == 'g');
    cleanup();

    /* A previously linked root cluster can receive the relocated marker
     * without allocating another directory cluster. */
    prepare_root_growth();
    exfat_put32(media[24] + 8U, 5U);
    exfat_put32(media[24] + 20U, EXFAT_CLUSTER_EOC);
    media[41][0] |= (u8)(1U << (5U - 2U));
    root_state.data_length = root_state.valid_data_length = root.size = 1024U;
    created = prepared_created_node();
    assert(exfat_create_contents_locked(&root, created, "existing-tail", 0,
        NULL, NULL, &facts, &pin) == VFS_OK);
    assert(fs.replace_transaction->directory_growth &&
           !fs.replace_transaction->directory_growth->appended &&
           exfat_le32(media[24] + 8U) == 5U &&
           media[43][0] == EXFAT_ENTRY_FILE);
    bind_created_node(created, &facts);
    finish_pin();
    assert(exfat_finddir(&root, "existing-tail") == created);
    cleanup();

    for (u32 contiguous = 0U; contiguous < 2U; contiguous++) {
        setup();
        add_system_entries();
        add_entry(160U, "sub", EXFAT_ATTR_DIRECTORY, 5U, 512U,
                  contiguous != 0U);
        media[41][0] |= (u8)(1U << (5U - 2U));
        exfat_put32(media[24] + 20U, EXFAT_CLUSTER_EOC);
        u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
        u8 count;
        for (u32 entry = 0; entry < 3U; entry++) {
            char child_name[32];
            snprintf(child_name, sizeof(child_name),
                     "abcdefghijklmnop%u", entry);
            assert(exfat_build_file_set(&fs, child_name, 0U, 0U, 0U, 0U,
                                        false, set, &count) && count == 4U);
            memcpy(media[43] + entry * 128U, set, 128U);
        }
        assert(exfat_build_file_set(&fs, "last", 0U, 0U, 0U, 0U,
                                    false, set, &count) && count == 3U);
        memcpy(media[43] + 384U, set, 96U);
        vfs_node_t *sub = exfat_finddir(&root, "sub");
        assert(sub && sub->type == VFS_TYPE_DIRECTORY);
        vfs_node_t *prepared = prepared_created_node();
        unsigned unchanged = writes;
        int result = exfat_create_contents_locked(sub, prepared, "child",
                                                   0U, NULL, NULL,
                                                   &facts, &pin);
        assert(result == VFS_OK && pin_active && pin &&
               fs.replace_transaction->directory_growth &&
               fs.replace_transaction->directory_growth->parent_update &&
               fs.replace_transaction->directory_growth->parent_primary_written &&
               fs.replace_transaction->directory_growth->marker_written &&
               fs.replace_transaction->directory_growth->contiguous_extension ==
                   (contiguous != 0U) &&
               sub->size == 1024U &&
               ((exfat_node_t *)sub->fs_data)->data_length == 1024U &&
               writes > unchanged);
        bind_created_node(prepared, &facts);
        finish_pin();
        assert(exfat_finddir(sub, "child") == prepared);
        cleanup();
    }

    /* Candidate, staged file links, and root tail share one FAT sector.
     * Linking the root must preserve every file-chain update. */
    prepare_root_growth();
    memset(media[41], 0xff, 8U);
    media[41][0] &= (u8)~((1U << 3U) | (1U << 5U) | (1U << 7U));
    u8 fragmented_data[1024];
    memset(fragmented_data, 'F', sizeof(fragmented_data));
    created = prepared_created_node();
    assert(exfat_create_contents_locked(&root, created, "fragmented", 1024U,
        replace_source, fragmented_data,
        &facts, &pin) == VFS_OK);
    assert(fs.replace_transaction->directory_growth->candidate == 5U &&
           !fs.replace_transaction->new_no_fat &&
           exfat_le32(media[24] + 8U) == 5U &&
           exfat_le32(media[24] + 20U) == EXFAT_CLUSTER_EOC &&
           exfat_le32(media[24] + 28U) == 9U &&
           exfat_le32(media[24] + 36U) == EXFAT_CLUSTER_EOC);
    bind_created_node(created, &facts);
    finish_pin();
    u8 last = 0;
    assert(exfat_node_read(created, 1023U, 1U, &last) == 1U && last == 'F');
    cleanup();

    /* A source failure after reserving the directory candidate returns
     * definite failure only after both candidate and file ledgers cancel. */
    prepare_root_growth();
    vfs_node_t prepared = {0};
    assert(exfat_create_contents_locked(&root, &prepared, "failed", 1U,
        short_source, NULL, &facts, &pin) == VFS_ERR_IO);
    assert(pin_active && !pin_unresolved && pin &&
           fs.replace_transaction->directory_growth->candidate_cancelled &&
           !(media[41][0] & (1U << (5U - 2U))) &&
           exfat_le32(media[24] + 8U) == EXFAT_CLUSTER_EOC &&
           media[40][448] == EXFAT_ENTRY_END && !prepared.fs_data);
    finish_pin();
    assert(!exfat_finddir(&root, "failed"));
    cleanup();

    prepare_root_growth();
    unsigned attempted = writes;
    fail_allocation = 5U; /* growth inventory after scan and base state */
    assert(exfat_create_contents_locked(&root, &prepared, "memory", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_NO_MEM);
    assert(writes == attempted && !pin_active && !fs.replace_transaction &&
           !prepared.fs_data && fs.version_confident);
    cleanup();

    prepare_root_growth();
    memset(media[41], 0xff, 8U);
    attempted = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "full", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_NO_SPACE);
    assert(writes == attempted && !pin_active && !fs.replace_transaction);
    cleanup();

    prepare_root_growth();
    media[40][480] = EXFAT_ENTRY_DELETED; /* stale tail after end marker */
    attempted = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "hidden", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_UNSUPPORTED);
    assert(writes == attempted && !pin_active && !fs.replace_transaction);
    cleanup();

    prepare_root_growth();
    fail_bitmap_write_number = 1U;
    assert(exfat_create_contents_locked(&root, &prepared, "bitmap", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_active && pin_unresolved &&
           fs.replace_transaction->directory_growth->candidate == 5U &&
           !fs.replace_transaction->directory_growth->bitmap_written &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_BITMAP);
    discard_unresolved_fixture_pin();
    cleanup();

    prepare_root_growth();
    fail_write_lba = 24U;
    assert(exfat_create_contents_locked(&root, &prepared, "fat", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_active && pin_unresolved &&
           fs.replace_transaction->directory_growth->bitmap_written &&
           !fs.replace_transaction->directory_growth->fat_written &&
           !fs.replace_transaction->directory_growth->tail_attempted);
    discard_unresolved_fixture_pin();
    cleanup();

    prepare_root_growth();
    fail_write_lba = 43U;
    assert(exfat_create_contents_locked(&root, &prepared, "zero", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_active && pin_unresolved &&
           fs.replace_transaction->directory_growth->initialized_sectors == 0U &&
           !fs.replace_transaction->directory_growth->tail_attempted);
    discard_unresolved_fixture_pin();
    cleanup();

    prepare_root_growth();
    fail_bitmap_write_number = 4U;
    assert(exfat_create_contents_locked(&root, &prepared, "cancel", 1U,
        short_source, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_active && pin_unresolved &&
           fs.replace_transaction->phase == EXFAT_REPLACE_CANCELLING &&
           fs.replace_transaction->cancellation_cursor == 1U &&
           !fs.replace_transaction->directory_growth->candidate_cancelled &&
           !fs.replace_transaction->directory_growth->tail_attempted);
    discard_unresolved_fixture_pin();
    cleanup();

    const exfat_replace_phase_t phases[] = {
        EXFAT_CREATE_DIRECTORY_PREPARING,
        EXFAT_CREATE_DIRECTORY_LINKING,
        EXFAT_CREATE_DIRECTORY_REVEALING,
    };
    for (u32 index = 0; index < 3U; index++) {
        prepare_root_growth();
        fail_write_phase = phases[index];
        fail_write_copy_bytes = index == 2U ? 480U : 128U;
        assert(exfat_create_contents_locked(&root, &prepared, "torn", 0,
            NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
        assert(!pin && !prepared.fs_data && pin_active && pin_unresolved &&
               !fs.version_confident && fs.replace_transaction &&
               fs.replace_transaction->directory_growth &&
               fs.replace_transaction->phase == phases[index]);
        if (index == 2U)
            assert(fs.replace_transaction->directory_growth->tail_attempted &&
                   fs.replace_transaction->directory_growth->marker_attempted);
        discard_unresolved_fixture_pin();
        cleanup();
    }

    for (u32 failed_flush = 1U; failed_flush <= 3U; failed_flush++) {
        prepare_root_growth();
        fail_flush_number = failed_flush;
        assert(exfat_create_contents_locked(&root, &prepared, "flush", 0,
            NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
        assert(pin_active && pin_unresolved && !pin &&
               fs.replace_transaction->directory_growth &&
               fs.replace_transaction->directory_growth->target_attempted &&
               fs.replace_transaction->directory_growth->target_written &&
               fs.replace_transaction->directory_growth->tail_attempted ==
                   (failed_flush >= 2U) &&
               fs.replace_transaction->directory_growth->marker_attempted ==
                   (failed_flush >= 3U) &&
               fs.replace_transaction->directory_growth->flush_progress ==
                   failed_flush - 1U);
        discard_unresolved_fixture_pin();
        cleanup();
    }

    prepare_root_growth();
    remove_after_write_number = 4U;
    assert(exfat_create_contents_locked(&root, &prepared, "removed", 0,
        NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_active && pin_unresolved && !prepared.fs_data &&
           fs.replace_transaction->directory_growth);
    discard_unresolved_fixture_pin();
    cleanup();
}

static void root_growth_maximum(void)
{
    setup_large();
    for (u32 offset = 160U; offset < 448U; offset += EXFAT_ENTRY_BYTES)
        media[120][offset] = EXFAT_ENTRY_LABEL;
    u8 *input = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(input);
    memset(input, 'M', MG_FILE_CONTENT_MAX_BYTES);
    vfs_node_t *created = prepared_created_node();
    vfs_file_facts_t facts;
    u64 pin;
    usize baseline = live_requested;
    unsigned old_reads = reads, old_writes = writes, old_flushes = flushes;
    peak_requested = baseline;
    clock_t started = clock();
    assert(exfat_create_contents_locked(&root, created, "maximum",
        MG_FILE_CONTENT_MAX_BYTES, replace_source, input,
        &facts, &pin) == VFS_OK);
    double cpu_ms = 1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC;
    assert(fs.replace_transaction->directory_growth->appended &&
           fs.replace_transaction->fresh_count == 4096U &&
           root_state.data_length == 1024U &&
           fs.replace_transaction->directory_growth->candidate == 7U);
    printf("exFAT 2 MiB root growth: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
        (size_t)(peak_requested - baseline),
        (size_t)(live_requested - baseline), reads - old_reads,
        writes - old_writes, flushes - old_flushes, cpu_ms);
    bind_created_node(created, &facts);
    finish_pin();
    u8 last;
    assert(exfat_finddir(&root, "maximum") == created &&
           exfat_node_read(created, MG_FILE_CONTENT_MAX_BYTES - 1U,
                           1U, &last) == 1U && last == 'M');
    free(input);
    cleanup();
}

static void create_contents_fragmented_maximum(void)
{
    setup_large();
    for (u32 cluster = 8U; cluster <= 10001U; cluster += 2U) {
        u32 bit = cluster - 2U;
        media[121U + bit / (EXFAT_SECTOR_BYTES * 8U)]
             [(bit / 8U) % EXFAT_SECTOR_BYTES] |=
             (u8)(1U << (bit % 8U));
    }
    u8 *input = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(input);
    memset(input, 'F', MG_FILE_CONTENT_MAX_BYTES);
    vfs_node_t *node = prepared_created_node();
    vfs_file_facts_t facts;
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned initial_reads = reads, initial_writes = writes;
    u64 pin = 0;
    assert(exfat_create_contents_locked(&root, node, "fragmented",
               MG_FILE_CONTENT_MAX_BYTES, replace_source, input,
               &facts, &pin) == VFS_OK);
    assert(pin && !fs.replace_transaction->new_no_fat &&
           fs.replace_transaction->fresh_count == 4096U);
    printf("exFAT fragmented 2 MiB create: peak=%zu retained=%zu bytes, %u reads, %u writes\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline),
           reads - initial_reads, writes - initial_writes);
    assert(writes - initial_writes < 12500);
    bind_created_node(node, &facts);
    finish_pin();
    u8 last;
    assert(exfat_node_read(node, MG_FILE_CONTENT_MAX_BYTES - 1U,
                           1U, &last) == 1U && last == 'F');
    free(input);
    cleanup();
}

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
           VFS_OK);
    assert(pin && pin_active && writes > before &&
           fs.replace_transaction->stream_written &&
           fs.replace_transaction->primary_written &&
           fs.replace_transaction->publication_flushed);
    finish_pin();
    assert(exfat_node_read(file, 0U, 1U, data) == 1U && data[0] == 'x');
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
    unsigned before_unsupported = writes;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1024U,
                                replace_source, "", &pin) == VFS_ERR_UNSUPPORTED);
    assert(!pin && !pin_active && !fs.replace_transaction &&
           fs.version_confident && writes == before_unsupported);
    cleanup();

    setup();
    add_system_entries();
    memset(media[41], 0xff, EXFAT_SECTOR_BYTES);
    media[41][0] &= (u8)~(1U << (6U - 2U));
    media[41][0] &= (u8)~(1U << (8U - 2U));
    file = exfat_finddir(&root, "old.txt");
    fail_write_lba = 24U;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1024U,
                                replace_source, linked_content, &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && pin_active && pin_unresolved && !fs.version_confident &&
           fs.replace_transaction && file->size == 0U &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_FAT &&
           fs.replace_transaction->fat_copy_progress == 0U);
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
           file->size == 0U && !fs.version_confident &&
           !fs.replace_transaction->stream_attempted &&
           !fs.replace_transaction->primary_attempted);
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
    ((u32 *)media[24])[4] = EXFAT_CLUSTER_EOC;
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
    ((u32 *)media[24])[3] = 4U;
    ((u32 *)media[24])[4] = 5U;
    ((u32 *)media[24])[5] = EXFAT_CLUSTER_EOC;
    ((u32 *)media[24])[6] = EXFAT_CLUSTER_EOC;
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

/* Independently placed system entries/chains. No ordinary file-stream flags
 * describe these allocations (Microsoft sections 7.1.3 and 7.2.3). */
static void system_stream_regressions(void)
{
    setup_large();
    /* Bitmap 3 -> 9 -> 5, deliberately leaving poisoned adjacent cluster 4. */
    ((u32 *)media[24])[3] = 9U;
    ((u32 *)media[24])[9] = 5U;
    memset(media[122], 0xff, 512U);
    memset(media[127], 0, 512U);
    media[121][0] = 0x1bU; /* 2,3,5,6 allocated; 4 is NOT bitmap storage */
    media[121][0] |= 0x80U; /* cluster 9 allocated */
    assert(exfat_read_system_entries(&fs));
    assert(exfat_system_streams_check(&fs) == VFS_OK);
    bool used;
    assert(exfat_bitmap_bit(&fs, 4100U, &used) && !used);
    assert(exfat_bitmap_set(&fs, 4100U, true));
    assert(media[127][0] == 4U && media[122][0] == 0xffU);
    assert(exfat_bitmap_bit(&fs, 4100U, &used) && used);
    u64 bitmap_lba;
    assert(exfat_bitmap_byte_lba(&fs, 512U, &bitmap_lba) && bitmap_lba == 127U);
    assert(exfat_bitmap_set_range(&fs, 4098U, 8U, false));
    assert(exfat_bitmap_range_unused(&fs, 4098U, 8U));
    assert(exfat_bitmap_set_range(&fs, 4098U, 8U, true));
    assert(!exfat_bitmap_range_unused(&fs, 4098U, 8U) &&
           media[127][0] == 0xffU && media[122][0] == 0xffU);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    /* Creation and replacement must agree with the bitmap's fragmented map. */
    vfs_node_t *created = prepared_created_node();
    vfs_file_facts_t facts;
    u64 pin = 0;
    assert(exfat_create_contents_locked(&root, created, "mapped", 1U,
               replace_source, "m", &facts, &pin) == VFS_OK);
    bind_created_node(created, &facts);
    finish_pin();
    assert(exfat_replace_locked(&super, created->inode, 1U, 1U,
                                replace_source, "n", &pin) == VFS_OK);
    finish_pin();
    u8 byte;
    assert(exfat_node_read(created, 0U, 1U, &byte) == 1U && byte == 'n');
    u8 adjacent_before_failure = media[122][0];
    fail_write_lba = 127U;
    assert(!exfat_bitmap_set_range(&fs, 4098U, 8U, false));
    assert(!fs.version_confident && media[122][0] == adjacent_before_failure);

    cleanup();

    setup_large();
    /* Full dense table across 256 nonadjacent clusters: Unicode mappings
     * and sentinel neighbours are independently encoded, not a table builder. */
    fs.upcase_cluster = 100U;
    fs.upcase_length = 0x20000U;
    exfat_put32(media[120] + 128U + 20U, 100U);
    exfat_put64(media[120] + 128U + 24U, fs.upcase_length);
    for (u32 part = 0; part < 256U; part++) {
        u32 cluster = 100U + part * 2U;
        ((u32 *)media[24U + cluster / 128U])[cluster % 128U] =
            part == 255U ? 0xffffffffU : cluster + 2U;
        u32 bit = cluster - 2U;
        media[121U + bit / 4096U][(bit / 8U) % 512U] |= 1U << (bit % 8U);
        for (u32 j = 0; j < 256U; j++) {
            u32 cp = part * 256U + j;
            u16 upper = cp >= 0x61U && cp <= 0x7aU ? cp - 32U : cp;
            if (cp == 0xe9U) upper = 0xc9U;
            exfat_put16(media[120U + cluster - 2U] + j * 2U, upper);
        }
        memset(media[120U + cluster - 1U], 0xa5, 512U);
    }
    /* Flatten only in this independent checksum oracle, not production I/O. */
    u32 checksum = 0;
    for (u32 cp = 0; cp < 65536U; cp++) {
        u16 upper = cp >= 0x61U && cp <= 0x7aU ? cp - 32U : cp;
        if (cp == 0xe9U) upper = 0xc9U;
        checksum = (checksum >> 1U) | (checksum << 31U);
        checksum += upper & 255U;
        checksum = (checksum >> 1U) | (checksum << 31U);
        checksum += upper >> 8U;
    }
    exfat_put32(media[120] + 128U + 4U, checksum);
    assert(exfat_read_system_entries(&fs));
    assert(exfat_system_streams_check(&fs) == VFS_OK);
    fs.upcase = NULL;
    assert(exfat_load_upcase(&fs));
    assert(exfat_upcase_unit(&fs, 0xe9U) == 0xc9U &&
           exfat_upcase_unit(&fs, 0xffffU) == 0xffffU);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    kfree(fs.upcase); fs.upcase = NULL;
    fail_read_lba = 220U; /* second upcase cluster */
    assert(!exfat_load_upcase(&fs) && !fs.upcase && live_requested == 0U);
    assert(fs.version_confident);
    cleanup();

    setup_large();
    assert(exfat_system_streams_check(&fs) == VFS_OK);
    ((u32 *)media[24])[2] = 2U;
    unsigned before_cycle_reads = reads;
    assert(!exfat_read_system_entries(&fs));
    assert(reads - before_cycle_reads < 8U && fs.version_confident);
    ((u32 *)media[24])[2] = 0xffffffffU;
    const u32 bad_next[] = {0U, 1U, 3U, 10002U, 0xfffffff7U, 0xffffffffU};
    for (usize i = 0; i < sizeof(bad_next) / sizeof(bad_next[0]); i++) {
        ((u32 *)media[24])[3] = bad_next[i];
        assert(exfat_system_streams_check(&fs) == VFS_ERR_BAD_FORMAT);
        assert(fs.version_confident && writes == 0U);
    }
    ((u32 *)media[24])[3] = 4U;
    ((u32 *)media[24])[5] = 7U; /* chain too long */
    assert(exfat_system_streams_check(&fs) == VFS_ERR_BAD_FORMAT);
    ((u32 *)media[24])[5] = 0xfffffff8U; /* EOC must be FFFFFFFF */
    assert(exfat_system_streams_check(&fs) == VFS_ERR_BAD_FORMAT);
    ((u32 *)media[24])[5] = 0xffffffffU;
    media[121][0] &= ~4U; /* bitmap's own cluster marked free */
    assert(exfat_system_streams_check(&fs) == VFS_ERR_BAD_FORMAT);
    media[121][0] |= 4U;
    fs.upcase_cluster = 3U; fs.upcase_length = 1536U; /* shared system chain */
    assert(exfat_system_streams_check(&fs) == VFS_ERR_BAD_FORMAT);
    fs.upcase_cluster = 6U; fs.upcase_length = 512U;
    fail_read_lba = 24U;
    assert(exfat_system_streams_check(&fs) == VFS_ERR_IO);
    fail_read_lba = 121U;
    assert(exfat_system_streams_check(&fs) == VFS_ERR_IO);
    fail_read_lba = ~(u64)0;
    online = false;
    assert(exfat_system_streams_check(&fs) == VFS_ERR_IO);
    online = true;

    fail_read_lba = ~(u64)0;
    fail_allocation = allocations + 1U;
    assert(exfat_system_streams_check(&fs) == VFS_ERR_NO_MEM);
    assert(fs.version_confident && writes == 0U && live_requested == 0U);
    fail_allocation = 0U;
    exfat_put32(media[120] + 96U + 20U, 10002U);
    assert(exfat_read_system_entries(&fs));
    assert(exfat_system_streams_check(&fs) == VFS_ERR_BAD_FORMAT);
    exfat_put32(media[120] + 96U + 20U, 3U);
    exfat_put64(media[120] + 96U + 24U, 1249U); /* ceil(10000/8) is 1250 */
    assert(!exfat_read_system_entries(&fs));
    exfat_put64(media[120] + 96U + 24U, 1536U);
    exfat_put64(media[120] + 128U + 24U, 0U);
    assert(!exfat_read_system_entries(&fs));
    exfat_put64(media[120] + 128U + 24U, EXFAT_MAX_UPCASE_BYTES + 1ULL);
    assert(!exfat_read_system_entries(&fs));
    exfat_put64(media[120] + 128U + 24U, 512U);
    memcpy(media[120] + 160U, media[120] + 128U, 32U);
    assert(!exfat_read_system_entries(&fs)); /* duplicate upcase */
    memset(media[120] + 160U, 0, 32U);
    assert(exfat_read_system_entries(&fs));
    for (u32 offset = 160U; offset < 512U; offset += 32U)
        media[120][offset] = 5U; /* fully occupied root, no end marker */
    assert(!exfat_read_system_entries(&fs));
    for (u32 offset = 160U; offset < 512U; offset += 32U)
        media[120][offset] = 0U;
    media[120][97] = 1U; /* second bitmap invalid for a one-FAT volume */
    assert(!exfat_read_system_entries(&fs));
    media[120][97] = 0x80U; /* reserved bits are not stream flags */
    assert(exfat_read_system_entries(&fs));
    media[120][160] = 0x81U; /* duplicate bitmap */
    assert(!exfat_read_system_entries(&fs));
    media[120][160] = 0U;
    fs.boot.fat_count = 2U;
    for (u32 active = 0; active < 2U; active++) {
        fs.active_fat = active;
        assert(!exfat_read_system_entries(&fs));
        assert(exfat_system_streams_check(&fs) == VFS_ERR_UNSUPPORTED);
        assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_UNSUPPORTED);
    }
    assert(fs.version_confident && writes == 0U);
    cleanup();

    setup_large();
    vfs_node_t retry = {0};
    fail_read_lba = 24U;
    assert(exfat_create_contents_locked(&root, &retry, "retry", 0U,
                                        NULL, NULL, &facts, &pin) == VFS_ERR_IO);
    assert(fs.version_confident && !pin_active && !retry.fs_data && writes == 0U);
    fail_read_lba = ~(u64)0;
    media[121][0] &= ~2U; /* bitmap storage marked free */
    assert(exfat_create_contents_locked(&root, &retry, "retry", 0U,
                                        NULL, NULL, &facts, &pin) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && !pin_active && !retry.fs_data && writes == 0U);
    cleanup();

    setup_large();
    assert(exfat_format_device(&device));
    unsigned formatted_writes = writes;
    exfat_fs_t *mounted = exfat_new_state(&device);
    assert(mounted && writes == formatted_writes);
    assert(exfat_system_streams_check(mounted) == VFS_OK);
    u32 formatted_root = mounted->root_cluster;
    u32 formatted_heap = mounted->boot.heap_offset;
    u32 formatted_spc = mounted->sectors_per_cluster;
    u32 formatted_fat = mounted->boot.fat_offset;
    /* Independent check: bitmap cluster 2 ends; upcase 3 -> 4 -> EOC. */
    assert(exfat_le32(media[formatted_fat] + 2U * 4U) == 0xffffffffU);
    assert(exfat_le32(media[formatted_fat] + 3U * 4U) == 4U);
    assert(exfat_le32(media[formatted_fat] + 4U * 4U) == 0xffffffffU);
    exfat_free_state(mounted);
    assert(live_requested == 0U);
    exfat_put32(media[formatted_fat] + 3U * 4U, 0xffffffffU);
    assert(!exfat_new_state(&device)); /* legacy missing system FAT link */
    assert(live_requested == 0U && writes == formatted_writes);
    exfat_put32(media[formatted_fat] + 3U * 4U, 4U);
    /* Main flags select policy; backup flags are stale by specification. */
    exfat_put16(media[12] + 106U, 1U);
    mounted = exfat_new_state(&device);
    assert(mounted); exfat_free_state(mounted);
    exfat_put16(media[12] + 106U, 0U);
    for (u16 flag = 2U; flag <= 8U; flag *= 2U) {
        exfat_put16(media[0] + 106U, flag);
        assert(!exfat_new_state(&device));
        assert(live_requested == 0U && writes == formatted_writes);
    }
    exfat_put16(media[0] + 106U, 0U);
    fail_read_lba = exfat_le32(media[0] + 80U);
    assert(!exfat_new_state(&device));
    assert(live_requested == 0U && writes == formatted_writes);
    fail_read_lba = ~(u64)0;
    u64 root_lba = formatted_heap + (formatted_root - 2U) * formatted_spc;
    memcpy(media[root_lba] + 64U, media[root_lba], 32U);
    media[root_lba][65] = 1U; /* independently encoded second bitmap */
    exfat_put32(media[root_lba] + 64U + 20U, 6U);
    exfat_put32(media[formatted_fat] + 6U * 4U, 0xffffffffU);
    media[formatted_heap][0] |= 0x10U; /* distinct inactive bitmap cluster */
    for (u32 i = 0; i < formatted_spc; i++)
        memcpy(media[formatted_heap + 4U * formatted_spc + i],
               media[formatted_heap + i], 512U);
    u32 fat_sectors = exfat_le32(media[0] + 84U);
    for (u32 i = 0; i < fat_sectors; i++)
        memcpy(media[formatted_fat + fat_sectors + i],
               media[formatted_fat + i], 512U);

    for (u32 region = 0; region <= 12U; region += 12U) {
        media[region][110] = 2U;
        u32 sum = 0;
        for (u32 i = 0; i < 11U * 512U; i++) {
            if (i == 106U || i == 107U || i == 112U) continue;
            sum = (sum >> 1U) | (sum << 31U);
            sum += media[region + i / 512U][i % 512U];
        }
        for (u32 i = 0; i < 512U; i += 4U)
            exfat_put32(media[region + 11U] + i, sum);
    }
    for (u16 active = 0; active < 2U; active++) {
        exfat_put16(media[0] + 106U, active);
        exfat_boot_info_t geometry;
        assert(exfat_read_geometry(&device, device.id, &geometry));
        assert(geometry.fat_count == 2U && geometry.volume_flags == active);
        assert(!exfat_new_state(&device));
        assert(live_requested == 0U && writes == formatted_writes);
    }
    cleanup();
    puts("exFAT system FAT streams, formatter and safe layout rejection passed");
}

/* Microsoft exFAT sections 7.2.5, 7.6.2 and 7.7.2. These mappings,
 * counts, flags and hash constants are independent of the production builders.
 * The mock device still exercises production decoding, lookup and creation. */
static void specification_regressions(void)
{
    static const u16 compressed[] = {
        0xffff, 0x0061, /* U+0000..0060: exactly 97 identity mappings */
        0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
        0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e,
        0x004f, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055,
        0x0056, 0x0057, 0x0058, 0x0059, 0x005a,
        0xffff, 0x006e, /* U+007B..00E8: 110 identities */
        0x00c9,         /* U+00E9 -> U+00C9 */
        0xffff, 0xff15, /* U+00EA..FFFE: 65301 identities */
        0xffff          /* Literal U+FFFF mapping, not a run marker */
    };
    u8 table[sizeof(compressed) + 2U];
    for (usize i = 0; i < sizeof(compressed) / sizeof(compressed[0]); i++) {
        table[i * 2U] = compressed[i] & 255U;
        table[i * 2U + 1U] = compressed[i] >> 8U;
    }
    assert(exfat_upcase_decode_valid(table, sizeof(compressed)));
    assert(exfat_upcase_decode_valid(exfat_default_upcase,
                                     sizeof(exfat_default_upcase)));
    assert(exfat_upcase_checksum(exfat_default_upcase,
                                 sizeof(exfat_default_upcase)) == 0xe619d30dU);
    exfat_fs_t mapping = {0};
    mapping.upcase = table;
    mapping.upcase_length = sizeof(compressed);
    const u16 inputs[] = {0, 0x60, 0x61, 0x62, 0x63, 0x7a, 0x7b,
                          0xe8, 0xe9, 0xea, 0xfffe, 0xffff};
    const u16 expected[] = {0, 0x60, 0x41, 0x42, 0x43, 0x5a, 0x7b,
                            0xe8, 0xc9, 0xea, 0xfffe, 0xffff};
    for (usize i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++)
        assert(exfat_upcase_unit(&mapping, inputs[i]) == expected[i]);
    const u16 abc[] = {0x61, 0x62, 0x63}, accent[] = {0xe9};
    assert(exfat_name_hash(&mapping, abc, 3U) == 0xc82bU);
    assert(exfat_name_hash(&mapping, accent, 1U) == 0x8064U);
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed) - 1U));
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed) - 2U));
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed) - 4U));
    table[sizeof(compressed)] = table[sizeof(compressed) + 1U] = 0;
    assert(!exfat_upcase_decode_valid(table, sizeof(table)));
    table[2] = 0; /* zero-length run */
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed)));
    table[2] = 0x62; /* identity run illegally includes lowercase a */
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed)));
    table[2] = 0x61;
    table[sizeof(compressed) - 4U] = 0x16; /* count overruns final mapping */
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed)));
    /* A final identity run may include U+FFFF without a literal suffix. */
    assert(exfat_upcase_decode_valid(table, sizeof(compressed) - 2U));
    table[sizeof(compressed) - 4U] = 0x17; /* beyond U+FFFF */
    assert(!exfat_upcase_decode_valid(table, sizeof(compressed) - 2U));
    table[sizeof(compressed) - 4U] = 0x15;

    /* A one-character identity run followed immediately by a mapping. */
    u8 single[sizeof(compressed) + 6U];
    memcpy(single, table, sizeof(compressed) - 6U);
    const u16 tail[] = {0xffff, 1, 0x00cb, 0xffff, 0xff13, 0xffff};
    for (usize i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
        exfat_put16(single + sizeof(compressed) - 6U + i * 2U, tail[i]);
    assert(exfat_upcase_decode_valid(single, sizeof(single)));
    mapping.upcase = single;
    mapping.upcase_length = sizeof(single);
    assert(exfat_upcase_unit(&mapping, 0xeaU) == 0xeaU);
    assert(exfat_upcase_unit(&mapping, 0xebU) == 0xcbU);
    assert(exfat_upcase_unit(&mapping, 0xecU) == 0xecU);

    setup();
    fs.upcase = NULL;
    fs.upcase_cluster = 4U;
    ((u32 *)media[24])[4] = EXFAT_CLUSTER_EOC;
    fs.upcase_length = sizeof(compressed);
    fs.upcase_checksum = exfat_upcase_checksum(table, sizeof(compressed));
    memcpy(media[42], table, sizeof(compressed));
    assert(exfat_load_upcase(&fs));
    assert(exfat_upcase_unit(&fs, 0xe9U) == 0xc9U);
    kfree(fs.upcase);
    fs.upcase = NULL;
    fs.upcase_checksum ^= 1U;
    assert(!exfat_load_upcase(&fs) && !fs.upcase && live_requested == 0U);
    fs.upcase_checksum ^= 1U;
    fail_read_lba = 42U;
    assert(!exfat_load_upcase(&fs) && !fs.upcase && live_requested == 0U);
    fail_read_lba = ~(u64)0;
    fail_allocation = allocations + 1U;
    assert(!exfat_load_upcase(&fs) && !fs.upcase && live_requested == 0U);
    cleanup();

    u8 *dense = malloc(0x20000U);
    assert(dense);
    for (u32 i = 0; i < 0x10000U; i++)
        exfat_put16(dense + i * 2U,
                    i >= 0x61U && i <= 0x7aU ? i - 0x20U : i);
    exfat_put16(dense + 0xe9U * 2U, 0xc9U);
    exfat_put16(dense + 0x1234U * 2U, 0xffffU);
    assert(exfat_upcase_decode_valid(dense, 0x20000U));
    mapping.upcase = dense;
    mapping.upcase_length = 0x20000U;
    assert(exfat_upcase_unit(&mapping, 0xffffU) == 0xffffU);
    assert(exfat_upcase_unit(&mapping, 0x61U) == 0x41U);
    assert(exfat_upcase_unit(&mapping, 0xe9U) == 0xc9U);
    assert(exfat_upcase_unit(&mapping, 0x1234U) == 0xffffU);
    dense[0x61U * 2U] = 0x61; /* mandatory ASCII mapping missing */
    assert(!exfat_upcase_decode_valid(dense, 0x20000U));
    free(dense);

    setup();
    add_system_entries();
    memcpy(upcase_fixture, table, sizeof(compressed));
    fs.upcase_length = sizeof(compressed);
    exfat_put64(media[40] + 128U + 24U, sizeof(compressed));
    /* Hand-encoded existing empty file named U+00E9; not a Mangrove builder. */
    memset(media[40], 0, 96U);
    media[40][0] = 0x85; media[40][1] = 2;
    media[40][32] = 0xc0; media[40][33] = 1;
    media[40][35] = 1; /* NameLength */
    media[40][36] = 0x64; media[40][37] = 0x80; /* NameHash 0x8064 */
    media[40][64] = 0xc1; media[40][66] = 0xe9;
    /* Independently calculated using the specification's byte recurrence. */
    media[40][2] = 0x26; media[40][3] = 0x3b;
    assert(exfat_entry_checksum(media[40], 3U) == 0x3b26U);
    exfat_entry_info_t entry;
    assert(exfat_entry_set_read(&fs, 2U, false, 0U, &entry));
    vfs_node_t *accent_node = exfat_finddir(&root, "\xc3\x89");
    assert(accent_node && exfat_finddir(&root, "\xc3\xa9") == accent_node);
    vfs_node_t spare = {0};
    vfs_file_facts_t facts;
    u64 pin = 0;
    assert(exfat_create_contents_locked(&root, &spare, "\xc3\x89", 0U,
                                        NULL, NULL, &facts, &pin) ==
           VFS_ERR_ALREADY_EXISTS);
    assert(writes == 0U && !pin_active && !spare.fs_data);
    for (u32 flags = 0; flags < 256U; flags++) {
        media[40][33] = flags;
        exfat_put16(media[40] + 2U, exfat_entry_checksum(media[40], 3U));
        assert(exfat_entry_set_read(&fs, 2U, false, 0U, &entry) ==
               ((flags & 3U) == 1U)); /* reserved bits do not affect decoding */
    }
    media[40][33] = 1;
    for (u32 flags = 1; flags < 256U; flags++) {
        media[40][65] = flags;
        exfat_put16(media[40] + 2U, exfat_entry_checksum(media[40], 3U));
        assert(exfat_entry_set_read(&fs, 2U, false, 0U, &entry) ==
               ((flags & 3U) == 0U));
    }
    media[40][65] = 0;
    media[40][34] = 1; /* stream reserved byte */
    exfat_put16(media[40] + 2U, exfat_entry_checksum(media[40], 3U));
    assert(exfat_entry_set_read(&fs, 2U, false, 0U, &entry));
    media[40][34] = 0;
    exfat_put32(media[40] + 52U, 5U);
    exfat_put64(media[40] + 56U, 1U);
    for (u32 flags = 0; flags < 256U; flags++) {
        media[40][33] = flags;
        exfat_put16(media[40] + 2U, exfat_entry_checksum(media[40], 3U));
        assert(exfat_entry_set_read(&fs, 2U, false, 0U, &entry) ==
               ((flags & 1U) != 0U));
    }
    media[40][33] = 3U;
    exfat_put32(media[40] + 52U, 0U);
    exfat_put16(media[40] + 2U, exfat_entry_checksum(media[40], 3U));
    assert(!exfat_entry_set_read(&fs, 2U, false, 0U, &entry));
    media[40][33] = 0x81U;
    media[40][34] = 0x5aU;
    exfat_put16(media[40] + 2U, exfat_entry_checksum(media[40], 3U));
    exfat_node_t *state = accent_node->fs_data;
    assert(exfat_node_metadata_write(state, &fs));
    assert(media[40][33] == 0x81U && media[40][34] == 0x5aU);
    exfat_replace_transaction_t tx = {0};
    tx.entry_offset = 0U;
    tx.new_no_fat = true;
    tx.new_first = 5U;
    tx.new_length = 1U;
    assert(exfat_prepare_metadata(&fs, state, &tx, 5U, true, 1U, 1U) == VFS_OK);
    assert(tx.proposed_sector[33] == 0x83U && tx.proposed_sector[34] == 0x5aU);
    /* Ownership admission remains conservative about reserved stream bits. */
    assert(exfat_ownership_preflight_locked(accent_node) == VFS_ERR_BAD_FORMAT);
    cleanup();
    puts("exFAT independent specification vectors passed");
}

static void metadata_pair_regressions(void)
{
    for (u32 fault = 0; fault < 5; fault++) {
        setup();
        ((u32 *)media[24])[31] = EXFAT_CLUSTER_EOC;
        ((u32 *)media[24])[32] = 0x12345678U; /* stale free-cluster FAT value */
        u8 original[512]; memcpy(original, media[24], 512);
        exfat_replace_transaction_t tx = {0};
        u32 count = fs.boot.cluster_count;
        fs.boot.cluster_count = 300;
        assert(exfat_replace_fat_update_pair(&fs, &tx, 32, false,
               EXFAT_CLUSTER_EOC, 128) == VFS_ERR_BAD_FORMAT);
        assert(exfat_replace_fat_update_pair(&fs, &tx, 32, false,
               EXFAT_CLUSTER_EOC, 32) == VFS_ERR_BAD_FORMAT);
        assert(!writes && !reads);
        fs.boot.cluster_count = count;
        if (fault == 1) fail_read_lba = 24;
        if (fault == 2) ((u32 *)media[24])[31] = 7;
        if (fault == 3) fail_write_lba = 24;
        if (fault == 4) online = false;
        int result = exfat_replace_fat_update_pair(&fs, &tx, 32, false,
                                                   EXFAT_CLUSTER_EOC, 31);
        if (!fault) {
            assert(result == VFS_OK && writes == 1 && !tx.metadata_pending);
            assert(((u32 *)media[24])[31] == 32 &&
                   ((u32 *)media[24])[32] == EXFAT_CLUSTER_EOC);
            assert(!memcmp(original, media[24], 124) &&
                   !memcmp(original + 132, media[24] + 132, 380));
        } else if (fault != 3) {
            assert(result == (fault == 2 ? VFS_ERR_BAD_FORMAT : VFS_ERR_IO));
            assert(!writes && !tx.metadata_pending);
        } else {
            assert(result == VFS_ERR_UNCERTAIN && tx.metadata_pending &&
                   tx.metadata_partner == 31 && tx.metadata_cluster == 32 &&
                   tx.fat_copy_progress == 0);
            assert(!memcmp(tx.metadata_before, original, 512) &&
                   !memcmp(tx.fat_before[0], original, 512));
            assert(exfat_le32(tx.metadata_proposed + 31U * 4U) == 32U &&
                   exfat_le32(tx.metadata_proposed + 32U * 4U) ==
                       EXFAT_CLUSTER_EOC);
            assert(((u32 *)media[24])[31] == 32 &&
                   ((u32 *)media[24])[32] == 0x12345678U);
        }
        cleanup();
    }
    setup(); add_system_entries();
    memset(media[41], 0xff, 512);
    media[41][0] &= (u8)~((1U << 4) | (1U << 6)); /* only clusters 6,8 */
    vfs_node_t prepared = {0};
    vfs_file_facts_t facts;
    u8 input[1024]; memset(input, 'P', sizeof(input));
    u64 pin = 0;
    fail_write_number = 7; /* second bitmap is confirmed, paired FAT tears */
    assert(exfat_create_contents_locked(&root, &prepared, "paired", 1024,
            replace_source, input, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_unresolved && !fs.version_confident &&
           fs.replace_transaction->fresh_count == 2 &&
           fs.replace_transaction->metadata_partner == 6 &&
           fs.replace_transaction->metadata_cluster == 8 &&
           fs.replace_transaction->metadata_pending && !prepared.fs_data);
    assert(media[40][160] == 0); /* namespace was never published */
    discard_unresolved_fixture_pin();
    cleanup();
}

static vfs_node_t *prepare_split_ordinary_file(void)
{
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    setup();
    add_system_entries();
    ((u32 *)media[24])[2] = 5U;
    ((u32 *)media[24])[5] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 1U << 3U; /* Cluster five belongs to root. */
    root_state.data_length = root_state.valid_data_length = 1024U;
    root.size = 1024U;
    for (u32 at = 160U; at < 480U; at += EXFAT_ENTRY_BYTES)
        media[40][at] = EXFAT_ENTRY_DELETED;
    assert(exfat_build_file_set(&fs, "split.txt", 0U, 0U, 0U, 0U,
                                false, set, &count) && count == 3U);
    memcpy(media[40] + 480U, set, EXFAT_ENTRY_BYTES);
    memcpy(media[43], set + EXFAT_ENTRY_BYTES,
           2U * EXFAT_ENTRY_BYTES);
    vfs_node_t *file = exfat_finddir(&root, "split.txt");
    assert(file && ((exfat_node_t *)file->fs_data)->entry_offset == 480U);
    return file;
}

static vfs_node_t *prepare_split_replacement(void)
{
    vfs_node_t *file = prepare_split_ordinary_file();
    u8 old[700];
    u64 written = 0, pin = 0;
    memset(old, 'O', sizeof(old));
    assert(exfat_node_write_pinned(file, 0U, sizeof(old), old,
                                   &written, &pin) == VFS_OK);
    finish_pin();
    assert(written == sizeof(old) &&
           ((exfat_node_t *)file->fs_data)->generation == 2U);
    return file;
}

typedef struct {
    vfs_node_t *file;
    const u8 *bytes;
    bool fail_metadata_read;
} split_replace_source_t;

static u32 split_replace_source(void *context, u64 offset, u8 *buffer,
                                 u32 requested)
{
    split_replace_source_t *source = context;
    exfat_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx && pin_active && !tx->primary_attempted && !tx->stream_attempted);
    assert(!tx->reclamation_cursor && source->file->size == 700U &&
           ((exfat_node_t *)source->file->fs_data)->generation == 2U);
    for (u32 i = 0; i < tx->old_count; i++) {
        bool used;
        u32 cluster = exfat_replace_old_cluster(tx, i);
        assert(exfat_bitmap_bit(&fs, cluster, &used) && used &&
               media[exfat_cluster_lba(&fs, cluster)][0] == 'O');
    }
    memcpy(buffer, source->bytes + offset, requested);
    if (source->fail_metadata_read) fail_read_lba = 40U;
    return requested;
}

static void split_replacement_regressions(void)
{
    u8 input[513], actual[513], before_primary[512], before_stream[512];
    u64 pin;
    unsigned start_writes, start_reads, start_flushes, start_allocations;
    usize baseline;
    clock_t start;
    memset(input, 'N', sizeof(input));
    input[512] = 'Z';

    vfs_node_t *file = prepare_split_replacement();
    u64 inode = file->inode;
    u32 old_first = ((exfat_node_t *)file->fs_data)->first_cluster;
    memcpy(before_primary, media[40], 512U);
    memcpy(before_stream, media[43], 512U);
    baseline = live_requested;
    peak_requested = baseline;
    start_writes = writes; start_reads = reads; start_flushes = flushes;
    start_allocations = allocations; start = clock();
    split_replace_source_t source = {file, input, false};
    pin = 0;
    assert(exfat_replace_locked(&super, inode, 2U, sizeof(input),
        split_replace_source, &source, &pin) == VFS_OK && pin && pin_active);
    exfat_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx->old_no_fat && !tx->old_clusters && tx->old_first == old_first &&
           tx->old_count == 2U && tx->fresh_count == 2U &&
           tx->staging_flushed && tx->publication_flushed &&
           tx->stream_attempted && tx->primary_attempted &&
           tx->stream_written && tx->primary_written &&
           tx->publication_confirmed && tx->reclamation_cursor == 2U &&
           !memcmp(tx->old_sector, before_primary, 512U) &&
           !memcmp(tx->old_stream_sector, before_stream, 512U) &&
           !memcmp(before_primary, media[40], 480U) &&
           !memcmp(before_primary + 480U, media[40] + 480U, 2U) &&
           !memcmp(before_primary + 484U, media[40] + 484U, 28U) &&
           !memcmp(before_stream + 32U, media[43] + 32U, 480U));
    assert(writes - start_writes == 8U && flushes - start_flushes == 3U);
    printf("exFAT split 700->513 replacement: peak=%zu retained=%zu "
           "allocations=%u reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline), allocations - start_allocations,
           reads - start_reads, writes - start_writes, flushes - start_flushes,
           1000.0 * (double)(clock() - start) / (double)CLOCKS_PER_SEC);
    finish_pin();
    assert(file->inode == inode && exfat_finddir(&root, "split.txt") == file &&
           exfat_node_read(file, 0U, sizeof(actual), actual) == sizeof(actual) &&
           !memcmp(actual, input, sizeof(input)) &&
           ((exfat_node_t *)file->fs_data)->generation == 3U &&
           exfat_ownership_preflight_locked(file) == VFS_OK);
    bool used;
    assert(exfat_bitmap_bit(&fs, old_first, &used) && !used &&
           exfat_bitmap_bit(&fs, old_first + 1U, &used) && !used);
    vfs_file_facts_t facts;
    assert(exfat_file_facts(file, &facts) == VFS_OK && facts.generation == 3U);
    mg_file_lines_state_t snapshot;
    assert(exfat_snapshot_stream(file, sizeof(actual), sink, actual,
        &snapshot) == VFS_OK && snapshot.revision == 3U &&
        !memcmp(actual, input, sizeof(input)));
    start_writes = writes;
    pin = 0;
    assert(exfat_replace_locked(&super, inode, 2U, 1U, replace_source,
        "x", &pin) == VFS_ERR_STALE_GENERATION && !pin &&
        !pin_active && writes == start_writes);
    assert(exfat_replace_locked(&super, inode, snapshot.revision,
        0U, NULL, NULL, &pin) == VFS_OK && pin);
    finish_pin();
    assert(file->size == 0U && ((exfat_node_t *)file->fs_data)->generation == 4U);
    assert(exfat_node_write_pinned(file, 0U, 1U, "W", &inode, &pin) == VFS_OK);
    finish_pin();
    assert(inode == 1U && file->size == 1U);
    cleanup();

    /* Every sector-write boundary: fresh bitmap/data, stream, primary,
     * and each old-cluster reclamation. An error can include a full write. */
    for (unsigned fault = 1U; fault <= 8U; fault++) {
        for (unsigned completion = 0U; completion < 3U; completion++) {
            file = prepare_split_replacement();
            old_first = ((exfat_node_t *)file->fs_data)->first_cluster;
            source = (split_replace_source_t){file, input, false};
            start_writes = writes;
            fail_write_number = start_writes + fault;
            fail_write_copy_bytes = completion == 0U ? 0U :
                                    completion == 1U ? 496U : 512U;
            pin = 0;
            assert(exfat_replace_locked(&super, file->inode, 2U, sizeof(input),
                split_replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
            tx = fs.replace_transaction;
            assert(!pin && pin_active && pin_unresolved && tx &&
                   pin_context == tx && !fs.version_confident &&
                   writes == start_writes + fault &&
                   tx->publication_confirmed == (fault >= 7U));
            if (fault <= 6U) {
                assert(!tx->reclamation_cursor && file->size == 700U &&
                       exfat_bitmap_bit(&fs, old_first, &used) && used &&
                       exfat_bitmap_bit(&fs, old_first + 1U, &used) && used &&
                       media[exfat_cluster_lba(&fs, old_first)][0] == 'O');
            } else {
                assert(tx->publication_flushed && file->size == sizeof(input) &&
                       tx->reclamation_cursor == fault - 7U);
            }
            assert(tx->stream_attempted == (fault >= 5U) &&
                   tx->primary_attempted == (fault >= 6U));
            unsigned retained_writes = writes;
            assert(exfat_replace_locked(&super, file->inode, 2U, 1U,
                replace_source, "x", &pin) == VFS_ERR_UNCERTAIN);
            u64 bytes = 0;
            assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes, &pin) ==
                   VFS_ERR_UNCERTAIN && !bytes);
            assert(exfat_truncate_pinned(file, &pin) == VFS_ERR_UNCERTAIN &&
                   writes == retained_writes && pin_context == tx);
            device.id++;
            assert(tx->old_count == 2U && tx->old_first == old_first &&
                   !memcmp(tx->old_sector + 480U, before_primary + 480U, 32U));
            discard_unresolved_fixture_pin();
            cleanup();
        }
    }
    for (unsigned fault = 1U; fault <= 3U; fault++) {
        file = prepare_split_replacement();
        old_first = ((exfat_node_t *)file->fs_data)->first_cluster;
        source = (split_replace_source_t){file, input, false};
        fail_flush_number = flushes + fault;
        pin = 0;
        assert(exfat_replace_locked(&super, file->inode, 2U, sizeof(input),
            split_replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
        tx = fs.replace_transaction;
        assert(!pin && pin_unresolved && tx &&
               tx->staging_flushed == (fault > 1U) &&
               tx->publication_confirmed == (fault > 1U) &&
               tx->publication_flushed == (fault > 2U) &&
               tx->reclamation_cursor == (fault == 3U ? 2U : 0U));
        assert(exfat_bitmap_bit(&fs, old_first, &used) && used == (fault < 3U));
        discard_unresolved_fixture_pin();
        cleanup();
    }
    for (unsigned boundary = 1U; boundary <= 8U; boundary++) {
        file = prepare_split_replacement();
        remove_after_write_number = writes + boundary;
        pin = 0;
        assert(exfat_replace_locked(&super, file->inode, 2U, sizeof(input),
            replace_source, input, &pin) == VFS_ERR_UNCERTAIN &&
            !pin && pin_unresolved && fs.replace_transaction);
        discard_unresolved_fixture_pin();
        cleanup();
    }

    /* Definite source/read failure cancels only fresh candidates and flushes
     * cancellation before handing the ACTIVE pin back to VFS. */
    for (unsigned fault = 0U; fault < 5U; fault++) {
        file = prepare_split_replacement();
        old_first = ((exfat_node_t *)file->fs_data)->first_cluster;
        source = (split_replace_source_t){file, input, true};
        if (fault == 2U) fail_bitmap_write_number = bitmap_writes + 2U;
        if (fault == 3U) fail_flush_number = flushes + 1U;
        if (fault == 4U) fail_bitmap_write_number = bitmap_writes + 4U;
        pin = 0;
        int result = exfat_replace_locked(&super, file->inode, 2U,
            fault == 1U || fault == 4U ? sizeof(input) : 1U,
            fault == 1U || fault == 4U ? split_replace_source : short_source,
            fault == 1U || fault == 4U ? &source : NULL, &pin);
        tx = fs.replace_transaction;
        assert(tx && !tx->stream_attempted && !tx->primary_attempted &&
               !tx->reclamation_cursor && file->size == 700U &&
               exfat_bitmap_bit(&fs, old_first, &used) && used);
        if (fault < 2U) {
            assert(result == VFS_ERR_IO && pin && !pin_unresolved &&
                   tx->cancellation_flushed &&
                   tx->cancellation_cursor == tx->fresh_count);
            for (u32 i = 0; i < tx->fresh_count; i++)
                assert(exfat_bitmap_bit(&fs, tx->fresh_clusters[i], &used) && !used);
            fail_read_lba = ~(u64)0;
            finish_pin();
            assert(fs.version_confident &&
                   ((exfat_node_t *)file->fs_data)->generation == 2U);
        } else {
            assert(result == VFS_ERR_UNCERTAIN && !pin && pin_unresolved);
            if (fault == 4U) assert(tx->cancellation_cursor == 1U);
            discard_unresolved_fixture_pin();
        }
        cleanup();
    }
    for (unsigned fault = 1U; fault <= 4U; fault++) {
        file = prepare_split_replacement();
        start_writes = writes;
        fail_allocation = allocations + fault;
        pin = 0;
        assert(exfat_replace_locked(&super, file->inode, 2U, sizeof(input),
            replace_source, input, &pin) == VFS_ERR_NO_MEM && !pin &&
            !pin_active && !fs.replace_transaction && writes == start_writes);
        cleanup();
    }
    file = prepare_split_replacement();
    start_writes = writes;
    fail_pin_begin = true;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 2U, sizeof(input),
        replace_source, input, &pin) == VFS_ERR_NO_SPACE && !pin &&
        !pin_active && !fs.replace_transaction && writes == start_writes);
    cleanup();
    for (unsigned fault = 1U; fault <= 2U; fault++) {
        file = prepare_split_ordinary_file(); /* Both dirty-flag copies. */
        fail_write_number = fault;
        pin = 0;
        assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
            replace_source, "x", &pin) == VFS_ERR_UNCERTAIN && !pin &&
            pin_unresolved && fs.replace_transaction->metadata_pending &&
            fs.replace_transaction->boot_progress == fault - 1U &&
            !fs.replace_transaction->fresh_count && file->size == 0U);
        discard_unresolved_fixture_pin();
        cleanup();
    }
    file = prepare_split_ordinary_file();
    fail_read_lba = 0U;
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
        replace_source, "x", &pin) == VFS_ERR_IO && pin && !pin_unresolved &&
        !writes && !flushes && !fs.replace_transaction->fresh_count);
    finish_pin();
    cleanup();
    for (unsigned fault = 2U; fault <= 5U; fault += 3U) {
        file = prepare_split_replacement();
        memset(media[41], 0xff, 512U);
        media[41][0] &= (u8)~(1U << (8U - 2U));
        media[41][1] &= (u8)~(1U << ((10U - 2U) % 8U));
        fail_write_number = writes + fault;
        pin = 0;
        assert(exfat_replace_locked(&super, file->inode, 2U, sizeof(input),
            replace_source, input, &pin) == VFS_ERR_UNCERTAIN && !pin &&
            pin_unresolved && fs.replace_transaction->metadata_pending &&
            fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_FAT &&
            fs.replace_transaction->fresh_count == (fault == 2U ? 1U : 2U) &&
            !fs.replace_transaction->reclamation_cursor && file->size == 700U);
        if (fault == 5U) assert(fs.replace_transaction->metadata_partner == 8U);
        discard_unresolved_fixture_pin();
        cleanup();
    }
    puts("exFAT split replacement: all publication, flush, cancellation and removal boundaries passed");
}

static void split_replacement_layouts(void)
{
    /* Maximum VFS name: immutable filename secondaries occupy two further
     * clusters, while only the primary and stream sectors are published. */
    setup();
    add_system_entries();
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count, names_before[1024], actual;
    char name[256]; memset(name, 'a', 255U); name[255] = '\0';
    ((u32 *)media[24])[2] = 5U;
    ((u32 *)media[24])[5] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[41][0] |= (1U << 3U) | (1U << 5U);
    for (u32 at = 160U; at < 480U; at += 32U)
        media[40][at] = EXFAT_ENTRY_DELETED;
    root_state.data_length = root_state.valid_data_length = root.size = 1536U;
    assert(exfat_build_file_set(&fs, name, 0U, 0U, 0U, 0U, false, set, &count) &&
           count == 19U);
    memcpy(media[40] + 480U, set, 32U);
    memcpy(media[43], set + 32U, 512U);
    memcpy(media[45], set + 544U, (usize)count * 32U - 544U);
    memcpy(names_before, media[43], 512U);
    memcpy(names_before + 512U, media[45], 512U);
    vfs_node_t *file = exfat_finddir(&root, name);
    assert(file);
    u64 pin = 0;
    unsigned before = writes;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
        replace_source, "M", &pin) == VFS_OK && writes - before == 6U);
    finish_pin();
    assert(!memcmp(names_before + 32U, media[43] + 32U, 480U) &&
           !memcmp(names_before + 512U, media[45], 512U) &&
           exfat_finddir(&root, name) == file &&
           exfat_node_read(file, 0U, 1U, &actual) == 1U && actual == 'M' &&
           exfat_ownership_preflight_locked(file) == VFS_OK);
    cleanup();

    /* A split entry set within a contiguous multi-sector root cluster. */
    setup();
    fs.sectors_per_cluster = 2U; fs.cluster_bytes = 1024U;
    fs.bitmap_length = fs.upcase_length = 512U;
    fs.upcase_cluster = 4U;
    root_state.data_length = root_state.valid_data_length = root.size = 1024U;
    memcpy(media[42], media[41], 512U);
    memset(media[41], 0, 512U);
    media[42][0] = 7U;
    ((u32 *)media[24])[4] = EXFAT_CLUSTER_EOC;
    memset(media[40], 0, 512U);
    media[40][0] = EXFAT_ENTRY_BITMAP;
    exfat_put32(media[40] + 20U, 3U);
    exfat_put64(media[40] + 24U, 512U);
    media[40][32] = EXFAT_ENTRY_UPCASE;
    exfat_put32(media[40] + 52U, 4U);
    exfat_put64(media[40] + 56U, 512U);
    for (u32 at = 64U; at < 480U; at += 32U)
        media[40][at] = EXFAT_ENTRY_DELETED;
    assert(exfat_build_file_set(&fs, "within.txt", 0U, 0U, 0U,
                                0U, false, set, &count));
    memcpy(media[40] + 480U, set, 32U);
    memcpy(media[41], set + 32U, (usize)(count - 1U) * 32U);
    file = exfat_finddir(&root, "within.txt");
    pin = 0;
    assert(file && exfat_replace_locked(&super, file->inode, 1U, 1U,
        replace_source, "I", &pin) == VFS_OK &&
        fs.replace_transaction->directory_sector == 40U &&
        fs.replace_transaction->stream_sector == 41U);
    finish_pin();
    assert(exfat_node_read(file, 0U, 1U, &actual) == 1U && actual == 'I' &&
           exfat_ownership_preflight_locked(file) == VFS_OK);
    cleanup();
}

static void split_replacement_maximum(void)
{
    setup_large();
    /* Fragmented root chain and a split imported entry set. */
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    for (u32 at = 0U; at < 96U; at += 32U)
        media[120][at] = EXFAT_ENTRY_DELETED;
    for (u32 at = 160U; at < 480U; at += 32U)
        media[120][at] = EXFAT_ENTRY_DELETED;
    ((u32 *)media[24])[2] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[121][0] |= 1U << 5U;
    root_state.data_length = root_state.valid_data_length = root.size = 1024U;
    assert(exfat_build_file_set(&fs, "large.txt", 0U, 0U, 0U, 0U,
                                false, set, &count));
    memcpy(media[120] + 480U, set, 32U);
    memcpy(media[125], set + 32U, (usize)(count - 1U) * 32U);
    for (u32 cluster = 8U; cluster <= 10001U; cluster += 2U) {
        u32 bit = cluster - 2U;
        media[121U + bit / 4096U][(bit / 8U) % 512U] |=
            (u8)(1U << (bit % 8U));
    }
    vfs_node_t *file = exfat_finddir(&root, "large.txt");
    u8 *large = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(file && large);
    memset(large, 'F', MG_FILE_CONTENT_MAX_BYTES);
    u64 pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U,
        MG_FILE_CONTENT_MAX_BYTES, replace_source, large, &pin) == VFS_OK &&
        !fs.replace_transaction->new_no_fat);
    finish_pin();
    /* Release the artificial blockers, preserving allocated file clusters. */
    for (u32 cluster = 8U; cluster <= 10001U; cluster += 2U) {
        u32 bit = cluster - 2U;
        media[121U + bit / 4096U][(bit / 8U) % 512U] &=
            (u8)~(1U << (bit % 8U));
    }
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned before_reads = reads, before_writes = writes,
             before_flushes = flushes, before_allocations = allocations;
    clock_t start = clock();
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 2U,
        MG_FILE_CONTENT_MAX_BYTES, replace_source, large, &pin) == VFS_OK);
    assert(fs.replace_transaction->old_count == 4096U &&
           !fs.replace_transaction->old_no_fat &&
           fs.replace_transaction->fresh_count == 4096U &&
           !fs.replace_transaction->new_no_fat);
    assert(live_requested - baseline ==
           sizeof(exfat_replace_transaction_t) + 8192U * sizeof(u32));
    printf("exFAT split fragmented 2 MiB old/new: peak=%zu retained=%zu "
           "allocations=%u reads=%u writes=%u flushes=%u cpu_ms=%.3f "
           "max_32_pins=%zu bytes\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline), allocations - before_allocations,
           reads - before_reads, writes - before_writes, flushes - before_flushes,
           1000.0 * (double)(clock() - start) / (double)CLOCKS_PER_SEC,
           32U * (sizeof(exfat_replace_transaction_t) + 8192U * sizeof(u32)));
    finish_pin();
    u8 last;
    assert(exfat_node_read(file, MG_FILE_CONTENT_MAX_BYTES - 1U, 1U, &last) ==
           1U && last == 'F' && exfat_ownership_preflight_locked(file) == VFS_OK);
    pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 3U, 1U,
        replace_source, "C", &pin) == VFS_OK &&
        fs.replace_transaction->new_no_fat);
    finish_pin();
    assert(exfat_node_read(file, 0U, 1U, &last) == 1U && last == 'C');
    free(large);
    cleanup();
}

static void ordinary_split_regressions(void)
{
    vfs_node_t *file;
    u64 bytes, pin;
    exfat_entry_info_t entry;
    u8 original_primary[512], original_stream[512];
    usize success_peak;
    unsigned success_reads, success_writes, success_flushes;
    unsigned first_reads, first_writes, first_flushes;
    unsigned first_before_reads, first_before_writes, first_before_flushes;
    clock_t success_start, first_start;
    double success_cpu_ms;
    double first_cpu_ms;

    file = prepare_split_ordinary_file();
    memcpy(original_primary, media[40], 512U);
    memcpy(original_stream, media[43], 512U);
    success_start = first_start = clock();
    first_before_reads = reads;
    first_before_writes = writes;
    first_before_flushes = flushes;
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 5U, "hello", &bytes,
                                   &pin) == VFS_OK && bytes == 5U &&
           pin == next_pin_id && pin_active && !pin_unresolved);
    assert(exfat_entry_set_read(&fs, 2U, false, 480U, &entry) &&
           entry.data_length == 5U && entry.valid_data_length == 5U &&
           entry.first_cluster >= 2U && file->size == 5U &&
           !memcmp(media[40], original_primary, 480U) &&
           !memcmp(media[43] + 32U, original_stream + 32U, 480U));
    u32 allocated = entry.first_cluster;
    finish_pin();
    first_reads = reads - first_before_reads;
    first_writes = writes - first_before_writes;
    first_flushes = flushes - first_before_flushes;
    first_cpu_ms = 1000.0 * (double)(clock() - first_start) /
                   (double)CLOCKS_PER_SEC;
    assert(!fs.replace_transaction && fs.version_confident);
    u64 stale_pin = 0;
    assert(exfat_replace_locked(&super, file->inode, 1U, 1U,
        replace_source, "x", &stale_pin) == VFS_ERR_STALE_GENERATION &&
        !stale_pin && !pin_active);
    char readback[8] = {0};
    assert(exfat_node_read(file, 0U, 5U, readback) == 5U &&
           !memcmp(readback, "hello", 5U));
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 1U, 2U, "XY", &bytes,
                                   &pin) == VFS_OK && bytes == 2U);
    finish_pin();
    assert(exfat_node_read(file, 0U, 5U, readback) == 5U &&
           !memcmp(readback, "hXYlo", 5U));
    pin = 0;
    assert(exfat_truncate_pinned(file, &pin) == VFS_OK &&
           pin == next_pin_id && file->size == 0U);
    finish_pin();
    assert(exfat_entry_set_read(&fs, 2U, false, 480U, &entry) &&
           entry.data_length == 0U && !entry.first_cluster &&
           !(media[41][(allocated - 2U) / 8U] &
             (1U << ((allocated - 2U) % 8U))));
    success_peak = peak_requested;
    success_reads = reads;
    success_writes = writes;
    success_flushes = flushes;
    success_cpu_ms = 1000.0 * (double)(clock() - success_start) /
                     (double)CLOCKS_PER_SEC;
    cleanup();

    file = prepare_split_ordinary_file();
    memcpy(original_primary, media[40], 512U);
    memcpy(original_stream, media[43], 512U);
    fail_write_lba = 40U; /* Stream confirmed, primary checksum tears. */
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           !bytes && !pin && pin_unresolved && !fs.version_confident);
    assert(fs.replace_transaction->stream_attempted &&
           fs.replace_transaction->primary_attempted &&
           !fs.replace_transaction->publication_confirmed &&
           fs.replace_transaction->directory_sector == 40U &&
           fs.replace_transaction->stream_sector == 43U &&
           !memcmp(fs.replace_transaction->old_sector,
                   original_primary, 512U) &&
           !memcmp(fs.replace_transaction->old_stream_sector,
                   original_stream, 512U) &&
           memcmp(media[43], original_stream, 512U));
    unsigned retained_writes = writes;
    assert(exfat_node_write_pinned(file, 0U, 1U, "y", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           writes == retained_writes && pin_unresolved);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    fail_write_lba = 43U; /* Stream publication itself tears. */
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           !bytes && !pin && pin_unresolved &&
           fs.replace_transaction->stream_attempted &&
           !fs.replace_transaction->primary_attempted);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    fail_flush_number = 2U; /* Both metadata sectors were written. */
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           !bytes && !pin && pin_unresolved &&
           fs.replace_transaction->publication_confirmed &&
           file->size == 1U);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    fail_pin_begin = true;
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_NO_SPACE &&
           !bytes && !pin && !writes && !pin_active);
    cleanup();

    for (unsigned fault = 1U; fault <= 4U; fault++) {
        file = prepare_split_ordinary_file();
        fail_write_number = fault;
        bytes = pin = 0;
        assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                       &pin) == VFS_ERR_UNCERTAIN &&
               !bytes && !pin && pin_unresolved &&
               fs.replace_transaction &&
               !fs.version_confident);
        discard_unresolved_fixture_pin();
        cleanup();
    }
    for (unsigned fault = 1U; fault <= 2U; fault++) {
        file = prepare_split_ordinary_file();
        fail_flush_number = fault;
        bytes = pin = 0;
        assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                       &pin) == VFS_ERR_UNCERTAIN &&
               !bytes && !pin && pin_unresolved &&
               fs.replace_transaction);
        assert(fs.replace_transaction->publication_confirmed ==
               (fault == 2U));
        discard_unresolved_fixture_pin();
        cleanup();
    }
    file = prepare_split_ordinary_file();
    remove_after_write_number = 1U;
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           !bytes && pin_unresolved && fs.replace_transaction);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_OK && bytes == 1U);
    finish_pin();
    u32 old_cluster = ((exfat_node_t *)file->fs_data)->first_cluster;
    fail_bitmap_write_number = bitmap_writes + 1U;
    pin = 0;
    assert(exfat_truncate_pinned(file, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && pin_unresolved && file->size == 0U &&
           fs.replace_transaction->publication_confirmed &&
           fs.replace_transaction->reclamation_cursor == 0U &&
           exfat_replace_old_cluster(fs.replace_transaction, 0U) == old_cluster);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_OK);
    finish_pin();
    u32 preserved_cluster = ((exfat_node_t *)file->fs_data)->first_cluster;
    fail_write_lba = 43U;
    pin = 0;
    assert(exfat_truncate_pinned(file, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && pin_unresolved &&
           fs.replace_transaction->stream_attempted &&
           !fs.replace_transaction->primary_attempted &&
           fs.replace_transaction->reclamation_cursor == 0U &&
           ((exfat_node_t *)file->fs_data)->first_cluster ==
               preserved_cluster);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 512U,
        (const char[512]){[0] = 'A'}, &bytes, &pin) == VFS_OK &&
        bytes == 512U);
    finish_pin();
    u32 first = ((exfat_node_t *)file->fs_data)->first_cluster;
    assert(first >= 6U && ((exfat_node_t *)file->fs_data)->no_fat_chain);
    u8 blocker_set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 blocker_count;
    assert(exfat_build_file_set(&fs, "blocker", 0U, first + 1U,
                                512U, 512U, true,
                                blocker_set, &blocker_count));
    memcpy(media[43] + 64U, blocker_set,
           blocker_count * EXFAT_ENTRY_BYTES);
    media[41][(first - 1U) / 8U] |=
        (u8)(1U << ((first - 1U) % 8U));
    assert(exfat_finddir(&root, "blocker"));
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 512U, 1U, "Z", &bytes,
                                   &pin) == VFS_OK && bytes == 1U);
    finish_pin();
    assert(!((exfat_node_t *)file->fs_data)->no_fat_chain &&
           exfat_entry_set_read(&fs, 2U, false, 480U, &entry) &&
           !(entry.stream_flags & EXFAT_STREAM_NO_FAT_CHAIN) &&
           entry.data_length == 513U &&
           exfat_node_read(file, 0U, 1U, readback) == 1U &&
           readback[0] == 'A' &&
           exfat_node_read(file, 512U, 1U, readback) == 1U &&
           readback[0] == 'Z');
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 1024U, 1U, "Q", &bytes,
                                   &pin) == VFS_OK && bytes == 1U);
    finish_pin();
    assert(exfat_entry_set_read(&fs, 2U, false, 480U, &entry) &&
           entry.data_length == 1025U &&
           exfat_node_read(file, 1024U, 1U, readback) == 1U &&
           readback[0] == 'Q');
    cleanup();

    file = prepare_split_ordinary_file();
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 512U,
        (const char[512]){[0] = 'A'}, &bytes, &pin) == VFS_OK);
    finish_pin();
    first = ((exfat_node_t *)file->fs_data)->first_cluster;
    assert(exfat_build_file_set(&fs, "blocker", 0U, first + 1U,
                                512U, 512U, true,
                                blocker_set, &blocker_count));
    memcpy(media[43] + 64U, blocker_set,
           blocker_count * EXFAT_ENTRY_BYTES);
    media[41][(first - 1U) / 8U] |=
        (u8)(1U << ((first - 1U) % 8U));
    fail_write_lba = fs.boot.fat_offset;
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 512U, 1U, "Z", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           !bytes && !pin && pin_unresolved &&
           fs.replace_transaction->metadata_kind == EXFAT_REPLACE_META_FAT &&
           fs.replace_transaction->metadata_pending &&
           fs.replace_transaction->fresh_count == 1U &&
           fs.replace_transaction->fresh_bitmap_progress == 1U &&
           fs.replace_transaction->old_fat_progress == 0U);
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    fail_read_lba = 0U; /* Dirty-sector read, before any write attempt. */
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_IO &&
           !bytes && pin == next_pin_id && !writes && !pin_unresolved);
    finish_pin();
    assert(fs.version_confident && !fs.replace_transaction);
    cleanup();

    file = prepare_split_ordinary_file();
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 5U, "hello", &bytes,
                                   &pin) == VFS_OK && bytes == 5U);
    finish_pin();
    u32 data_cluster = ((exfat_node_t *)file->fs_data)->first_cluster;
    u64 data_lba = exfat_cluster_lba(&fs, data_cluster);
    fail_write_lba = data_lba;
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "X", &bytes,
                                   &pin) == VFS_ERR_UNCERTAIN &&
           !bytes && !pin && pin_unresolved &&
           fs.replace_transaction->data_attempted &&
           fs.replace_transaction->data_sector == data_lba &&
           fs.replace_transaction->old_no_fat &&
           fs.replace_transaction->old_first == data_cluster &&
           fs.replace_transaction->old_data_sector[0] == 'h' &&
           fs.replace_transaction->proposed_data_sector[0] == 'X');
    discard_unresolved_fixture_pin();
    cleanup();

    file = prepare_split_ordinary_file();
    memset(media[41], 0xff, 512U);
    bytes = pin = 0;
    unsigned before = writes;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_NO_SPACE &&
           !bytes && !pin && !pin_active && writes == before);
    cleanup();

    file = prepare_split_ordinary_file();
    fail_allocation = allocations + 1U;
    bytes = pin = 0;
    assert(exfat_node_write_pinned(file, 0U, 1U, "x", &bytes,
                                   &pin) == VFS_ERR_NO_MEM &&
           !bytes && !pin && !pin_active && !writes);
    cleanup();

    printf("exFAT ordinary split write: reads=%u writes=%u flushes=%u "
           "cpu_ms=%.3f; write/overwrite/truncate: peak=%zu reads=%u "
           "writes=%u flushes=%u cpu_ms=%.3f; tx=%zu max_pin=%zu "
           "max_32_pins=%zu bytes\n",
           first_reads, first_writes, first_flushes, first_cpu_ms,
           (size_t)success_peak, success_reads, success_writes,
           success_flushes, success_cpu_ms,
           sizeof(exfat_replace_transaction_t),
           sizeof(exfat_replace_transaction_t) +
               (size_t)EXFAT_SCAN_MAX_CLAIMS * sizeof(u32),
           32U * (sizeof(exfat_replace_transaction_t) +
                  (size_t)EXFAT_SCAN_MAX_CLAIMS * sizeof(u32)));
}

static void ordinary_large_imported_regression(void)
{
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    const u64 length = MG_FILE_CONTENT_MAX_BYTES + 1ULL;
    const u32 clusters = (u32)(length / 512U + 1U);
    u64 bytes = 0, pin = 0;
    usize retained;
    unsigned before_writes, before_reads;

    setup_large();
    assert(exfat_build_file_set(&fs, "old.txt", 0U, 7U,
                                length, length, true, set, &count));
    memcpy(media[120], set, count * EXFAT_ENTRY_BYTES);
    for (u32 cluster = 7U; cluster < 7U + clusters; cluster++) {
        u32 bit = cluster - 2U;
        media[121U + bit / 4096U][(bit / 8U) % 512U] |=
            (u8)(1U << (bit % 8U));
    }
    vfs_node_t *file = exfat_finddir(&root, "old.txt");
    assert(file && file->size == length);
    before_writes = writes;
    before_reads = reads;
    assert(exfat_node_write_pinned(file, length - 1U, 1U, "L", &bytes,
                                   &pin) == VFS_OK && bytes == 1U && pin);
    finish_pin();
    u8 actual = 0;
    assert(exfat_node_read(file, length - 1U, 1U, &actual) == 1U &&
           actual == 'L');
    pin = 0;
    assert(exfat_truncate_pinned(file, &pin) == VFS_OK && pin &&
           file->size == 0U);
    retained = live_requested;
    assert(fs.replace_transaction->old_no_fat &&
           fs.replace_transaction->old_first == 7U &&
           fs.replace_transaction->old_count == clusters &&
           !fs.replace_transaction->old_clusters);
    finish_pin();
    assert(exfat_entry_set_read(&fs, 2U, false, 0U,
           &(exfat_entry_info_t){0}) && file->size == 0U);
    printf("exFAT >2 MiB ordinary overwrite/truncate: peak=%zu "
           "retained=%zu reads=%u writes=%u\n",
           (size_t)peak_requested, (size_t)retained,
           reads - before_reads, writes - before_writes);
    cleanup();
}

static void metadata_view_regressions(void)
{
    setup_large();
    const u32 clusters[4] = {100, 700, 250, 999};
    for (unsigned i = 0; i < 4; i++) {
        ((u32 *)media[24 + clusters[i] / 128])[clusters[i] % 128] =
            i == 3 ? EXFAT_CLUSTER_EOC : clusters[i + 1];
        memset(media[120 + clusters[i] - 2], (int)(0x30 + i), 512);
    }
    exfat_read_view_t fat = {0};
    exfat_stream_view_t stream = {0};
    u8 actual[600], expected[600];
    unsigned before = reads, before_allocations = allocations;
    for (unsigned offset = 0; offset < 2048; offset += 32) {
        assert(exfat_dir_byte_read_view(&fs, 100, false, offset, actual, 32,
                                        &fat, &stream));
        for (unsigned i = 0; i < 32; i++) assert(actual[i] == 0x30 + offset / 512);
    }
    assert(reads - before == 7 && allocations == before_allocations);
    exfat_fs_t other = fs;
    before = reads;
    assert(exfat_dir_byte_read_view(&other, 100, false, 1536, actual, 1,
                                    &fat, &stream));
    assert(reads > before && stream.owner == &other && fat.owner == &other);
    /* Backward access resets the chain cursor; crossing reads preserve data. */
    assert(exfat_dir_byte_read(&fs, 100, false, 480, expected, sizeof(expected)));
    assert(exfat_dir_byte_read_view(&fs, 100, false, 480, actual, sizeof(actual),
                                    &fat, &stream));
    assert(!memcmp(actual, expected, sizeof(actual)));
    before = reads;
    online = false;
    assert(!exfat_dir_byte_read_view(&fs, 100, false, 480, actual, 1, &fat, &stream));
    assert(reads == before && !fat.valid && !stream.valid && !stream.mapped);
    online = true;
    fail_read_lba = 218;
    assert(!exfat_dir_byte_read_view(&fs, 100, false, 0, actual, 1, &fat, &stream));
    assert(!fat.valid && !stream.valid && !stream.mapped);
    fail_read_lba = ~(u64)0;
    assert(exfat_dir_byte_read_view(&fs, 100, false, 0, actual, 1, &fat, &stream));
    device.id++;
    assert(!exfat_dir_byte_read_view(&fs, 100, false, 0, actual, 1, &fat, &stream));
    device.id--;
    /* A new read-only operation must observe a changed mapping. */
    fat.valid = false; stream.valid = stream.mapped = false;
    ((u32 *)media[24])[100] = EXFAT_CLUSTER_EOC;
    assert(!exfat_dir_byte_read_view(&fs, 100, false, 512, actual, 1, &fat, &stream));
    assert(!fat.valid && !stream.valid && !stream.mapped);
    ((u32 *)media[24])[100] = 700;
    fail_read_lba = 24;
    assert(!exfat_dir_byte_read_view(&fs, 100, false, 512, actual, 1, &fat, &stream));
    assert(!fat.valid && !stream.valid && !stream.mapped);
    fail_read_lba = ~(u64)0;
    assert(exfat_dir_byte_read_view(&fs, 100, false, 512, actual, 1, &fat, &stream));
    /* Switching first cluster or NoFatChain semantics never reuses a mapping. */
    assert(exfat_dir_byte_read_view(&fs, 700, true, 0, actual, 1, &fat, &stream));
    assert(actual[0] == 0x31);
    assert(!exfat_dir_byte_read_view(&fs, 10001, true, 512, actual, 1, &fat, &stream));
    assert(!fat.valid && !stream.valid && !stream.mapped);
    assert(!writes && allocations == before_allocations);
    cleanup();
}

static void metadata_directory_workload(bool fragmented)
{
    setup_large();
    u32 clusters[128];
    for (u32 i = 0; i < 128; i++)
        clusters[i] = i == 0 ? 2U : fragmented ? 7000U + 2U * i : 6U + i;
    for (u32 i = 0; i < 128; i++) {
        u32 c = clusters[i], bit = c - 2;
        ((u32 *)media[24 + c / 128])[c % 128] =
            i + 1 == 128 ? EXFAT_CLUSTER_EOC : clusters[i + 1];
        media[121 + bit / 4096][(bit / 8) % 512] |= (u8)(1U << (bit % 8));
        u64 lba = 120 + c - 2;
        if (i) memset(media[lba], 0, 512);
        for (u32 at = i ? 0 : 160; at < 512; at += 32)
            media[lba][at] = EXFAT_ENTRY_DELETED; /* reusable inactive file entry */
    }
    memset(media[120 + clusters[127] - 2] + 480, 0, 32);
    unsigned before_reads = reads, before_writes = writes;
    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    assert(node); memset(node, 0, sizeof(*node));
    vfs_file_facts_t facts;
    u64 pin = 0;
    assert(exfat_create_contents_locked(&root, node, "bigdir", 0,
           NULL, NULL, &facts, &pin) == VFS_OK);
    assert(pin && facts.size == 0 && root_state.generation == 2);
    printf("exFAT %s 128-cluster directory create: %u reads, %u writes\n",
           fragmented ? "fragmented" : "contiguous",
           reads - before_reads, writes - before_writes);
    assert(reads - before_reads < 400);
    finish_pin();
    bind_created_node(node, &facts);
    unsigned saved_writes = writes;
    vfs_node_t *other = kmalloc(sizeof(vfs_node_t));
    assert(other); memset(other, 0, sizeof(*other));
    assert(exfat_create_contents_locked(&root, other, "BIGDIR", 0,
           NULL, NULL, &facts, &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(writes == saved_writes);
    kfree(other);
    u32 c = clusters[126];
    ((u32 *)media[24 + c / 128])[c % 128] = clusters[1];
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident);
    cleanup();
}

static void external_directory_growth(const char *path, bool nonroot)
{
    FILE *file;
    exfat_fs_t *mounted;
    vfs_node_t *created;
    vfs_file_facts_t facts;
    u64 pin;

    setup();
    device.sector_count = 12000U;
    file = fopen(path, "r+b");
    assert(file && fread(media, sizeof(media), 1U, file) == 1U);
    mounted = exfat_new_state(&device);
    assert(mounted && mounted->cluster_bytes == 4096U);
    fs = *mounted;
    kfree(mounted);
    fs.next_inode = 1U;
    fs.nodes = &root_state;
    root_state.first_cluster = fs.root_cluster;
    root_state.data_length = root_state.valid_data_length = fs.cluster_bytes;
    root.size = fs.cluster_bytes;
    vfs_node_t *directory = &root;
    if (nonroot) {
        /* Place the parent's primary at the last entry of a sector so its
         * stream and checksum require separate prepared-sector writes. */
        u64 slot = 0;
        for (u32 index = 0; index < 16U; index++) {
            char name[16];
            vfs_node_t *ordinary = NULL;
            assert(exfat_find_free_entries(&fs, fs.root_cluster, false,
                                           0U, 3U, &slot));
            if (slot % EXFAT_SECTOR_BYTES == 480U) break;
            snprintf(name, sizeof(name), "p%02u.txt", index);
            assert(exfat_create(&root, name, &ordinary) == VFS_OK && ordinary);
        }
        assert(slot % EXFAT_SECTOR_BYTES == 480U);
        int mkdir_result = exfat_mkdir(&root, "nested", &directory);
        assert(mkdir_result == VFS_OK &&
               directory && directory->type == VFS_TYPE_DIRECTORY &&
               ((exfat_node_t *)directory->fs_data)->entry_offset %
                   EXFAT_SECTOR_BYTES == 480U);
    }
    for (u32 index = 0; index < (nonroot ? 42U : 41U); index++) {
        char name[16];
        vfs_node_t *ordinary = NULL;
        snprintf(name, sizeof(name), "e%02u.txt", index);
        assert(exfat_create(directory, name, &ordinary) == VFS_OK && ordinary);
    }
    created = prepared_created_node();
    assert(exfat_create_contents_locked(directory, created, "growth.txt", 1U,
        replace_source, "G", &facts, &pin) == VFS_OK);
    assert(fs.replace_transaction->directory_growth &&
           fs.replace_transaction->directory_growth->appended);
    if (nonroot)
        assert(fs.replace_transaction->directory_growth->parent_update &&
               fs.replace_transaction->directory_growth->parent_primary_sector !=
               fs.replace_transaction->directory_growth->parent_stream_sector);
    bind_created_node(created, &facts);
    finish_pin();
    assert(exfat_finddir(directory, "growth.txt") == created &&
           (!nonroot ||
            ((exfat_node_t *)directory->fs_data)->data_length ==
                fs.cluster_bytes * 2U) &&
           exfat_set_volume_dirty(&fs, false) &&
           block_flush_for_id(&device, fs.device_id));
    rewind(file);
    assert(fwrite(media, sizeof(media), 1U, file) == 1U &&
           fflush(file) == 0 && fclose(file) == 0);
    kfree(fs.upcase);
    fs.upcase = NULL;
    cleanup();
    puts(nonroot ? "exFAT external-image non-root growth and readback passed" :
                   "exFAT external-image root growth and readback passed");
}

static void external_split_file(const char *path, bool replacement)
{
    FILE *file;
    exfat_fs_t *mounted;
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    u8 contents[700], actual[700];
    u64 pin = 0, written = 0;
    u64 root_lba;
    u32 end = 0;

    setup();
    device.sector_count = 12000U;
    file = fopen(path, "r+b");
    assert(file && fread(media, sizeof(media), 1U, file) == 1U);
    mounted = exfat_new_state(&device);
    assert(mounted && mounted->cluster_bytes >= 1024U);
    fs = *mounted;
    kfree(mounted);
    fs.next_inode = 1U;
    fs.nodes = &root_state;
    root_state.first_cluster = fs.root_cluster;
    root_state.data_length = root_state.valid_data_length = fs.cluster_bytes;
    root.size = fs.cluster_bytes;
    root_lba = exfat_cluster_lba(&fs, fs.root_cluster);
    assert(root_lba + 1U < device.sector_count);
    if (!replacement) {
        while (end < 480U && media[root_lba][end] != EXFAT_ENTRY_END)
            end += EXFAT_ENTRY_BYTES;
        assert(end < 480U);
        for (u32 at = end; at < 480U; at += EXFAT_ENTRY_BYTES)
            media[root_lba][at] = EXFAT_ENTRY_DELETED;
        assert(!media[root_lba + 1U][0]);
        assert(exfat_build_file_set(&fs, "splitvalid.txt", 0U, 0U,
                                    0U, 0U, false, set, &count) && count == 3U);
        memcpy(media[root_lba] + 480U, set, EXFAT_ENTRY_BYTES);
        memcpy(media[root_lba + 1U], set + EXFAT_ENTRY_BYTES,
               2U * EXFAT_ENTRY_BYTES);
    }
    vfs_node_t *target = exfat_finddir(&root, "splitvalid.txt");
    assert(target &&
           ((exfat_node_t *)target->fs_data)->entry_offset == 480U);
    for (u32 at = 0; at < sizeof(contents); at++)
        contents[at] = (u8)(at * 17U + 3U);
    if (!replacement) {
        assert(exfat_node_write_pinned(target, 0U, sizeof(contents), contents,
                                       &written, &pin) == VFS_OK &&
               written == sizeof(contents) && pin);
        finish_pin();
    }
    assert(exfat_node_read(target, 0U, sizeof(actual), actual) ==
           sizeof(actual) && !memcmp(actual, contents, sizeof(actual)));
    if (replacement) {
        u8 proposed[1300], readback[1300];
        mg_file_lines_state_t snapshot;
        assert(exfat_snapshot_stream(target, sizeof(actual), sink, actual,
            &snapshot) == VFS_OK && snapshot.revision == 1U);
        for (u32 at = 0; at < sizeof(proposed); at++)
            proposed[at] = (u8)(at * 29U + 7U);
        usize baseline = live_requested;
        peak_requested = baseline;
        unsigned before_reads = reads, before_writes = writes,
                 before_flushes = flushes;
        clock_t start = clock();
        pin = 0;
        assert(exfat_replace_locked(&super, target->inode, snapshot.revision,
            sizeof(proposed), replace_source, proposed, &pin) == VFS_OK && pin &&
            fs.replace_transaction->directory_sector == root_lba &&
            fs.replace_transaction->stream_sector == root_lba + 1U &&
            fs.replace_transaction->old_count == 1U);
        printf("exFAT external split replacement: peak=%zu retained=%zu "
               "reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
               (size_t)(peak_requested - baseline),
               (size_t)(live_requested - baseline), reads - before_reads,
               writes - before_writes, flushes - before_flushes,
               1000.0 * (double)(clock() - start) / (double)CLOCKS_PER_SEC);
        finish_pin();
        assert(exfat_node_read(target, 0U, sizeof(readback), readback) ==
               sizeof(readback) && !memcmp(readback, proposed, sizeof(readback)));
        assert(exfat_finddir(&root, "splitvalid.txt") == target &&
               exfat_ownership_preflight_locked(target) == VFS_OK);
        pin = 0;
        assert(exfat_replace_locked(&super, target->inode, 2U,
            sizeof(contents), replace_source, contents, &pin) == VFS_OK && pin);
        finish_pin();
        assert(exfat_node_read(target, 0U, sizeof(actual), actual) ==
               sizeof(actual) && !memcmp(actual, contents, sizeof(actual)));
    }
    assert(exfat_set_volume_dirty(&fs, false) &&
           block_flush_for_id(&device, fs.device_id));
    rewind(file);
    assert(fwrite(media, sizeof(media), 1U, file) == 1U &&
           fflush(file) == 0 && fclose(file) == 0);
    kfree(fs.upcase);
    fs.upcase = NULL;
    cleanup();
    puts(replacement ? "exFAT external-image split conditional replacement/readback passed" :
                       "exFAT external-image split entry-set ordinary write/readback passed");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--ordinary-split-image")) {
        external_split_file(argv[2], false);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--replace-split-image")) {
        external_split_file(argv[2], true);
        return 0;
    }
    ordinary_split_regressions();
    ordinary_large_imported_regression();
    split_replacement_regressions();
    split_replacement_layouts();
    split_replacement_maximum();
    metadata_pair_regressions();
    metadata_view_regressions();
    metadata_directory_workload(false);
    metadata_directory_workload(true);
    specification_regressions();
    system_stream_regressions();
    setup();
    add_system_entries();
    vfs_node_t *file = exfat_finddir(&root, "old.txt");
    assert(file && file->inode != root.inode);
    assert(exfat_finddir(&root, "old.txt") == file);
    vfs_file_facts_t facts;
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 1 && facts.size == 0);
    u64 ordinary_written = 0, ordinary_pin = 0;
    assert(exfat_node_write_pinned(file, 0, 5, "hello",
                                   &ordinary_written, &ordinary_pin) == VFS_OK &&
           ordinary_written == 5U && ordinary_pin == next_pin_id);
    finish_pin();
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
    ordinary_pin = 0;
    assert(exfat_truncate_pinned(file, &ordinary_pin) == VFS_OK &&
           ordinary_pin == next_pin_id && file->size == 0);
    finish_pin();
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
    ordinary_pin = ordinary_written = 0;
    assert(exfat_node_write_pinned(new_file, 0, 1, "x",
                                   &ordinary_written, &ordinary_pin) ==
           VFS_ERR_UNCERTAIN && !ordinary_written && !ordinary_pin &&
           pin_active && pin_unresolved);
    assert(!fs.version_confident);
    assert(exfat_file_facts(new_file, &facts) == VFS_ERR_UNCERTAIN);
    unsigned attempted = writes;
    device.id++;
    assert(!exfat_read(&fs, 40, 1, data));
    assert(writes == attempted);
    discard_unresolved_fixture_pin();
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
    create_contents_case(0U);
    create_contents_case(3U);
    create_contents_case(512U * 1024U);
    create_contents_case(MG_FILE_CONTENT_MAX_BYTES);
    create_contents_failures();
    create_contents_fragmented_maximum();
    root_growth_regressions();
    root_growth_maximum();
    contiguous_directory_mapping_regressions();
    contiguous_growth_regressions();
    contiguous_conversion_faults();
    split_contiguous_conversion();
    multisector_conversion_regressions();
    nonroot_growth_regressions();
    nonroot_growth_maximum();
    if (argc > 1) external_directory_growth(argv[1], false);
    if (argc > 2) external_directory_growth(argv[2], true);
    puts("exFAT mount identities, generations, snapshots, ownership and safe creation passed");
    return 0;
}
