/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production exFAT functions with an in-memory mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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
    /* The end marker has only two slots left in this sector.  Publishing
     * an entry set in the next sector would leave it invisible. */
    for (u32 offset = 160U; offset < 448U; offset += EXFAT_ENTRY_BYTES)
        media[40][offset] = EXFAT_ENTRY_LABEL;
    before = writes;
    assert(exfat_create_contents_locked(&root, &prepared, "new.txt", 0U,
               NULL, NULL, &facts, &pin) == VFS_ERR_NO_SPACE);
    assert(!pin_active && writes == before);
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
    memcpy(tx.entry_set, media[40], 96U);
    memcpy(tx.proposed_sector, media[40], 512U);
    exfat_replace_build_sector(state, &tx);
    assert(tx.proposed_sector[33] == 0x83U && tx.proposed_sector[34] == 0x5aU);
    /* Ownership admission remains conservative about reserved stream bits. */
    assert(exfat_ownership_preflight_locked(accent_node) == VFS_ERR_BAD_FORMAT);
    cleanup();
    puts("exFAT independent specification vectors passed");
}

int main(void)
{
    specification_regressions();
    system_stream_regressions();
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
    create_contents_case(0U);
    create_contents_case(3U);
    create_contents_case(512U * 1024U);
    create_contents_case(MG_FILE_CONTENT_MAX_BYTES);
    create_contents_failures();
    create_contents_fragmented_maximum();
    puts("exFAT mount identities, generations, snapshots, ownership and safe creation passed");
    return 0;
}
