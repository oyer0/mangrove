/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host fixture includes the production MGFS implementation. Only block I/O,
 * heap, operation-lock, and recovery-pin dependencies are controlled here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../../kernel/src/storage/mgfs.c"

#define FIXTURE_BLOCKS 3200U
static u8 media[FIXTURE_BLOCKS][MGFS_BLOCK_BYTES];
static u8 record_bits[MGFS_BLOCK_BYTES];
static mgfs_record_ref_t ids[64];
static block_device_t device;
static mgfs_fs_t fs;
static vfs_super_t super;
static vfs_super_t retired_super;
static vfs_node_t *tracked_node;
static bool retarget_parent_on_size_update;
static unsigned reads, writes, fail_read, fail_write, allocations, fail_allocation;
static unsigned live_allocations, pin_begins, pin_finishes, pin_unresolved;
static usize live_bytes, peak_bytes;
static struct { void *pointer; usize bytes; } tracked_allocations[256];
static bool operation_locked, pin_live, pin_is_unresolved;
static u64 last_pin_id;
static unsigned table_writes, original_data_writes;
static unsigned old_high_storage_writes;
static unsigned second_table_writes, child_table_order, parent_table_order;
static unsigned failed_write_persistence;
static unsigned fail_bitmap_read_after_writes;
static unsigned fail_parent_tail_read_after_writes;
static bool fail_pin_begin, fail_pin_finish_busy, fail_mutex_wait;
static bool fail_mutex_reacquire, fail_handoff_callback;
static bool attempt_mutations_during_finish, remove_device_during_finish;
static bool ordinary_mutation_test;
static unsigned blocked_handoff_mutations;
static vfs_recovery_handoff_t pin_handoff;
static void *pin_context;

/* Unused MGFS node-operation references kept by the included static tables. */
void vfs_node_register(vfs_node_t *node) { (void)node; assert(false); }
void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size)
{
    assert(sb == &super);
    if (tracked_node && tracked_node->inode == inode) tracked_node->size = size;
    if (tracked_node && retarget_parent_on_size_update)
        tracked_node->super = &retired_super;
}
bool vfs_current_uid(u32 *out_uid)
{
    assert(out_uid);
    *out_uid = 7;
    return true;
}
bool vfs_check_access(const vfs_node_t *node, u32 access)
{
    assert(node && access == VFS_ACCESS_WRITE);
    return (node->owner_uid == 7 ?
            (node->permissions & VFS_PERMISSION_OWNER_WRITE) :
            (node->permissions & VFS_PERMISSION_OTHER_WRITE)) != 0;
}
void vfs_node_set_child_mutation_policy(
    vfs_node_t *node, vfs_child_mutation_policy_t policy)
{ (void)node; (void)policy; assert(false); }

void *kmalloc(usize size)
{
    if (++allocations == fail_allocation) return NULL;
    void *result = malloc(size);
    assert(result);
    unsigned slot = 0;
    while (slot < 256 && tracked_allocations[slot].pointer) slot++;
    assert(slot < 256);
    tracked_allocations[slot] = (typeof(tracked_allocations[0])){result, size};
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    live_allocations++;
    return result;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    assert(live_allocations);
    unsigned slot = 0;
    while (slot < 256 && tracked_allocations[slot].pointer != ptr) slot++;
    assert(slot < 256);
    live_bytes -= tracked_allocations[slot].bytes;
    tracked_allocations[slot].pointer = NULL;
    tracked_allocations[slot].bytes = 0;
    live_allocations--;
    free(ptr);
}

bool block_device_id_is_live(u64 id) { return id == device.id; }

bool block_read_for_id(block_device_t *dev, u64 id, u64 lba,
                       u32 sectors, void *buffer)
{
    assert(dev == &device && id == device.id && sectors == 8 && lba % 8 == 0);
    assert(lba / 8 < FIXTURE_BLOCKS);
    if (lba / 8 == 1 && writes >= fail_bitmap_read_after_writes &&
        fail_bitmap_read_after_writes) return false;
    if (lba / 8 == 7 && writes >= fail_parent_tail_read_after_writes &&
        fail_parent_tail_read_after_writes) return false;
    if (++reads == fail_read) return false;
    memcpy(buffer, media[lba / 8], MGFS_BLOCK_BYTES);
    return true;
}

bool block_write_for_id(block_device_t *dev, u64 id, u64 lba,
                        u32 sectors, const void *buffer)
{
    u64 block = lba / 8;
    assert(dev == &device && id == device.id && sectors == 8 && lba % 8 == 0);
    assert(block < FIXTURE_BLOCKS && operation_locked &&
           (pin_live || ordinary_mutation_test));
    if (block == 2) {
        table_writes++;
        if (fs.layout.record_table_blocks == 2) parent_table_order = writes + 1;
    }
    if (block == 3 && fs.layout.record_table_blocks == 2) {
        second_table_writes++;
        child_table_order = writes + 1;
    }
    if (block == 5) original_data_writes++;
    if ((block >= 1100 && block < 1100 + MGFS_REPLACE_MAX_BLOCKS) ||
        (block >= 2000 && block < 2000 + MGFS_REPLACE_MAX_BLOCKS))
        old_high_storage_writes++;
    if (++writes == fail_write) {
        if (failed_write_persistence == 1)
            memcpy(media[block], buffer, MGFS_BLOCK_BYTES);
        if (failed_write_persistence == 2)
            memcpy(media[block], buffer, MGFS_BLOCK_BYTES / 2);
        return false;
    }
    memcpy(media[block], buffer, MGFS_BLOCK_BYTES);
    return true;
}

bool block_flush_for_id(block_device_t *dev, u64 id)
{ assert(dev == &device && id == device.id && operation_locked); return true; }

bool vfs_super_retain(vfs_super_t *sb)
{
    assert(sb == &super);
    sb->reference_count++;
    return true;
}

void vfs_super_release(vfs_super_t *sb)
{
    assert(sb == &super && sb->reference_count);
    sb->reference_count--;
}

bool vfs_super_operation_lock(vfs_super_t *sb)
{
    assert(sb == &super && !operation_locked);
    if (sb->state == VFS_SUPER_DEAD) return false;
    operation_locked = true;
    return true;
}

void vfs_super_operation_unlock(vfs_super_t *sb)
{
    assert(sb == &super && operation_locked);
    operation_locked = false;
}

bool mutex_lock(mutex_t *mutex)
{
    assert(mutex == &super.operation_lock && !operation_locked);
    if (fail_mutex_wait || fail_mutex_reacquire) return false;
    operation_locked = true;
    return true;
}

bool mutex_unlock(mutex_t *mutex)
{
    assert(mutex == &super.operation_lock && operation_locked);
    operation_locked = false;
    return true;
}

/* Controlled VFS admission for driver-only cases; real gates have full-unit tests. */
void vfs_super_operation_pause(vfs_super_t *sb) { vfs_super_operation_unlock(sb); }
bool vfs_super_operation_guard_release(vfs_super_t *sb) { (void)sb; return true; }
int vfs_super_operation_error(const vfs_super_t *sb) { return vfs_super_is_live(sb) ? VFS_OK : VFS_ERR_DEVICE_GONE; }

int vfs_recovery_pin_begin_handoff(vfs_super_t *sb,
                                   vfs_recovery_handoff_t handoff,
                                   void *context, u64 *out_id)
{
    assert(sb == &super && operation_locked && !pin_live && handoff);
    if (fail_pin_begin) return VFS_ERR_NO_SPACE;
    pin_begins++;
    pin_live = true;
    pin_handoff = handoff;
    pin_context = context;
    *out_id = ++last_pin_id;
    sb->reference_count++;
    return VFS_OK;
}

int vfs_recovery_pin_mark_unresolved(u64 id)
{
    assert(pin_live && !pin_is_unresolved && id == last_pin_id);
    pin_is_unresolved = true;
    pin_unresolved++;
    return VFS_OK;
}

int vfs_recovery_pin_finish_handoff(u64 id)
{
    assert(!operation_locked && pin_live && !pin_is_unresolved && id == last_pin_id);
    if (attempt_mutations_during_finish) {
        vfs_node_t file = {.super = &super, .inode = 2, .type = VFS_TYPE_FILE};
        vfs_node_t dir = {.super = &super, .inode = 1,
                          .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
        vfs_node_t *created = NULL;
        u8 byte = 9;
        u8 readback = 0;
        attempt_mutations_during_finish = false;
        assert(vfs_super_operation_lock(&super));
        unsigned before = writes;
        assert(mgfs_write(&file, 0, 1, &byte) == 0);
        assert(mgfs_truncate(&file) == VFS_ERR_UNCERTAIN);
        assert(mgfs_append_lines(&file, "\n", 1, 1, 0, NULL) ==
               VFS_ERR_UNCERTAIN);
        assert(mgfs_create(&dir, "x", &created) == VFS_ERR_UNCERTAIN);
        assert(mgfs_mkdir(&dir, "x", &created) == VFS_ERR_UNCERTAIN);
        assert(mgfs_unlink(&dir, "x") == VFS_ERR_UNCERTAIN);
        assert(mgfs_rmdir(&dir, "x") == VFS_ERR_UNCERTAIN);
        assert(mgfs_rename_entry(&dir, "x", &dir, "y") ==
               VFS_ERR_UNCERTAIN);
        assert(writes == before && !created);
        assert(mgfs_read(&file, 0, 1, &readback) == 1);
        assert(readback == (fs.stage_transaction->target_id == 2 ? 0 : 0xA5));
        blocked_handoff_mutations++;
        vfs_super_operation_unlock(&super);
    }
    if (remove_device_during_finish) {
        remove_device_during_finish = false;
        super.state = VFS_SUPER_DEAD;
    }
    if (fail_pin_finish_busy || fail_mutex_reacquire) return VFS_ERR_BUSY;
    operation_locked = true; /* VFS holds the raw operation mutex. */
    if (fail_handoff_callback)
        fs.stage_transaction->handoff_pending = false;
    bool completed = pin_handoff(&super, pin_context);
    if (fail_handoff_callback) {
        assert(!completed && fs.stage_transaction);
        fs.stage_transaction->handoff_pending = true;
    }
    if (!completed) {
        pin_is_unresolved = true;
        pin_unresolved++;
        operation_locked = false;
        return VFS_ERR_UNCERTAIN;
    }
    assert(!fs.stage_transaction);
    pin_live = false;
    pin_finishes++;
    operation_locked = false;
    assert(super.reference_count);
    super.reference_count--;
    return VFS_OK;
}

static void put(u8 *data, u64 value) { mgfs_store_le64(data, value); }
static u8 *record_at(unsigned slot)
{
    assert(slot < 21);
    return media[2] + MGFS_BITMAP_HEADER_BYTES + slot * MGFS_RECORD_BYTES;
}

static void index_record(u64 id, u64 slot)
{
    u64 index = mgfs_hash_id(id) & 63;
    while (ids[index].id) index = (index + 1) & 63;
    ids[index] = (mgfs_record_ref_t){id, slot};
    record_bits[MGFS_BITMAP_HEADER_BYTES + slot / 8] |= (u8)(1U << (slot % 8));
}

static void seal(void)
{
    for (unsigned slot = 0; slot < 2; slot++)
        mgfs_recompute_checksum(record_at(slot), MGFS_RECORD_CHECKSUM_OFFSET,
                                MGFS_RECORD_BYTES);
    put(media[1], MGFS_METADATA_ALLOCATION_BITMAP);
    put(media[1] + 8, 0);
    mgfs_recompute_checksum(media[1], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    put(media[2], MGFS_METADATA_RECORD_TABLE);
    put(media[2] + 8, 0);
    mgfs_recompute_checksum(media[2], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    put(media[3], MGFS_METADATA_RECORD_BITMAP);
    put(media[3] + 8, 0);
    memcpy(media[3] + MGFS_BITMAP_HEADER_BYTES,
           record_bits + MGFS_BITMAP_HEADER_BYTES,
           MGFS_BLOCK_BYTES - MGFS_BITMAP_HEADER_BYTES);
    mgfs_recompute_checksum(media[3], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    mgfs_recompute_checksum(media[0], MGFS_SUPER_CHECKSUM_OFFSET,
                            MGFS_HEADER_BYTES);
}

static void seed(void)
{
    assert(!live_allocations && !live_bytes);
    memset(media, 0, sizeof(media));
    memset(record_bits, 0, sizeof(record_bits));
    memset(ids, 0, sizeof(ids));
    memset(&fs, 0, sizeof(fs));
    memset(&super, 0, sizeof(super));
    memset(&device, 0, sizeof(device));
    device.id = 7;
    device.sector_size = 512;
    device.sector_count = FIXTURE_BLOCKS * 8ULL;
    /* This focused fixture supplies a prevalidated mount. Full writable
     * admission and recovery read-only policy have separate production tests. */
    fs.ownership_confidence = MGFS_CONFIDENCE_PROVEN;
    fs.dev = &device;
    fs.device_id = device.id;
    fs.layout.record_count = 21;
    fs.layout.record_table_start = 2;
    fs.layout.record_table_blocks = 1;
    fs.layout.record_bitmap_start = 3;
    fs.layout.record_bitmap_blocks = 1;
    fs.layout.allocation_bitmap_start = 1;
    fs.layout.allocation_bitmap_blocks = 1;
    fs.layout.data_start = 4;
    fs.layout.data_blocks = FIXTURE_BLOCKS - 4;
    fs.record_bitmap = record_bits;
    fs.record_ids = ids;
    fs.record_id_capacity = 64;
    fs.next_record_id = 3;
    super.private_data = &fs;
    super.reference_count = 1;
    reads = writes = fail_read = fail_write = allocations = fail_allocation = 0;
    peak_bytes = 0;
    pin_begins = pin_finishes = pin_unresolved = 0;
    pin_live = pin_is_unresolved = operation_locked = false;
    table_writes = original_data_writes = 0;
    old_high_storage_writes = 0;
    second_table_writes = child_table_order = parent_table_order = 0;
    failed_write_persistence = 0;
    fail_bitmap_read_after_writes = 0;
    fail_parent_tail_read_after_writes = 0;
    fail_pin_begin = fail_pin_finish_busy = fail_mutex_wait = false;
    fail_mutex_reacquire = fail_handoff_callback = false;
    pin_handoff = NULL; pin_context = NULL;
    attempt_mutations_during_finish = remove_device_during_finish = false;
    blocked_handoff_mutations = 0;
    ordinary_mutation_test = false;
    tracked_node = NULL;
    retarget_parent_on_size_update = false;
    put(media[0] + 64, MGFS_STATE_CLEAN);
    put(media[0] + 96, fs.next_record_id);
    u8 *root = record_at(0);
    put(root, MGFS_RECORD_DIRECTORY);
    put(root + 8, (7ULL << MGFS_RECORD_OWNER_SHIFT) |
                  (3ULL << MGFS_RECORD_PERMISSIONS_SHIFT));
    put(root + 16, 1);
    put(root + 24, 1);
    index_record(1, 0);
    u8 *file = record_at(1);
    put(file, MGFS_RECORD_FILE);
    put(file + 8, 3ULL << MGFS_RECORD_PERMISSIONS_SHIFT);
    put(file + 16, 2);
    put(file + 24, 1);
    put(file + 32, MGFS_BLOCK_BYTES);
    put(file + 40, 1);
    put(file + 48, 1);
    mgfs_store_extent(file, 64, 0, 5, 1, MGFS_EXTENT_DATA);
    media[1][MGFS_BITMAP_HEADER_BYTES] |= 1U << 1;
    memset(media[5], 0xA5, MGFS_BLOCK_BYTES);
    index_record(2, 1);
    seal();
}

static void version_admission_tests(void)
{
    mgfs_layout_t layout;
    mgfs_fs_t mounted = {0};
    u8 header[MGFS_BLOCK_BYTES] = {0};
    seed();
    assert(mgfs_calculate_layout(FIXTURE_BLOCKS, &layout));
    memcpy(header, "MGFSv1\0\0", 8);
    put(header + 8, MGFS_FORMAT_MAJOR);
    put(header + 16, MGFS_FORMAT_MINOR);
    put(header + 24, MGFS_HEADER_BYTES);
    put(header + 32, MGFS_BLOCK_BYTES);
    put(header + 40, FIXTURE_BLOCKS);
    put(header + 64, MGFS_STATE_CLEAN);
    put(header + 88, 1);
    put(header + 96, 2);
    put(header + 104, layout.allocation_bitmap_start);
    put(header + 112, layout.allocation_bitmap_blocks);
    put(header + 120, layout.record_bitmap_start);
    put(header + 128, layout.record_bitmap_blocks);
    put(header + 136, layout.record_table_start);
    put(header + 144, layout.record_table_blocks);
    put(header + 152, layout.record_count);
    put(header + 160, layout.data_start);
    put(header + 168, layout.data_blocks);
    mgfs_recompute_checksum(header, MGFS_SUPER_CHECKSUM_OFFSET,
                            MGFS_HEADER_BYTES);
    assert(mgfs_validate_superblock(&device, header, &mounted));
    put(header + 16, 1);
    mgfs_recompute_checksum(header, MGFS_SUPER_CHECKSUM_OFFSET,
                            MGFS_HEADER_BYTES);
    assert(!mgfs_validate_superblock(&device, header, &mounted));
    assert(!writes && !pin_live && !live_allocations);
}

typedef struct {u64 length; unsigned calls, short_call, error_call;} source_state_t;
static u32 source(void *context, u64 offset, u8 *buffer, u32 requested)
{
    source_state_t *state = context;
    assert(offset + requested <= state->length);
    state->calls++;
    if (state->calls == state->error_call) return ~(u32)0;
    if (state->calls == state->short_call) return requested - 1;
    for (u32 i = 0; i < requested; i++) buffer[i] = (u8)((offset + i) % 251);
    return requested;
}

static void assert_original(void)
{
    assert(!table_writes && !original_data_writes);
    for (unsigned i = 0; i < MGFS_BLOCK_BYTES; i++) assert(media[5][i] == 0xA5);
    assert(mgfs_get_le64(record_at(1) + 24) == 1);
    assert(mgfs_get_le64(record_at(1) + 32) == MGFS_BLOCK_BYTES);
}

static void resolved(int expected, u64 length, unsigned short_call)
{
    source_state_t state = {length, 0, short_call, 0};
    u8 original_table[MGFS_BLOCK_BYTES], original_bitmap[MGFS_BLOCK_BYTES];
    memcpy(original_table, media[2], sizeof(original_table));
    memcpy(original_bitmap, media[1], sizeof(original_bitmap));
    int actual = mgfs_stage_and_cancel(&super, 2, 1, length, source, &state);
    assert(actual == expected);
    assert(!fs.stage_transaction && !fs.record_lifecycle_uncertain);
    assert(!pin_live && pin_begins == pin_finishes);
    assert(super.reference_count == 1 && !live_allocations);
    assert(!memcmp(original_table, media[2], sizeof(original_table)));
    assert(!memcmp(original_bitmap, media[1], sizeof(original_bitmap)));
    assert(mgfs_get_le64(media[0] + 64) == MGFS_STATE_CLEAN);
    assert_original();
}

static void discard_unresolved_fixture(void)
{
    assert(pin_live && pin_is_unresolved && (fs.stage_transaction || fs.ordinary_transaction));
    if (fs.ordinary_transaction) {
        kfree(fs.ordinary_transaction);
        fs.ordinary_transaction = NULL;
    } else
    if (pin_handoff == mgfs_create_finish_handoff)
        mgfs_create_free((mgfs_create_transaction_t *)fs.stage_transaction);
    else
        mgfs_stage_free(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = pin_is_unresolved = false;
    super.reference_count = 1;
}

/* Ordinary fixture routes use the real pinned callbacks and post-unlock
 * handoff; only registry/lock transport is controlled by this fixture. */
static u64 fixture_write(vfs_node_t *node, u64 offset, u64 size, const void *bytes)
{
    u64 count = 0, pin = 0;
    int result = mgfs_write_pinned(node, offset, size, bytes, &count, &pin);
    if (pin) {
        assert(operation_locked);
        operation_locked = false;
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        operation_locked = true;
    }
    if (result == VFS_ERR_UNCERTAIN) assert(!count && pin_live && pin_is_unresolved);
    return count;
}
static int fixture_truncate(vfs_node_t *node)
{
    u64 pin = 0;
    int result = mgfs_truncate_pinned(node, &pin);
    if (pin) {
        operation_locked = false;
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        operation_locked = true;
    }
    return result;
}

static void mark_old_block(u64 block)
{
    u64 bit = block - fs.layout.data_start;
    assert(block < FIXTURE_BLOCKS);
    media[1][MGFS_BITMAP_HEADER_BYTES + bit / 8] |=
        (u8)(1U << (bit % 8));
}

static void assert_published(u64 length)
{
    u8 record[MGFS_RECORD_BYTES];
    assert(table_writes == 1 && !original_data_writes);
    assert(!fs.stage_transaction && !fs.record_lifecycle_uncertain &&
           !fs.publication_uncertain);
    assert(!pin_live && pin_begins == pin_finishes && !live_allocations);
    assert(super.reference_count == 1);
    assert(mgfs_get_le64(media[0] + 64) == MGFS_STATE_CLEAN);
    assert(mgfs_read_record(&fs, 2, record));
    assert(mgfs_get_le64(record + 24) == 2);
    assert(mgfs_get_le64(record + 32) == length);
    assert(mgfs_validate_metadata_header(media[2], MGFS_METADATA_RECORD_TABLE, 0));
    for (unsigned i = 0; i < MGFS_BLOCK_BYTES; i++)
        assert(media[5][i] == 0xA5);
}

static void write_dir_entry(u8 *at, const char *name, usize name_length)
{
    usize bytes = (32 + name_length + 7) & ~(usize)7;
    assert(bytes == 40 || bytes == 48);
    memset(at, 0, bytes);
    put(at, 2);
    put(at + 8, name_length);
    put(at + 16, MGFS_DIRENT_IN_USE);
    memcpy(at + 32, name, name_length);
    mgfs_recompute_checksum(at, 24, bytes);
}

static void seed_parent_tail(void)
{
    seed();
    u8 *root = record_at(0);
    put(root + 32, 4080);
    put(root + 40, 1);
    put(root + 48, 1);
    mgfs_store_extent(root, 64, 0, 7, 1, MGFS_EXTENT_DIRECTORY_METADATA);
    for (unsigned i = 0; i < 102; i++) {
        char name[2] = {(char)('A' + i / 26), (char)('A' + i % 26)};
        write_dir_entry(media[7] + i * 40, name, 2);
    }
    mark_old_block(7);
    seal();
}

static void seed_fragmented_parent(void)
{
    seed();
    u8 *root = record_at(0);
    put(root + 32, 3 * MGFS_BLOCK_BYTES);
    put(root + 40, 3);
    put(root + 48, 2);
    put(root + 56, 13);
    mgfs_store_extent(root, 64, 0, 7, 1, MGFS_EXTENT_DIRECTORY_METADATA);
    mgfs_store_extent(root, 96, 1, 9, 1, MGFS_EXTENT_DIRECTORY_METADATA);
    put(media[13], MGFS_EXTENT_LIST_MAGIC);
    put(media[13] + 8, 1);
    put(media[13] + 24, 1);
    mgfs_store_extent(media[13], MGFS_EXTENT_LIST_HEADER_BYTES,
                      2, 11, 1, MGFS_EXTENT_DIRECTORY_METADATA);
    mgfs_recompute_checksum(media[13], 32, MGFS_BLOCK_BYTES);
    for (unsigned block = 0; block < 3; block++) {
        unsigned physical = block == 0 ? 7 : block == 1 ? 9 : 11;
        unsigned cursor = 0;
        for (unsigned i = 0; i < 100; i++) {
            unsigned ordinal = block * 100 + i;
            char name[2] = {(char)('A' + ordinal / 26),
                            (char)('A' + ordinal % 26)};
            write_dir_entry(media[physical] + cursor, name, 2);
            cursor += 40;
        }
        for (unsigned i = 0; i < 2; i++) {
            char name[9] = {'l','o','n','g','0','0','0','0',
                            (char)('A' + block * 2 + i)};
            write_dir_entry(media[physical] + cursor, name, 9);
            cursor += 48;
        }
        assert(cursor == MGFS_BLOCK_BYTES);
        mark_old_block(physical);
    }
    mark_old_block(13);
    seal();
}

static void fragment_fresh_space(void);

static void write_tombstone(u8 *at, usize name_length)
{
    usize bytes = (32 + name_length + 7) & ~(usize)7;
    assert(bytes == 64 || bytes == 288);
    memset(at, 0, bytes);
    put(at, 2);
    put(at + 8, name_length);
    put(at + 16, MGFS_DIRENT_TOMBSTONE);
    memset(at + 32, 't', name_length);
    mgfs_recompute_checksum(at, 24, bytes);
}

static void seed_large_parent(u64 blocks, bool sparse_lists)
{
    seed();
    assert(blocks >= 3 && blocks <= MGFS_CREATE_MAX_PARENT_BLOCKS);
    u8 *root = record_at(0);
    put(root + 32, blocks * MGFS_BLOCK_BYTES);
    put(root + 40, blocks);
    put(root + 48, 2);
    put(root + 56, 2000);
    mgfs_store_extent(root, 64, 0, 1100, 1, MGFS_EXTENT_DIRECTORY_METADATA);
    mgfs_store_extent(root, 96, 1, 1101, 1, MGFS_EXTENT_DIRECTORY_METADATA);
    u64 remaining = blocks - 2;
    u64 list_count = sparse_lists ? remaining :
        mgfs_ceil_div(remaining, MGFS_EXTENTS_PER_LIST_BLOCK);
    assert(2000 + list_count < FIXTURE_BLOCKS);
    for (u64 logical = 0; logical < blocks; logical++) {
        u8 *data = media[1100 + logical];
        for (unsigned i = 0; i < 14; i++)
            write_tombstone(data + i * 288, 255);
        write_tombstone(data + 14 * 288, 25);
        mark_old_block(1100 + logical);
    }
    for (u64 i = 0; i < list_count; i++) {
        u8 *list = media[2000 + i];
        u64 first = sparse_lists ? i : i * MGFS_EXTENTS_PER_LIST_BLOCK;
        u64 entries = sparse_lists ? 1 : remaining - first;
        if (entries > MGFS_EXTENTS_PER_LIST_BLOCK)
            entries = MGFS_EXTENTS_PER_LIST_BLOCK;
        put(list, MGFS_EXTENT_LIST_MAGIC);
        put(list + 8, 1);
        put(list + 16, i + 1 == list_count ? 0 : 2000 + i + 1);
        put(list + 24, entries);
        for (u64 j = 0; j < entries; j++)
            mgfs_store_extent(list, MGFS_EXTENT_LIST_HEADER_BYTES + j * 32,
                first + j + 2, 1100 + first + j + 2, 1,
                MGFS_EXTENT_DIRECTORY_METADATA);
        mgfs_recompute_checksum(list, 32, MGFS_BLOCK_BYTES);
        mark_old_block(2000 + i);
    }
    fragment_fresh_space();
}

static void seed_second_record_table(void)
{
    seed();
    fs.layout.record_table_blocks = 2;
    fs.layout.record_count = 42;
    fs.layout.record_bitmap_start = 4;
    fs.layout.data_start = 5;
    fs.layout.data_blocks = FIXTURE_BLOCKS - 5;
    for (u64 slot = 2; slot < 21; slot++) {
        u8 *record = record_at((unsigned)slot);
        put(record, MGFS_RECORD_FILE);
        put(record + 8, 3ULL << MGFS_RECORD_PERMISSIONS_SHIFT);
        put(record + 16, slot + 1);
        put(record + 24, 1);
        mgfs_recompute_checksum(record, MGFS_RECORD_CHECKSUM_OFFSET,
                                MGFS_RECORD_BYTES);
        index_record(slot + 1, slot);
    }
    fs.record_id_count = 21;
    fs.next_record_id = 22;
    put(media[0] + 96, fs.next_record_id);
    memset(media[3], 0, MGFS_BLOCK_BYTES);
    put(media[3], MGFS_METADATA_RECORD_TABLE);
    put(media[3] + 8, 1);
    mgfs_recompute_checksum(media[3], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    memset(media[4], 0, MGFS_BLOCK_BYTES);
    put(media[4], MGFS_METADATA_RECORD_BITMAP);
    memcpy(media[4] + MGFS_BITMAP_HEADER_BYTES,
           record_bits + MGFS_BITMAP_HEADER_BYTES,
           MGFS_BLOCK_BYTES - MGFS_BITMAP_HEADER_BYTES);
    mgfs_recompute_checksum(media[4], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    media[1][MGFS_BITMAP_HEADER_BYTES] = 1U; /* existing file at block 5 */
    mgfs_recompute_checksum(media[1], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    mgfs_recompute_checksum(media[2], MGFS_METADATA_CHECKSUM_OFFSET,
                            MGFS_BLOCK_BYTES);
    mgfs_recompute_checksum(media[0], MGFS_SUPER_CHECKSUM_OFFSET,
                            MGFS_HEADER_BYTES);
}

static void create_contents_tests(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        seed();
        vfs_node_t dir = {.super = &super, .inode = 1,
                          .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
        tracked_node = &dir;
        u64 length = mode == 0 ? 0 : mode == 1 ? 56 :
                     mode == 2 ? 57 : 4097;
        source_state_t input = {length, 0, 0, 0};
        u64 child_id = 0;
        u8 old_file[MGFS_RECORD_BYTES];
        u8 child[MGFS_RECORD_BYTES];
        u8 parent[MGFS_RECORD_BYTES];
        mgfs_directory_entry_t entry;
        memcpy(old_file, record_at(1), sizeof(old_file));
        assert(mgfs_create_contents_private(&dir, "created", length,
                   source, &input, &child_id) == VFS_OK);
        assert(child_id == 3 && !pin_live && !fs.stage_transaction);
        assert(!fs.record_lifecycle_uncertain && !fs.publication_uncertain);
        assert(pin_begins == 1 && pin_finishes == 1);
        assert(!live_allocations && super.reference_count == 1);
        assert(mgfs_get_le64(media[0] + 64) == MGFS_STATE_CLEAN);
        assert(!memcmp(old_file, record_at(1), sizeof(old_file)));
        assert(mgfs_read_record(&fs, 3, child));
        assert(mgfs_get_le64(child) == MGFS_RECORD_FILE);
        assert(mgfs_get_le64(child + 24) == 1);
        assert(mgfs_get_le64(child + 32) == length);
        assert(mgfs_record_owner(child) == 7);
        if (length) {
            vfs_node_t file = {.super = &super, .inode = child_id,
                               .type = VFS_TYPE_FILE};
            u8 bytes[80];
            u64 count = length < sizeof(bytes) ? length : sizeof(bytes);
            assert(mgfs_read(&file, 0, count, bytes) == count);
            for (u64 i = 0; i < count; i++) assert(bytes[i] == i % 251);
        }
        assert(mgfs_read_record(&fs, 1, parent));
        assert(mgfs_read_directory_entry(&fs, parent, 0, &entry));
        assert(entry.target_record_id == 3 &&
               strcmp(entry.name, "created") == 0);
        assert(dir.size == entry.total_bytes);
        unsigned prior_writes = writes;
        child_id = 55;
        assert(mgfs_create_contents_private(&dir, "created", 0,
                   NULL, NULL, &child_id) == VFS_ERR_ALREADY_EXISTS);
        assert(child_id == 0 && writes == prior_writes);
    }

    for (unsigned failing_write = 2; failing_write <= 9; failing_write++) {
        seed();
        vfs_node_t dir = {.super = &super, .inode = 1,
                          .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
        source_state_t input = {57, 0, 0, 0};
        u64 child_id = 0;
        fail_write = failing_write;
        failed_write_persistence = failing_write == 2 || failing_write >= 7 ? 2 : 0;
        int result = mgfs_create_contents_private(&dir, "new", 57,
                                                  source, &input, &child_id);
        assert(result == VFS_ERR_UNCERTAIN && child_id == 0);
        assert(fs.stage_transaction && fs.record_lifecycle_uncertain);
        assert(pin_live && pin_is_unresolved && super.reference_count == 2);
        assert(!memcmp(record_at(1), fs.stage_transaction->original_table +
                       MGFS_BITMAP_HEADER_BYTES + MGFS_RECORD_BYTES,
                       MGFS_RECORD_BYTES));
        mgfs_create_transaction_t *tx =
            (mgfs_create_transaction_t *)fs.stage_transaction;
        if (failing_write < 8) {
            assert(!tx->child_published && !tx->parent_published);
            assert(!table_writes);
        } else if (failing_write == 8) {
            assert(tx->create_phase == MGFS_CREATE_CHILD_PUBLICATION_UNCERTAIN);
            assert(!tx->child_published && fs.publication_uncertain);
            assert(table_writes == 1);
        } else {
            assert(tx->create_phase == MGFS_CREATE_PARENT_PUBLICATION_UNCERTAIN);
            assert(tx->child_published && !tx->parent_published);
            assert(fs.publication_uncertain && table_writes == 2);
            assert(tx->stage.ledger_count == 2);
            assert(tx->stage.pin_id == last_pin_id);
        }
        discard_unresolved_fixture();
    }

    seed();
    vfs_node_t definite_dir = {.super = &super, .inode = 1,
                               .type = VFS_TYPE_DIRECTORY,
                               .fs_data = (void *)1};
    u64 definite_child_id = 0;
    fail_write = 6; /* child table; no fixture bytes persist */
    assert(mgfs_create_contents_private(&definite_dir, "child-fail", 0,
               NULL, NULL, &definite_child_id) == VFS_ERR_UNCERTAIN);
    assert(!definite_child_id && fs.publication_uncertain &&
           ((mgfs_create_transaction_t *)fs.stage_transaction)->create_phase ==
               MGFS_CREATE_CHILD_PUBLICATION_UNCERTAIN);
    assert(mgfs_get_le64(record_at(0) + 32) == 0 && pin_is_unresolved);
    discard_unresolved_fixture();

    seed();
    vfs_node_t dir = {.super = &super, .inode = 1,
                      .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    source_state_t input = {57, 0, 1, 0};
    u64 child_id = 0;
    u8 original_table[MGFS_BLOCK_BYTES];
    memcpy(original_table, media[2], sizeof(original_table));
    assert(mgfs_create_contents_private(&dir, "new", 57,
               source, &input, &child_id) == VFS_ERR_IO);
    assert(!pin_live && !fs.stage_transaction && !fs.record_lifecycle_uncertain);
    assert(!memcmp(original_table, media[2], sizeof(original_table)));
    assert(!live_allocations);

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    source_state_t late_short = {4097, 0, 2, 0};
    u8 original_bitmap[MGFS_BLOCK_BYTES];
    memcpy(original_bitmap, media[1], sizeof(original_bitmap));
    assert(mgfs_create_contents_private(&dir, "short", late_short.length,
               source, &late_short, &child_id) == VFS_ERR_IO);
    assert(late_short.calls == 2 && !table_writes && !pin_live);
    assert(!fs.stage_transaction && !fs.record_lifecycle_uncertain);
    assert(!memcmp(original_bitmap, media[1], sizeof(original_bitmap)));
    assert(!live_allocations);

    seed_parent_tail();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    tracked_node = &dir;
    u8 old_tail[MGFS_BLOCK_BYTES];
    memcpy(old_tail, media[7], sizeof(old_tail));
    assert(mgfs_create_contents_private(&dir, "crossing", 0,
               NULL, NULL, &child_id) == VFS_OK);
    assert(child_id == 3 && dir.size == 4120);
    assert(!memcmp(old_tail, media[7], sizeof(old_tail)));
    u8 parent[MGFS_RECORD_BYTES];
    mgfs_directory_entry_t entry;
    assert(mgfs_read_record(&fs, 1, parent));
    assert(mgfs_read_directory_entry(&fs, parent, 4080, &entry));
    assert(strcmp(entry.name, "crossing") == 0 &&
           entry.target_record_id == 3);
    assert((media[1][MGFS_BITMAP_HEADER_BYTES] & (1U << 3)) == 0);

    seed_parent_tail();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    memcpy(old_tail, media[7], sizeof(old_tail));
    fail_parent_tail_read_after_writes = 1;
    child_id = 0;
    assert(mgfs_create_contents_private(&dir, "crossing", 0,
               NULL, NULL, &child_id) == VFS_ERR_IO);
    assert(!table_writes && !pin_live && !fs.stage_transaction);
    assert(!fs.record_lifecycle_uncertain && !live_allocations);
    assert(!memcmp(old_tail, media[7], sizeof(old_tail)));

    seed_parent_tail();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    memcpy(old_tail, media[7], sizeof(old_tail));
    fail_write = 10; /* first old-parent block reclamation */
    failed_write_persistence = 2;
    child_id = 0;
    assert(mgfs_create_contents_private(&dir, "crossing", 0,
               NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
    mgfs_create_transaction_t *tx =
        (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(tx && tx->child_published && tx->parent_published);
    assert(tx->create_phase == MGFS_CREATE_RECLAIMING_PARENT);
    assert(tx->old_tail_block == 7 && tx->old_list_released == 0);
    assert(!fs.publication_uncertain && pin_live && pin_is_unresolved);
    assert(!memcmp(old_tail, media[7], sizeof(old_tail)));
    discard_unresolved_fixture();

    seed_fragmented_parent();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    tracked_node = &dir;
    child_id = 0;
    assert(mgfs_create_contents_private(&dir, "fragmented", 0,
               NULL, NULL, &child_id) == VFS_OK);
    assert(child_id == 3 && dir.size == 3 * MGFS_BLOCK_BYTES + 48);
    assert(mgfs_read_record(&fs, 1, parent));
    assert(mgfs_get_le64(parent + 40) == 4);
    assert(mgfs_read_directory_entry(&fs, parent, 3 * MGFS_BLOCK_BYTES,
                                     &entry));
    assert(strcmp(entry.name, "fragmented") == 0);
    assert((media[1][MGFS_BITMAP_HEADER_BYTES + 1] & (1U << 1)) == 0);
    assert(!pin_live && !fs.stage_transaction && !live_allocations);

    for (unsigned failed = 6; failed <= 7; failed++) {
        seed_second_record_table();
        dir = (vfs_node_t){.super = &super, .inode = 1,
                           .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
        fail_write = failed;
        failed_write_persistence = 2;
        child_id = 0;
        assert(mgfs_create_contents_private(&dir, "two-tables", 0,
                   NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
        mgfs_create_transaction_t *pending =
            (mgfs_create_transaction_t *)fs.stage_transaction;
        assert(pending && pin_is_unresolved && fs.publication_uncertain);
        assert(pending->child_published == (failed == 7));
        assert(!pending->parent_published);
        assert(second_table_writes == 1);
        if (failed == 7) assert(table_writes == 1);
        discard_unresolved_fixture();
    }

    seed_fragmented_parent();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    fail_write = 10; /* old directory extent-list reclamation */
    failed_write_persistence = 2;
    child_id = 0;
    assert(mgfs_create_contents_private(&dir, "fragmented", 0,
               NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
    mgfs_create_transaction_t *pending =
        (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(pending && pending->parent_published);
    assert(pending->create_phase == MGFS_CREATE_RECLAIMING_PARENT);
    assert(pending->old_list_count == 1 && pending->old_list_released == 0);
    assert(pin_live && pin_is_unresolved);
    discard_unresolved_fixture();

    seed_second_record_table();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    child_id = 0;
    assert(mgfs_create_contents_private(&dir, "other-table", 0,
               NULL, NULL, &child_id) == VFS_OK);
    assert(child_id == 22 && second_table_writes == 1 && table_writes == 1);
    assert(child_table_order < parent_table_order);
    assert(mgfs_validate_metadata_header(media[2],
           MGFS_METADATA_RECORD_TABLE, 0));
    assert(mgfs_validate_metadata_header(media[3],
           MGFS_METADATA_RECORD_TABLE, 1));

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    fail_write = 7; /* parent table: fixture makes no media change */
    assert(mgfs_create_contents_private(&dir, "orphan", 0,
               NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
    tx = (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(tx && tx->child_published && !tx->parent_published);
    assert(tx->create_phase == MGFS_CREATE_PARENT_PUBLICATION_UNCERTAIN);
    assert(tx->stage.pin_id == last_pin_id && pin_is_unresolved);
    assert(mgfs_validate_metadata_header(tx->stage.proposed_table,
           MGFS_METADATA_RECORD_TABLE, 0));
    assert(mgfs_validate_metadata_header(tx->parent_proposed_table,
           MGFS_METADATA_RECORD_TABLE, 0));
    assert(!memcmp(tx->stage.proposed_table + MGFS_BITMAP_HEADER_BYTES +
                   2 * MGFS_RECORD_BYTES,
                   tx->parent_proposed_table + MGFS_BITMAP_HEADER_BYTES +
                   2 * MGFS_RECORD_BYTES, MGFS_RECORD_BYTES));
    assert(mgfs_get_le64(record_at(0) + 32) == 0);
    super.state = VFS_SUPER_DEAD;
    assert(super.reference_count == 2 && fs.stage_transaction);
    assert(pin_live && pin_is_unresolved);
    discard_unresolved_fixture();

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    fail_write = 8; /* clean-state restoration, after both publications */
    assert(mgfs_create_contents_private(&dir, "new", 0,
               NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
    tx = (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(tx && tx->parent_published &&
           tx->create_phase == MGFS_CREATE_RESTORING_STATE);
    assert(pin_live && pin_is_unresolved && !fs.publication_uncertain);
    discard_unresolved_fixture();

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    fail_pin_begin = true;
    assert(mgfs_create_contents_private(&dir, "new", 0,
               NULL, NULL, &child_id) == VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live && !fs.stage_transaction && !live_allocations);
    for (unsigned allocation = 1; allocation <= 3; allocation++) {
        seed();
        dir = (vfs_node_t){.super = &super, .inode = 1,
                           .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
        fail_allocation = allocation;
        assert(mgfs_create_contents_private(&dir, "new", 0,
                   NULL, NULL, &child_id) == VFS_ERR_NO_MEM);
        assert(!writes && !pin_live && !fs.stage_transaction &&
               !live_allocations);
    }

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    mark_old_block(4);
    mark_old_block(6);
    mark_old_block(8);
    mark_old_block(10);
    seal();
    source_state_t fragmented_child = {3 * MGFS_BLOCK_BYTES, 0, 0, 0};
    assert(mgfs_create_contents_private(&dir, "fragmented-child",
               fragmented_child.length, source, &fragmented_child,
               &child_id) == VFS_OK);
    u8 fragmented_record[MGFS_RECORD_BYTES];
    assert(mgfs_read_record(&fs, child_id, fragmented_record));
    assert(mgfs_get_le64(fragmented_record + 40) == 3);
    assert(mgfs_get_le64(fragmented_record + 56) != 0);
    assert(!pin_live && !fs.stage_transaction && !live_allocations);

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    source_state_t maximum = {MGFS_STAGE_MAX_BLOCKS * MGFS_BLOCK_BYTES,
                              0, 0, 0};
    assert(mgfs_create_contents_private(&dir, "maximum", maximum.length,
               source, &maximum, &child_id) == VFS_OK);
    assert(child_id == 3 && maximum.calls == MGFS_STAGE_MAX_BLOCKS);
    assert(!pin_live && !live_allocations);
    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    assert(mgfs_create_contents_private(&dir, "too-large",
               MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES + 1,
               source, &maximum, &child_id) == VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live && !live_allocations);

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    u8 *root = record_at(0);
    put(root + 32, (MGFS_CREATE_MAX_PARENT_BLOCKS + 1) * MGFS_BLOCK_BYTES);
    seal();
    assert(mgfs_create_contents_private(&dir, "over-limit", 0,
               NULL, NULL, &child_id) == VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live && !fs.stage_transaction && !live_allocations);

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    put(record_at(0) + 8, (7ULL << MGFS_RECORD_OWNER_SHIFT) |
                              ((u64)VFS_PERMISSION_OWNER_READ <<
                               MGFS_RECORD_PERMISSIONS_SHIFT));
    seal();
    assert(mgfs_create_contents_private(&dir, "denied", 0,
               NULL, NULL, &child_id) == VFS_ERR_ACCESS_DENIED);
    assert(!writes && !pin_live && !live_allocations);
    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    put(record_at(0) + 24, MGFS_U64_MAX);
    seal();
    assert(mgfs_create_contents_private(&dir, "overflow", 0,
               NULL, NULL, &child_id) == VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live && !live_allocations);
    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};

    seed_parent_tail();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    mgfs_store_extent(record_at(1), 64, 0, 7, 1, MGFS_EXTENT_DATA);
    seal();
    assert(mgfs_create_contents_private(&dir, "shared", 0,
               NULL, NULL, &child_id) == VFS_ERR_BAD_FORMAT);
    assert(fs.record_lifecycle_uncertain && !writes && !pin_live);
    seed_parent_tail();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    media[1][MGFS_BITMAP_HEADER_BYTES] &= (u8)~(1U << 3);
    seal();
    assert(mgfs_create_contents_private(&dir, "free-bit", 0,
               NULL, NULL, &child_id) == VFS_ERR_BAD_FORMAT);
    assert(fs.record_lifecycle_uncertain && !writes && !pin_live);

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    attempt_mutations_during_finish = true;
    assert(mgfs_create_contents_private(&dir, "gated", 0,
               NULL, NULL, &child_id) == VFS_OK);
    assert(blocked_handoff_mutations == 1 && !fs.stage_transaction);
    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    fail_handoff_callback = true;
    assert(mgfs_create_contents_private(&dir, "handoff", 0,
               NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_is_unresolved && fs.stage_transaction);
    assert(((mgfs_create_transaction_t *)fs.stage_transaction)->parent_published);
    discard_unresolved_fixture();

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    fail_pin_finish_busy = fail_mutex_wait = true;
    assert(mgfs_create_contents_private(&dir, "finish-busy", 0,
               NULL, NULL, &child_id) == VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_is_unresolved && fs.stage_transaction);
    assert(((mgfs_create_transaction_t *)fs.stage_transaction)->parent_published);
    discard_unresolved_fixture();

    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    remove_device_during_finish = true;
    assert(mgfs_create_contents_private(&dir, "removed", 0,
               NULL, NULL, &child_id) == VFS_OK);
    assert(super.state == VFS_SUPER_DEAD && !fs.stage_transaction &&
           !pin_live && super.reference_count == 1);

    /* The VFS hook returns facts from the confirmed child table image while
     * the transaction and operation lock still exist. */
    seed();
    dir = (vfs_node_t){.super = &super, .inode = 1,
                       .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    vfs_file_facts_t facts = {0};
    u64 pin = 0;
    source_state_t hooked_input = {57, 0, 0, 0};
    tracked_node = &dir;
    retarget_parent_on_size_update = true;
    assert(vfs_super_operation_lock(&super));
    vfs_node_t prepared_node = {0};
    assert(mgfs_create_contents_hook(&dir, &prepared_node, "hooked", 57, source,
                                     &hooked_input, &facts, &pin) == VFS_OK);
    assert(pin && pin_live && fs.stage_transaction);
    assert(dir.super == &retired_super);
    assert(facts.record_id == 3 && facts.generation == 1 &&
           facts.size == 57 && facts.owner_uid == 7 &&
           facts.permissions == VFS_DEFAULT_FILE_PERMISSIONS);
    vfs_super_operation_unlock(&super);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(!fs.stage_transaction && !pin_live && super.reference_count == 1);
}

static void publication_tests(void)
{
    source_state_t input = {0, 0, 0, 0};
    u8 old_table[MGFS_BLOCK_BYTES], old_record[MGFS_RECORD_BYTES];
    u8 readback[80];
    vfs_node_t node = {.super = &super, .inode = 2, .type = VFS_TYPE_FILE};

    for (unsigned mode = 0; mode < 4; mode++) {
        seed(); input.length = mode == 0 ? 0 : mode == 1 ? 56 :
                              mode == 2 ? 57 : 4097;
        memcpy(old_table, media[2], sizeof(old_table));
        memcpy(old_record, record_at(1), sizeof(old_record));
        assert(mgfs_stage_and_publish(&super, 2, 1, input.length,
                                      source, &input) == VFS_OK);
        assert_published(input.length);
        assert(!memcmp(media[2] + MGFS_BITMAP_HEADER_BYTES,
                       old_table + MGFS_BITMAP_HEADER_BYTES,
                       MGFS_RECORD_BYTES));
        assert((mgfs_get_le64(record_at(1) + 8) & ~MGFS_RECORD_INLINE_DATA) ==
               (mgfs_get_le64(old_record + 8) & ~MGFS_RECORD_INLINE_DATA));
        assert(mgfs_get_le64(record_at(1) + 16) == 2);
        assert((media[1][MGFS_BITMAP_HEADER_BYTES] & (1U << 1)) == 0);
        if (input.length) {
            u64 amount = input.length < sizeof(readback) ? input.length : sizeof(readback);
            assert(mgfs_read(&node, 0, amount, readback) == amount);
            for (u64 i = 0; i < amount; i++) assert(readback[i] == i % 251);
        }
    }

    seed(); input.length = 57;
    put(record_at(1) + 8, (42ULL << MGFS_RECORD_OWNER_SHIFT) |
                           (3ULL << MGFS_RECORD_PERMISSIONS_SHIFT));
    seal();
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) == VFS_OK);
    assert_published(57);
    assert(mgfs_record_owner(record_at(1)) == 42);
    assert(mgfs_record_permissions(record_at(1)) == 3);

    seed(); input.length = 57; input.calls = 0; input.short_call = 1;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) == VFS_ERR_IO);
    assert(!table_writes && !pin_live && !fs.stage_transaction &&
           !fs.record_lifecycle_uncertain);
    input.short_call = 0;

    for (unsigned failing_write = 2; failing_write <= 3; failing_write++) {
        seed(); input.length = 57; fail_write = failing_write;
        failed_write_persistence = 2;
        assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
               VFS_ERR_UNCERTAIN);
        assert(!table_writes && !fs.publication_uncertain);
        assert(fs.stage_transaction->phase ==
               (failing_write == 2 ? MGFS_STAGE_ALLOCATING :
                                     MGFS_STAGE_WRITING_DATA));
        assert(pin_live && pin_is_unresolved);
        assert_original();
        discard_unresolved_fixture();
    }

    for (unsigned persistence = 0; persistence < 3; persistence++) {
        seed(); input.length = 57; fail_write = 4;
        failed_write_persistence = persistence;
        assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
               VFS_ERR_UNCERTAIN);
        assert(table_writes == 1 && fs.publication_uncertain &&
               fs.record_lifecycle_uncertain);
        assert(fs.stage_transaction->phase == MGFS_STAGE_PUBLISHING);
        assert(!fs.stage_transaction->inventory);
        assert(fs.stage_transaction->ledger_count == 1);
        assert(fs.stage_transaction->old_data_blocks == 1);
        assert(fs.stage_transaction->old_extent_index == 0);
        assert((media[1][MGFS_BITMAP_HEADER_BYTES] & (1U << 1)) != 0);
        assert(pin_live && pin_is_unresolved && pin_finishes == 0);
        assert(!mgfs_read_record(&fs, 2, old_record));
        assert(mgfs_read(&node, 0, 1, readback) == 0);
        assert(!mgfs_read_directory_bytes(&fs, record_at(0), 0, 0, readback));
        unsigned previous_writes = writes;
        assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
               VFS_ERR_UNCERTAIN);
        assert(writes == previous_writes);
        discard_unresolved_fixture();
    }

    seed(); input.length = 57; fail_write = 5;
    failed_write_persistence = 2;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(!fs.publication_uncertain && fs.record_lifecycle_uncertain);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING_OLD);
    assert(fs.stage_transaction->old_extent_index == 0 &&
           fs.stage_transaction->old_extent_offset == 0);
    assert(mgfs_get_le64(record_at(1) + 24) == 2);
    assert(mgfs_read(&node, 0, 1, readback) == 1);
    assert(pin_live && pin_is_unresolved);
    discard_unresolved_fixture();

    seed(); fail_bitmap_read_after_writes = 2;
    assert(mgfs_stage_and_publish(&super, 2, 1, 0, NULL, NULL) ==
           VFS_ERR_UNCERTAIN);
    assert(table_writes == 1 && !fs.publication_uncertain);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING_OLD);
    assert(fs.stage_transaction->old_extent_offset == 0);
    discard_unresolved_fixture();

    seed(); input.length = 57;
    put(record_at(1) + 32, 2 * MGFS_BLOCK_BYTES);
    mgfs_store_extent(record_at(1), 64, 0, 5, 2, MGFS_EXTENT_DATA);
    mark_old_block(6); memset(media[6], 0xA5, MGFS_BLOCK_BYTES); seal();
    fail_write = 6;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->old_extent_offset == 1);
    assert(fs.stage_transaction->old_extent_index == 0);
    assert(mgfs_get_le64(record_at(1) + 24) == 2);
    assert(pin_live && pin_is_unresolved && !fs.publication_uncertain);
    discard_unresolved_fixture();

    seed(); input.length = 0;
    put(record_at(1) + 32, 3 * MGFS_BLOCK_BYTES);
    put(record_at(1) + 40, 3); put(record_at(1) + 48, 0);
    put(record_at(1) + 56, 11);
    memset(record_at(1) + 64, 0, 64);
    put(media[11], MGFS_EXTENT_LIST_MAGIC);
    put(media[11] + 8, 2); put(media[11] + 24, 3);
    for (u64 i = 0; i < 3; i++) {
        u64 block = 5 + 2 * i;
        mgfs_store_extent(media[11], MGFS_EXTENT_LIST_HEADER_BYTES + i * 32,
                          i, block, 1, MGFS_EXTENT_DATA);
        mark_old_block(block);
    }
    mark_old_block(11);
    mgfs_recompute_checksum(media[11], 32, MGFS_BLOCK_BYTES);
    seal(); fail_write = 6;
    assert(mgfs_stage_and_publish(&super, 2, 1, 0, NULL, NULL) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->old_extent_index == 3);
    assert(fs.stage_transaction->old_list_count == 1);
    assert(fs.stage_transaction->old_list_released == 0);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING_OLD);
    discard_unresolved_fixture();

    seed(); input.length = 57; fail_write = 6;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RESTORING_STATE);
    assert(fs.stage_transaction->old_extent_index == 1);
    assert(pin_live && pin_is_unresolved);
    discard_unresolved_fixture();

    /* Zero inline extents and one entry in each list block are valid. Thus
     * 128 mapped data blocks can really require 128 list blocks. */
    seed();
    put(record_at(1) + 32, MGFS_STAGE_MAX_BLOCKS * MGFS_BLOCK_BYTES);
    put(record_at(1) + 40, MGFS_STAGE_MAX_BLOCKS);
    put(record_at(1) + 48, 0);
    put(record_at(1) + 56, 200);
    memset(record_at(1) + 64, 0, 64);
    for (u64 i = 0; i < MGFS_STAGE_MAX_BLOCKS; i++) {
        u64 list = 200 + i;
        put(media[list], MGFS_EXTENT_LIST_MAGIC);
        put(media[list] + 8, 2);
        put(media[list] + 16,
            i + 1 == MGFS_STAGE_MAX_BLOCKS ? 0 : list + 1);
        put(media[list] + 24, 1);
        mgfs_store_extent(media[list], MGFS_EXTENT_LIST_HEADER_BYTES,
                          i, 5 + i, 1, MGFS_EXTENT_DATA);
        mgfs_recompute_checksum(media[list], 32, MGFS_BLOCK_BYTES);
        mark_old_block(5 + i);
        mark_old_block(list);
    }
    seal();
    assert(mgfs_stage_and_publish(&super, 2, 1, 0, NULL, NULL) == VFS_OK);
    assert_published(0);
    assert((media[1][MGFS_BITMAP_HEADER_BYTES + (200 - 4) / 8] &
            (1U << ((200 - 4) % 8))) == 0);

    seed(); input.length = 0;
    put(record_at(1) + 40, 1);
    mgfs_store_extent(record_at(1), 64, 0, 5,
                      MGFS_REPLACE_MAX_BLOCKS + 1, MGFS_EXTENT_DATA);
    for (u64 block = 5; block < 5 + MGFS_REPLACE_MAX_BLOCKS + 1; block++)
        mark_old_block(block);
    seal();
    assert(mgfs_stage_and_publish(&super, 2, 1, 0, NULL, NULL) ==
           VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live && !fs.stage_transaction && !live_allocations);

    seed(); input.length = 3 * MGFS_BLOCK_BYTES;
    mark_old_block(7); seal();
    assert(mgfs_stage_and_publish(&super, 2, 1, input.length,
                                  source, &input) == VFS_OK);
    assert_published(input.length);
    assert(mgfs_get_le64(record_at(1) + 40) == 3);
    assert(mgfs_get_le64(record_at(1) + 56) == 9);
    assert(mgfs_get_le64(media[9]) == MGFS_EXTENT_LIST_MAGIC);

    seed(); input.length = 57;
    fs.record_lifecycle_uncertain = true;
    assert(mgfs_read_record(&fs, 2, old_record));
    assert(mgfs_read(&node, 0, 1, readback) == 1);
    assert(mgfs_read_directory_bytes(&fs, record_at(0), 0, 0, readback));

    seed(); input.length = 57; attempt_mutations_during_finish = true;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) == VFS_OK);
    assert(blocked_handoff_mutations == 1);
    assert_published(57);
    assert(vfs_super_operation_lock(&super));
    assert(!mgfs_mutation_unavailable(&fs));
    vfs_super_operation_unlock(&super);

    seed(); input.length = 57; attempt_mutations_during_finish = true;
    fail_pin_finish_busy = fail_mutex_wait = true;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(blocked_handoff_mutations == 1 && fs.stage_transaction);
    assert(fs.stage_transaction->handoff_pending &&
           fs.stage_transaction->phase == MGFS_STAGE_RESTORING_STATE);
    assert(pin_live && pin_is_unresolved && pin_finishes == 0);
    assert(mgfs_get_le64(record_at(1) + 24) == 2);
    assert(vfs_super_operation_lock(&super));
    assert(mgfs_mutation_unavailable(&fs));
    vfs_super_operation_unlock(&super);
    discard_unresolved_fixture();

    seed(); input.length = 57; fail_mutex_reacquire = true;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_finishes == 0 && pin_is_unresolved);
    assert(fs.stage_transaction && fs.stage_transaction->handoff_pending);
    assert(super.reference_count == 2 && mgfs_mutation_unavailable(&fs));
    discard_unresolved_fixture();

    seed(); input.length = 57; fail_handoff_callback = true;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_is_unresolved && pin_unresolved == 1 &&
           fs.stage_transaction && fs.stage_transaction->handoff_pending);
    assert(super.reference_count == 2 && mgfs_mutation_unavailable(&fs));
    discard_unresolved_fixture();

    seed(); input.length = 57; remove_device_during_finish = true;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) == VFS_OK);
    assert(super.state == VFS_SUPER_DEAD && !fs.stage_transaction &&
           !pin_live && super.reference_count == 1);

    seed(); input.length = 57; remove_device_during_finish = true;
    fail_pin_finish_busy = fail_mutex_wait = true;
    assert(mgfs_stage_and_publish(&super, 2, 1, 57, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(super.state == VFS_SUPER_DEAD && fs.stage_transaction);
    assert(pin_live && pin_is_unresolved && super.reference_count == 2);
    discard_unresolved_fixture();
}

static void file_facts_tests(void)
{
    vfs_node_t node = {.inode = 2, .type = VFS_TYPE_FILE, .super = &super};
    vfs_file_facts_t facts = {0};

    seed();
    assert(mgfs_file_facts(&node, &facts) == VFS_OK);
    assert(facts.record_id == 2 && facts.generation == 1 &&
           facts.size == MGFS_BLOCK_BYTES && facts.owner_uid == 0 &&
           facts.permissions == 3 && !writes);
    fs.stage_transaction = (struct mgfs_stage_transaction *)(uintptr_t)1;
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_BUSY);
    fs.stage_transaction = NULL;
    put(record_at(1) + 8, mgfs_security_flags(41, 2, false,
                                               VFS_CHILD_MUTATION_OPEN));
    seal();
    assert(mgfs_file_facts(&node, &facts) == VFS_OK);
    assert(facts.owner_uid == 41 && facts.permissions == 2);
    node.inode = 99;
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_NOT_FOUND);
    node.inode = 2;
    fail_read = reads + 1;
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_IO);
    assert(!fs.record_lifecycle_uncertain && !writes);

    seed();
    put(record_at(1) + 16, 3);
    seal();
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_BAD_FORMAT);
    assert(fs.record_lifecycle_uncertain && !writes);
    seed();
    put(record_at(1), MGFS_RECORD_DIRECTORY);
    seal();
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_BAD_FORMAT);
    assert(fs.record_lifecycle_uncertain && !writes);
    seed();
    assert(mgfs_remove_record_id(&fs, 2));
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_NOT_FOUND);
    assert(!fs.record_lifecycle_uncertain && !writes);

    seed();
    ordinary_mutation_test = operation_locked = true;
    tracked_node = &node;
    assert(fixture_write(&node, 0, 1, "X") == 1);
    assert(mgfs_file_facts(&node, &facts) == VFS_OK &&
           facts.generation == 2 && facts.size == MGFS_BLOCK_BYTES &&
           node.size == MGFS_BLOCK_BYTES);
    assert(fixture_truncate(&node) == VFS_OK);
    assert(mgfs_file_facts(&node, &facts) == VFS_OK &&
           facts.generation == 3 && facts.size == 0 && node.size == 0);
    operation_locked = ordinary_mutation_test = false;
    assert(!live_allocations && !fs.record_lifecycle_uncertain);

    seed();
    ordinary_mutation_test = operation_locked = true;
    fail_write = 2; /* The state write succeeds; the data write is ambiguous. */
    assert(fixture_write(&node, 0, 1, "X") == 0);
    assert(fs.record_lifecycle_uncertain);
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_UNCERTAIN);
    assert(mgfs_get_le64(record_at(1) + 24) == 1);
    operation_locked = ordinary_mutation_test = false;
    assert(pin_live && pin_is_unresolved && fs.ordinary_transaction);
    discard_unresolved_fixture();
    assert(!live_allocations);
}

static void generation_overflow_tests(void)
{
    vfs_node_t node = {.inode = 2, .type = VFS_TYPE_FILE, .super = &super};
    vfs_file_facts_t facts;
    u8 *snapshot = malloc(sizeof(media));
    assert(snapshot);

    for (unsigned inline_file = 0; inline_file < 2; inline_file++) {
        seed();
        tracked_node = &node;
        node.size = MGFS_BLOCK_BYTES;
        if (inline_file) {
            u8 *record = record_at(1);
            put(record + 8, mgfs_get_le64(record + 8) |
                            MGFS_RECORD_INLINE_DATA);
            put(record + 32, 3);
            put(record + 40, 0);
            put(record + 48, 0);
            memset(record + 64, 0, 64);
            memcpy(record + 128, "abc", 3);
            media[1][MGFS_BITMAP_HEADER_BYTES] &= (u8)~(1U << 1);
            node.size = 3;
        }
        put(record_at(1) + 24, MGFS_U64_MAX - 1);
        seal();
        ordinary_mutation_test = operation_locked = true;

        assert(fixture_write(&node, 0, 1, "X") == 1);
        assert(mgfs_file_facts(&node, &facts) == VFS_OK &&
               facts.generation == MGFS_U64_MAX);
        memcpy(snapshot, media, sizeof(media));
        unsigned before_writes = writes;
        u64 before_size = node.size;

        assert(fixture_write(&node, 0, 1, "Y") == 0);
        assert(writes == before_writes &&
               memcmp(media, snapshot, sizeof(media)) == 0);
        assert(fixture_truncate(&node) == VFS_ERR_NO_SPACE);
        assert(writes == before_writes &&
               memcmp(media, snapshot, sizeof(media)) == 0);
        assert(node.size == before_size && !fs.record_lifecycle_uncertain &&
               !fs.stage_transaction && !live_allocations);
        assert(mgfs_file_facts(&node, &facts) == VFS_OK &&
               facts.generation == MGFS_U64_MAX && facts.size == before_size);
        operation_locked = ordinary_mutation_test = false;
    }
    free(snapshot);
}

static void replacement_hook_tests(void)
{
    vfs_node_t node = {.inode = 2, .type = VFS_TYPE_FILE,
                       .super = &super, .size = MGFS_BLOCK_BYTES};
    source_state_t input = {2 * MGFS_BLOCK_BYTES, 0, 0, 0};
    u64 pin = 0;

    seed(); tracked_node = &node;
    assert(vfs_super_operation_lock(&super));
    assert(mgfs_replace_locked(&super, node.inode, 1, input.length, source,
                               &input, &pin) == VFS_OK);
    assert(pin && pin_live && node.size == input.length);
    assert(mgfs_get_le64(record_at(1) + 24) == 2);
    vfs_super_operation_unlock(&super);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(!pin_live && !fs.stage_transaction && !live_allocations);

    pin = 0;
    assert(vfs_super_operation_lock(&super));
    assert(mgfs_replace_locked(&super, node.inode, 2, 0, NULL, NULL,
                               &pin) == VFS_OK);
    assert(pin && node.size == 0);
    vfs_super_operation_unlock(&super);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(mgfs_get_le64(record_at(1) + 24) == 3 &&
           !fs.stage_transaction && !live_allocations);

    seed(); tracked_node = &node; node.size = MGFS_BLOCK_BYTES;
    input = (source_state_t){57, 0, 1, 0};
    pin = 0;
    assert(vfs_super_operation_lock(&super));
    assert(mgfs_replace_locked(&super, node.inode, 1, 57, source,
                               &input, &pin) ==
           VFS_ERR_IO);
    assert(pin && !table_writes && node.size == MGFS_BLOCK_BYTES);
    vfs_super_operation_unlock(&super);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(!fs.stage_transaction && !live_allocations);

    seed(); tracked_node = &node; node.size = MGFS_BLOCK_BYTES;
    input = (source_state_t){57, 0, 0, 0};
    fail_write = 6; /* Publication succeeds; old-block cleanup fails. */
    pin = 0;
    assert(vfs_super_operation_lock(&super));
    assert(mgfs_replace_locked(&super, node.inode, 1, 57, source,
                               &input, &pin) ==
           VFS_ERR_UNCERTAIN);
    assert(!pin && node.size == 57 && fs.stage_transaction);
    assert(pin_live && pin_is_unresolved &&
           mgfs_get_le64(record_at(1) + 24) == 2);
    vfs_super_operation_unlock(&super);
    discard_unresolved_fixture();
}

static void fragment_fresh_space(void)
{
    /* The 512 even-numbered data blocks remain free. Odd blocks force one
     * extent per staged data block, with room afterward for five list blocks. */
    for (u64 block = 7; block <= 1027; block += 2) mark_old_block(block);
    seal();
}

static void large_create_tests(void)
{
    vfs_node_t dir = {.super = &super, .inode = 1,
                      .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    u64 child_id = 0;
    source_state_t input = {MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES,
                            0, 0, 0};
    seed_large_parent(MGFS_CREATE_MAX_PARENT_BLOCKS, false);
    dir.super = &super;
    assert(mgfs_create_contents_private(&dir, "large", input.length,
               source, &input, &child_id) == VFS_OK);
    assert(child_id == 3 && input.calls == MGFS_REPLACE_MAX_BLOCKS);
    assert(!pin_live && !live_bytes && !old_high_storage_writes);
    u8 parent[MGFS_RECORD_BYTES], child[MGFS_RECORD_BYTES];
    mgfs_directory_entry_t entry;
    assert(mgfs_read_record(&fs, 1, parent));
    assert(mgfs_read_record(&fs, child_id, child));
    assert(mgfs_get_le64(parent + 40) == 513);
    assert(mgfs_get_le64(child + 40) == MGFS_REPLACE_MAX_BLOCKS);
    assert(mgfs_get_le64(parent + 56) && mgfs_get_le64(child + 56));
    assert(mgfs_read_directory_entry(&fs, parent,
           MGFS_CREATE_MAX_PARENT_BLOCKS * MGFS_BLOCK_BYTES, &entry));
    assert(entry.target_record_id == child_id &&
           strcmp(entry.name, "large") == 0);
    usize peak = peak_bytes;
    usize retained = sizeof(mgfs_create_transaction_t) +
        MGFS_CREATE_MAX_LEDGER * sizeof(u64) +
        MGFS_REPLACE_MAX_BLOCKS * sizeof(mgfs_extent_desc_t) +
        MGFS_CREATE_MAX_PARENT_EXTENTS *
            (sizeof(u64) + sizeof(mgfs_extent_desc_t)) +
        MGFS_CREATE_MAX_PROPOSED_EXTENTS * sizeof(mgfs_extent_desc_t);
    assert(peak >= retained);
    printf("MGFS create requested heap peak=%zu retained=%zu tx=%zu bytes; 32 retained=%zu bytes\n",
           (size_t)peak, (size_t)retained,
           sizeof(mgfs_create_transaction_t), (size_t)(32 * retained));

    seed_large_parent(130, true);
    dir.super = &super; child_id = 0;
    source_state_t partial = {MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES - 17,
                              0, 0, 0};
    assert(mgfs_create_contents_private(&dir, "sparse", partial.length,
               source, &partial, &child_id) == VFS_OK);
    assert(child_id == 3 && !old_high_storage_writes && !live_bytes);
    assert(mgfs_read_record(&fs, child_id, child));
    assert(mgfs_get_le64(child + 32) == partial.length);
    vfs_node_t file = {.super = &super, .inode = child_id,
                       .type = VFS_TYPE_FILE};
    u8 final[MGFS_BLOCK_BYTES];
    assert(mgfs_read(&file, 511 * MGFS_BLOCK_BYTES,
                     MGFS_BLOCK_BYTES - 17, final) == MGFS_BLOCK_BYTES - 17);
    assert(final[MGFS_BLOCK_BYTES - 18] ==
           (u8)((partial.length - 1) % 251));

    for (unsigned allocation = 2; allocation <= 3; allocation++) {
        seed_large_parent(130, true);
        dir.super = &super; child_id = 0;
        fail_allocation = allocation; /* parent and shared stage inventories */
        assert(mgfs_create_contents_private(&dir, "alloc", input.length,
                   source, &input, &child_id) == VFS_ERR_NO_MEM);
        assert(!writes && !pin_live && !live_bytes && !fs.stage_transaction);
    }

    seed_large_parent(130, true);
    dir.super = &super; child_id = 0;
    input.calls = 0; input.short_call = 512;
    assert(mgfs_create_contents_private(&dir, "short", input.length,
               source, &input, &child_id) == VFS_ERR_IO);
    assert(!table_writes && !pin_live && !live_bytes);
    input.short_call = 0;

    /* Failure of a linked child-list write leaves all fresh candidates and
     * the pin retained, before either record publication. */
    seed_large_parent(130, true);
    dir.super = &super; child_id = 0;
    fail_write = 1 + 2 * MGFS_REPLACE_MAX_BLOCKS +
        MGFS_REPLACE_MAX_LISTS + 1;
    failed_write_persistence = 2;
    assert(mgfs_create_contents_private(&dir, "list", input.length,
               source, &input, &child_id) == VFS_ERR_UNCERTAIN);
    mgfs_create_transaction_t *tx =
        (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(tx && tx->create_phase == MGFS_CREATE_STAGING_CHILD);
    assert(tx->stage.new_list_count == MGFS_REPLACE_MAX_LISTS);
    assert(!tx->child_published && !table_writes && pin_is_unresolved);
    discard_unresolved_fixture();

    seed_large_parent(130, true);
    dir.super = &super; child_id = 0;
    fail_write = 1 + 2 * MGFS_REPLACE_MAX_BLOCKS +
        2 * MGFS_REPLACE_MAX_LISTS + 2 +
        mgfs_ceil_div(129, MGFS_EXTENTS_PER_LIST_BLOCK) + 1;
    failed_write_persistence = 2;
    assert(mgfs_create_contents_private(&dir, "parent-list", input.length,
               source, &input, &child_id) == VFS_ERR_UNCERTAIN);
    tx = (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(tx && tx->create_phase == MGFS_CREATE_STAGING_PARENT);
    assert(tx->parent_new_list_count && !tx->child_published &&
           !table_writes && pin_is_unresolved);
    discard_unresolved_fixture();

    const unsigned child_table_attempt =
        1 + 2 * MGFS_REPLACE_MAX_BLOCKS + 2 * MGFS_REPLACE_MAX_LISTS +
        2 + 2 * mgfs_ceil_div(129, MGFS_EXTENTS_PER_LIST_BLOCK) + 2 + 1;
    for (unsigned failure = child_table_attempt;
         failure <= child_table_attempt + 1; failure++) {
        seed_large_parent(130, true);
        dir.super = &super; child_id = 0;
        fail_write = failure; failed_write_persistence = 2;
        assert(mgfs_create_contents_private(&dir, "publish", input.length,
                   source, &input, &child_id) == VFS_ERR_UNCERTAIN);
        tx = (mgfs_create_transaction_t *)fs.stage_transaction;
        assert(tx && tx->stage.inventory && tx->parent_inventory);
        assert(tx->stage.ledger_count == MGFS_REPLACE_MAX_BLOCKS +
               MGFS_REPLACE_MAX_LISTS + 1 + 2);
        assert(tx->child_published == (failure == child_table_attempt + 1));
        assert(!tx->parent_published && fs.publication_uncertain &&
               pin_is_unresolved && child_id == 0);
        assert(!old_high_storage_writes);
        discard_unresolved_fixture();
    }

    seed_large_parent(130, true);
    dir.super = &super; child_id = 0;
    fail_write = child_table_attempt + 2 + 11;
    failed_write_persistence = 2;
    assert(mgfs_create_contents_private(&dir, "cleanup", input.length,
               source, &input, &child_id) == VFS_ERR_UNCERTAIN);
    tx = (mgfs_create_transaction_t *)fs.stage_transaction;
    assert(tx && tx->child_published && tx->parent_published);
    assert(tx->create_phase == MGFS_CREATE_RECLAIMING_PARENT);
    assert(tx->old_list_released == 11 && tx->old_list_count == 128);
    assert(pin_is_unresolved && !fs.publication_uncertain &&
           !old_high_storage_writes && child_id == 0);
    discard_unresolved_fixture();

    seed_large_parent(130, true);
    dir.super = &super; child_id = 0;
    for (u64 block = fs.layout.data_start;
         block < fs.layout.data_start + fs.layout.data_blocks; block++)
        mark_old_block(block);
    seal();
    assert(mgfs_create_contents_private(&dir, "full", input.length,
               source, &input, &child_id) == VFS_ERR_NO_SPACE);
    assert(!table_writes && !pin_live && !fs.stage_transaction &&
           !fs.record_lifecycle_uncertain && !live_bytes);
}

static void sparse_old_chain(void)
{
    u8 *old = record_at(1);
    put(old + 32, MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES);
    put(old + 40, MGFS_REPLACE_MAX_BLOCKS);
    put(old + 48, 0);
    put(old + 56, 2000);
    memset(old + 64, 0, 64);
    for (u64 i = 0; i < MGFS_REPLACE_MAX_BLOCKS; i++) {
        u64 list = 2000 + i;
        put(media[list], MGFS_EXTENT_LIST_MAGIC);
        put(media[list] + 8, 2);
        put(media[list] + 16,
            i + 1 == MGFS_REPLACE_MAX_BLOCKS ? 0 : list + 1);
        put(media[list] + 24, 1);
        mgfs_store_extent(media[list], MGFS_EXTENT_LIST_HEADER_BYTES,
                          i, 1100 + i, 1, MGFS_EXTENT_DATA);
        mgfs_recompute_checksum(media[list], 32, MGFS_BLOCK_BYTES);
        mark_old_block(1100 + i);
        mark_old_block(list);
    }
    fragment_fresh_space();
}

static void verify_large_list_chain(void)
{
    const u8 *record = record_at(1);
    u64 list = mgfs_get_le64(record + 56);
    u64 logical = 2;
    unsigned lists = 0;
    assert(mgfs_get_le64(record + 40) == MGFS_REPLACE_MAX_BLOCKS);
    while (list) {
        const u8 *block = media[list];
        u64 entries = mgfs_get_le64(block + 24);
        assert(list < FIXTURE_BLOCKS && ++lists <= MGFS_REPLACE_MAX_LISTS);
        assert(mgfs_get_le64(block) == MGFS_EXTENT_LIST_MAGIC);
        assert(mgfs_get_le64(block + 8) == 2);
        assert(entries && entries <= MGFS_EXTENTS_PER_LIST_BLOCK);
        assert(mgfs_checksum_block(block, 32, MGFS_BLOCK_BYTES));
        for (u64 i = 0; i < entries; i++) {
            const u8 *extent = block + MGFS_EXTENT_LIST_HEADER_BYTES + i * 32;
            assert(mgfs_get_le64(extent) == logical++);
            assert(mgfs_get_le64(extent + 16) == 1);
            assert(mgfs_get_le64(extent + 24) == MGFS_EXTENT_DATA);
        }
        list = mgfs_get_le64(block + 16);
    }
    assert(lists == MGFS_REPLACE_MAX_LISTS && logical == MGFS_REPLACE_MAX_BLOCKS);
    vfs_node_t file = {.super = &super, .inode = 2, .type = VFS_TYPE_FILE};
    u8 sample[MGFS_BLOCK_BYTES];
    const u64 points[] = {0, 1, 127, 128, 254, 255, 380, 381, 511};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        u64 offset = points[i] * MGFS_BLOCK_BYTES;
        assert(mgfs_read(&file, offset, sizeof(sample), sample) == sizeof(sample));
        assert(sample[0] == (u8)(offset % 251));
        assert(sample[MGFS_BLOCK_BYTES - 1] ==
               (u8)((offset + MGFS_BLOCK_BYTES - 1) % 251));
    }
}

static void large_replacement_tests(void)
{
    const u64 length = MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES;
    source_state_t input = {length, 0, 0, 0};
    usize contiguous_peak, fragmented_peak, sparse_peak;

    assert(mgfs_node_ops.replacement_max_bytes ==
           MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES);
    assert(mgfs_node_ops.create_contents_max_bytes ==
           MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES);

    seed();
    mark_old_block(4); seal();
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) == VFS_OK);
    assert(mgfs_get_le64(record_at(1) + 40) == 1);
    assert(mgfs_get_le64(record_at(1) + 24) == 2);
    assert(table_writes == 1);
    assert(input.calls == MGFS_REPLACE_MAX_BLOCKS && !live_bytes);
    contiguous_peak = peak_bytes;

    seed(); fragment_fresh_space(); input.calls = 0;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) == VFS_OK);
    verify_large_list_chain();
    assert(table_writes == 1);
    assert(input.calls == MGFS_REPLACE_MAX_BLOCKS && !live_bytes);
    fragmented_peak = peak_bytes;

    seed(); fragment_fresh_space(); input.calls = 0;
    u8 original_bitmap[MGFS_BLOCK_BYTES];
    memcpy(original_bitmap, media[1], sizeof(original_bitmap));
    assert(mgfs_stage_and_cancel(&super, 2, 1, length, source, &input) == VFS_OK);
    assert(!memcmp(original_bitmap, media[1], sizeof(original_bitmap)));
    assert_original();
    assert(!pin_live && !live_bytes);

    seed(); sparse_old_chain(); input.calls = 0;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) == VFS_OK);
    verify_large_list_chain();
    for (u64 i = 0; i < MGFS_REPLACE_MAX_BLOCKS; i++) {
        u64 data_bit = 1100 + i - fs.layout.data_start;
        u64 list_bit = 2000 + i - fs.layout.data_start;
        assert(!(media[1][MGFS_BITMAP_HEADER_BYTES + data_bit / 8] &
                 (1U << (data_bit % 8))));
        assert(!(media[1][MGFS_BITMAP_HEADER_BYTES + list_bit / 8] &
                 (1U << (list_bit % 8))));
    }
    assert(!old_high_storage_writes);
    assert(!live_bytes && !pin_live);
    sparse_peak = peak_bytes;
    const usize retained_budget = sizeof(mgfs_stage_transaction_t) +
        (MGFS_REPLACE_MAX_BLOCKS + MGFS_REPLACE_MAX_LISTS) * sizeof(u64) +
        MGFS_REPLACE_MAX_BLOCKS * sizeof(mgfs_extent_desc_t) +
        MGFS_REPLACE_MAX_BLOCKS *
            (sizeof(mgfs_extent_desc_t) + sizeof(u64));
    const usize preflight_budget = retained_budget +
        sizeof(mgfs_ownership_scan_t) +
        (2 * MGFS_REPLACE_MAX_BLOCKS + 1) * sizeof(mgfs_owned_range_t);
    assert(sparse_peak == preflight_budget);
    printf("MGFS replacement requested heap peak: contiguous=%zu fragmented=%zu sparse-old=%zu bytes; retained=%zu tx=%zu create=%zu bytes\n",
           (size_t)contiguous_peak, (size_t)fragmented_peak,
           (size_t)sparse_peak, (size_t)retained_budget,
           sizeof(mgfs_stage_transaction_t),
           sizeof(mgfs_create_transaction_t));

    seed(); fragment_fresh_space(); input.calls = 0; input.short_call = 256;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) == VFS_ERR_IO);
    assert(!table_writes && !pin_live && !live_bytes && !fs.stage_transaction);
    assert_original(); input.short_call = 0;

    for (unsigned failing_allocation = 2; failing_allocation <= 4;
         failing_allocation++) {
        seed(); fragment_fresh_space(); fail_allocation = failing_allocation;
        assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) ==
               VFS_ERR_NO_MEM);
        assert(!writes && !pin_live && !live_bytes && !fs.stage_transaction);
    }

    /* State write, 512 bitmap/data pairs, then five list bitmap/data pairs. */
    const unsigned table_attempt = 1 + 2 * 512 + 2 * MGFS_REPLACE_MAX_LISTS + 1;
    seed(); fragment_fresh_space(); input.calls = 0;
    fail_write = 1 + 2 * 512 + 1; failed_write_persistence = 2;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->phase == MGFS_STAGE_ALLOCATING);
    assert(fs.stage_transaction->ledger_count == 513 && pin_is_unresolved);
    assert(!table_writes && !original_data_writes);
    discard_unresolved_fixture();

    seed(); fragment_fresh_space(); input.calls = 0;
    fail_write = table_attempt - 3; failed_write_persistence = 2;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->phase == MGFS_STAGE_WRITING_LIST);
    assert(fs.stage_transaction->new_list_count == MGFS_REPLACE_MAX_LISTS);
    assert(fs.stage_transaction->ledger_count == 512 + MGFS_REPLACE_MAX_LISTS);
    assert(!table_writes && pin_is_unresolved);
    discard_unresolved_fixture();

    seed(); fragment_fresh_space(); input.calls = 0;
    fail_write = table_attempt; failed_write_persistence = 2;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->phase == MGFS_STAGE_PUBLISHING);
    assert(table_writes == 1);
    assert(fs.stage_transaction->inventory && fs.stage_transaction->pin_id == last_pin_id);
    assert(fs.stage_transaction->new_list_count == MGFS_REPLACE_MAX_LISTS);
    assert(fs.publication_uncertain && pin_live && pin_is_unresolved);
    super.state = VFS_SUPER_DEAD;
    assert(fs.stage_transaction && super.reference_count == 2);
    super.reference_count--; /* forced detach releases the fixture mount ref */
    assert(super.reference_count == 1 && fs.stage_transaction->inventory);
    discard_unresolved_fixture();

    seed(); sparse_old_chain(); input.calls = 0;
    fail_write = table_attempt + 257; failed_write_persistence = 2;
    assert(mgfs_stage_and_publish(&super, 2, 1, length, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING_OLD);
    assert(fs.stage_transaction->old_extent_index == 256);
    assert(fs.stage_transaction->old_list_released == 0);
    assert(fs.stage_transaction->old_list_count == MGFS_REPLACE_MAX_BLOCKS);
    assert(fs.stage_transaction->ledger_count == 512 + MGFS_REPLACE_MAX_LISTS);
    assert(mgfs_get_le64(record_at(1) + 24) == 2);
    assert(pin_live && pin_is_unresolved && fs.record_lifecycle_uncertain);
    assert(!old_high_storage_writes);
    discard_unresolved_fixture();

    seed(); fragment_fresh_space(); input.calls = 0;
    fail_write = table_attempt - 1 + 257; failed_write_persistence = 2;
    assert(mgfs_stage_and_cancel(&super, 2, 1, length, source, &input) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING);
    assert(fs.stage_transaction->released_count == 256);
    assert(fs.stage_transaction->ledger_count == 512 + MGFS_REPLACE_MAX_LISTS);
    assert(!table_writes && pin_live && pin_is_unresolved);
    discard_unresolved_fixture();
}

/* Mapping views are operation-local: validate all list blocks before using
 * any cached extent, preserve entry validation, then rebuild on every scan. */
static void directory_mapping_tests(void)
{
    struct { mgfs_extent_desc_t entries[3]; u64 guard; } view;
    u8 parent[MGFS_RECORD_BYTES], actual[400], expected[400];
    mgfs_directory_entry_t entry;
    seed_fragmented_parent();
    assert(vfs_super_operation_lock(&super));
    memcpy(parent, record_at(0), sizeof(parent));
    view.guard = 0xabcdef0123456789ULL;
    unsigned before = reads, before_allocations = allocations;
    assert(mgfs_read_directory_mapping(&fs, parent, MGFS_U64_MAX,
                                       NULL, view.entries, 3));
    assert(reads == before + 1 && allocations == before_allocations);
    assert(view.entries[0].physical_start == 7 &&
           view.entries[1].physical_start == 9 &&
           view.entries[2].physical_start == 11);
    for (u64 offset = 3900; offset < 4300; offset += 8) {
        for (u64 i = 0; i < sizeof(expected); i++) {
            u64 logical = (offset + i) / MGFS_BLOCK_BYTES;
            u64 physical = logical == 0 ? 7 : 9;
            expected[i] = media[physical][(offset + i) % MGFS_BLOCK_BYTES];
        }
        assert(mgfs_read_directory_mapped_bytes(&fs, parent, offset,
                                                sizeof(actual), actual,
                                                view.entries, 3));
        assert(!memcmp(actual, expected, sizeof(actual)));
    }
    /* Header and entry span the physical discontinuity at the boundary. */
    u8 serialized[40];
    write_dir_entry(serialized, "cross", 5);
    memcpy(media[7] + 4080, serialized, 16);
    memcpy(media[9], serialized + 16, 24);
    assert(mgfs_read_directory_entry_mapped(&fs, parent, 4080, &entry,
                                            view.entries, 3));
    assert(entry.total_bytes == 40 && strcmp(entry.name, "cross") == 0);
    media[9][0] ^= 1;
    assert(!mgfs_read_directory_entry_mapped(&fs, parent, 4080, &entry,
                                             view.entries, 3));
    media[9][0] ^= 1;
    fail_read = reads + 1;
    assert(!mgfs_read_directory_entry_mapped(&fs, parent, 4080, &entry,
                                             view.entries, 3));
    fail_read = 0;
    fs.publication_uncertain = true;
    before = reads;
    assert(!mgfs_read_directory_mapped_bytes(&fs, parent, 0, 1, actual,
                                             view.entries, 3));
    assert(reads == before);
    fs.publication_uncertain = false;
    assert(!mgfs_read_directory_mapped_bytes(&fs, parent, MGFS_U64_MAX,
                                             1, actual, view.entries, 3));
    assert(!mgfs_read_directory_mapped_bytes(&fs, parent, 12288,
                                             1, actual, view.entries, 3));
    assert(view.guard == 0xabcdef0123456789ULL && !writes);
    vfs_super_operation_unlock(&super);

    /* No partially populated view is used after a validation error. */
    for (unsigned fault = 0; fault < 8; fault++) {
        seed_fragmented_parent();
        assert(vfs_super_operation_lock(&super));
        memcpy(parent, record_at(0), sizeof(parent));
        u8 *list = media[13];
        if (fault == 0) list[32] ^= 1; /* checksum */
        if (fault == 1) put(list + 8, 77); /* owner */
        if (fault == 2) list[40] = 1; /* reserved, checksum resealed below */
        if (fault == 3) put(list + 16, 13); /* cycle */
        if (fault == 4) put(list + 72, FIXTURE_BLOCKS); /* physical bound */
        if (fault == 5) { /* More entries than the record/caller capacity. */
            put(list + 24, 2);
            mgfs_store_extent(list, 96, 3, 12, 1, MGFS_EXTENT_DIRECTORY_METADATA);
        }
        if (fault == 6) fail_read = reads + 1;
        if (fault == 7) put(parent + 48, 3); /* inline array bound */
        if (fault != 0) mgfs_recompute_checksum(list, 32, MGFS_BLOCK_BYTES);
        view.guard = 0xabcdef0123456789ULL;
        assert(!mgfs_read_directory_mapping(&fs, parent, MGFS_U64_MAX,
                                            NULL, view.entries, 3));
        assert(view.guard == 0xabcdef0123456789ULL && !writes &&
               !fs.record_lifecycle_uncertain && !live_bytes);
        vfs_super_operation_unlock(&super);
    }
    seed_fragmented_parent();
    assert(vfs_super_operation_lock(&super));
    before = reads;
    assert(!mgfs_read_directory_mapping(&fs, record_at(0), MGFS_U64_MAX,
                                        NULL, view.entries, 2));
    assert(reads == before && !writes);
    vfs_super_operation_unlock(&super);

    /* The next create rebuilds its view after publication; it must see the
     * appended entry rather than use an earlier parent record/mapping. */
    seed_large_parent(130, true);
    vfs_node_t dir = {.super = &super, .inode = 1,
                      .type = VFS_TYPE_DIRECTORY, .fs_data = (void *)1};
    u64 id = 0;
    assert(mgfs_create_contents_private(&dir, "fresh", 0, NULL, NULL, &id) == VFS_OK);
    assert(id == 3 && !live_bytes && reads < 20000);
    unsigned saved_writes = writes;
    assert(mgfs_create_contents_private(&dir, "fresh", 0, NULL, NULL, &id) ==
           VFS_ERR_ALREADY_EXISTS);
    assert(!id && writes == saved_writes && !live_bytes);
    assert(mgfs_create_contents_private(&dir, "next", 0, NULL, NULL, &id) == VFS_OK);
    assert(id == 4 && !live_bytes && !fs.record_lifecycle_uncertain);
}

static void read_only_tests(void)
{
    seed(); super.read_only = true;
    vfs_node_t file = {.type=VFS_TYPE_FILE, .super=&super, .inode=2};
    vfs_node_t parent = {.type=VFS_TYPE_DIRECTORY, .super=&super, .inode=1};
    vfs_node_t *created = (void *)1;
    u64 pin=123, child=123;
    mg_file_lines_state_t lines;
    mgfs_fs_t original = fs;
    assert(!mgfs_write(&file, 0, 1, "x"));
    assert(mgfs_truncate(&file) == VFS_ERR_ACCESS_DENIED);
    assert(mgfs_append_lines(&file, "hi\n", 3, 2, 0, &lines) == VFS_ERR_ACCESS_DENIED);
    assert(mgfs_stage_locked(&super, 2, 1, 0, NULL, NULL, true, &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    assert(mgfs_create_contents_locked(&parent, "new", 0, NULL, NULL, &pin, &child) == VFS_ERR_ACCESS_DENIED && !pin && !child);
    assert(mgfs_create(&parent, "new", &created) == VFS_ERR_ACCESS_DENIED && !created);
    assert(mgfs_mkdir(&parent, "new", &created) == VFS_ERR_ACCESS_DENIED && !created);
    assert(mgfs_create_owned(&parent, "new", 7, VFS_PERMISSION_OWNER_READ, &created) == VFS_ERR_ACCESS_DENIED && !created);
    assert(mgfs_unlink(&parent, "old") == VFS_ERR_ACCESS_DENIED);
    assert(mgfs_rmdir(&parent, "old") == VFS_ERR_ACCESS_DENIED);
    assert(mgfs_rename_entry(&parent, "old", &parent, "new") == VFS_ERR_ACCESS_DENIED);
    assert(!writes && !pin_begins && !live_allocations && !allocations);
    assert(!memcmp(&original, &fs, sizeof(fs)));
    assert_original();
}

int main(void)
{
    directory_mapping_tests();
    file_facts_tests();
    generation_overflow_tests();
    replacement_hook_tests();
    seed(); resolved(VFS_OK, 0, 0); assert(!writes);
    seed(); resolved(VFS_OK, 56, 0); assert(writes == 2);
    seed();
    u8 *inline_file = record_at(1);
    put(inline_file + 8, mgfs_get_le64(inline_file + 8) |
                         MGFS_RECORD_INLINE_DATA);
    put(inline_file + 32, 3);
    put(inline_file + 40, 0);
    put(inline_file + 48, 0);
    memset(inline_file + 64, 0, 64);
    memcpy(inline_file + 128, "abc", 3);
    media[1][MGFS_BITMAP_HEADER_BYTES] &= (u8)~(1U << 1);
    seal();
    source_state_t inline_growth = {57, 0, 0, 0};
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source,
                                 &inline_growth) == VFS_OK);
    assert(!pin_live && !live_allocations && !fs.record_lifecycle_uncertain);
    assert(mgfs_get_le64(record_at(1) + 32) == 3);
    assert(!memcmp(record_at(1) + 128, "abc", 3));
    assert(!table_writes && !original_data_writes);
    seed(); resolved(VFS_OK, 57, 0); assert(writes == 5);
    for (unsigned i = 57; i < MGFS_BLOCK_BYTES; i++) assert(media[4][i] == 0);
    seed(); resolved(VFS_OK, 4096, 0); assert(writes == 5);
    seed(); resolved(VFS_OK, 4097, 0); assert(writes == 8);
    for (unsigned i = 1; i < MGFS_BLOCK_BYTES; i++) assert(media[6][i] == 0);
    seed(); resolved(VFS_OK, MGFS_STAGE_MAX_BLOCKS * MGFS_BLOCK_BYTES, 0);
    seed(); resolved(VFS_ERR_IO, 4097, 2); assert(writes == 5);
    seed(); resolved(VFS_ERR_IO, 56, 1); assert(writes == 2);
    seed();
    put(media[0] + 64, MGFS_STATE_NEEDS_FSCK);
    seal();
    source_state_t state_needs_fsck = {57, 0, 0, 0};
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source,
                                 &state_needs_fsck) == VFS_OK);
    assert(mgfs_get_le64(media[0] + 64) == MGFS_STATE_NEEDS_FSCK);
    assert(!pin_live && !live_allocations && !fs.record_lifecycle_uncertain);
    seed();
    source_state_t error_state = {4097, 0, 0, 2};
    assert(mgfs_stage_and_cancel(&super, 2, 1, error_state.length,
                                 source, &error_state) == VFS_ERR_IO);
    assert(!pin_live && !live_allocations && !fs.record_lifecycle_uncertain);
    assert_original();
    seed();
    media[1][MGFS_BITMAP_HEADER_BYTES] |= (1U << 3); /* reserve block 7 */
    seal();
    resolved(VFS_OK, 3 * MGFS_BLOCK_BYTES, 0);
    assert(mgfs_get_le64(media[9]) == MGFS_EXTENT_LIST_MAGIC);
    seed();
    for (unsigned block = 7; block < 4 + 2 * MGFS_STAGE_MAX_BLOCKS;
         block += 2) {
        unsigned bit = block - 4;
        media[1][MGFS_BITMAP_HEADER_BYTES + bit / 8] |=
            (u8)(1U << (bit % 8));
    }
    seal();
    resolved(VFS_OK, MGFS_STAGE_MAX_BLOCKS * MGFS_BLOCK_BYTES, 0);
    assert(mgfs_get_le64(media[4 + 2 * MGFS_STAGE_MAX_BLOCKS]) ==
           MGFS_EXTENT_LIST_MAGIC);
    seed();
    source_state_t state = {MGFS_REPLACE_MAX_BLOCKS * MGFS_BLOCK_BYTES + 1,
                            0, 0, 0};
    assert(mgfs_stage_and_cancel(&super, 2, 1, state.length, source, &state) ==
           VFS_ERR_NO_SPACE);
    assert(!reads && !writes && !pin_live && !live_allocations);
    seed();
    media[1][MGFS_BITMAP_HEADER_BYTES] &= (u8)~(1U << 1);
    seal();
    state.length = 57;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_BAD_FORMAT);
    assert(fs.record_lifecycle_uncertain && !pin_live && !writes);
    seed();
    put(record_at(1), MGFS_RECORD_DIRECTORY);
    seal();
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_UNSUPPORTED);
    assert(!fs.record_lifecycle_uncertain && !pin_live && !writes);
    seed();
    memset(media[1] + MGFS_BITMAP_HEADER_BYTES, 0xFF,
           MGFS_BLOCK_BYTES - MGFS_BITMAP_HEADER_BYTES);
    seal();
    state.length = 57;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_NO_SPACE);
    assert(!pin_live && !live_allocations && !fs.record_lifecycle_uncertain);
    assert_original();
    seed();
    assert(mgfs_stage_and_cancel(&super, 2, 2, 57, source, &state) == VFS_ERR_BUSY);
    assert(!writes && !pin_live);
    seed();
    put(record_at(1) + 24, MGFS_U64_MAX);
    seal();
    assert(mgfs_stage_and_cancel(&super, 2, MGFS_U64_MAX, 57,
                                 source, &state) == VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live);
    for (unsigned failing_allocation = 1; failing_allocation <= 3;
         failing_allocation++) {
        seed(); fail_allocation = failing_allocation;
        assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
               VFS_ERR_NO_MEM);
        assert(!writes && !pin_live && !live_allocations);
    }
    seed(); fail_pin_begin = true;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_NO_SPACE);
    assert(!writes && !pin_live && !live_allocations &&
           super.reference_count == 1);
    for (unsigned failing_write = 1; failing_write <= 5; failing_write++) {
        seed(); fail_write = failing_write;
        failed_write_persistence = failing_write == 2 ? 1 :
                                   failing_write == 3 ? 2 : 0;
        int result = mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state);
        assert(result == VFS_ERR_UNCERTAIN);
        assert(fs.record_lifecycle_uncertain && fs.stage_transaction);
        assert(pin_live && pin_is_unresolved && pin_unresolved == 1);
        assert(pin_finishes == 0 && super.reference_count == 2);
        assert(fs.stage_transaction->phase ==
            (failing_write == 1 ? MGFS_STAGE_SETTING_STATE :
             failing_write == 2 ? MGFS_STAGE_ALLOCATING :
             failing_write == 3 ? MGFS_STAGE_WRITING_DATA :
             failing_write == 4 ? MGFS_STAGE_RECLAIMING :
                                  MGFS_STAGE_RESTORING_STATE));
        assert(fs.stage_transaction->ledger_count ==
               (failing_write == 1 ? 0 : 1));
        assert(!memcmp(fs.stage_transaction->original_table, media[2],
                       MGFS_BLOCK_BYTES));
        assert(mgfs_validate_metadata_header(
            fs.stage_transaction->proposed_table,
            MGFS_METADATA_RECORD_TABLE, 0));
        assert(fs.stage_transaction->pin_id == last_pin_id);
        if (failing_write == 3) {
            const u8 *old_record = fs.stage_transaction->original_table +
                MGFS_BITMAP_HEADER_BYTES + MGFS_RECORD_BYTES;
            const u8 *proposed_record = fs.stage_transaction->proposed_table +
                MGFS_BITMAP_HEADER_BYTES + MGFS_RECORD_BYTES;
            assert((mgfs_get_le64(old_record + 8) & ~MGFS_RECORD_INLINE_DATA) ==
                   (mgfs_get_le64(proposed_record + 8) &
                    ~MGFS_RECORD_INLINE_DATA));
            assert(mgfs_get_le64(proposed_record + 16) == 2);
            assert(mgfs_get_le64(proposed_record + 24) == 1);
        }
        if (failing_write >= 4) {
            const u8 *proposed_record = fs.stage_transaction->proposed_table +
                MGFS_BITMAP_HEADER_BYTES + MGFS_RECORD_BYTES;
            assert(mgfs_validate_record(proposed_record));
            assert(mgfs_validate_regular_file_record(proposed_record));
            assert(mgfs_get_le64(proposed_record + 24) == 2);
            assert(mgfs_get_le64(proposed_record + 32) == 57);
        }
        assert_original();
        /* Deliberately retained by design; release fixture memory only. */
        mgfs_stage_free(fs.stage_transaction);
        fs.stage_transaction = NULL;
        pin_live = false; super.reference_count = 1;
    }
    seed(); fail_write = 7;
    state.length = 4097;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 4097, source, &state) ==
           VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_is_unresolved && fs.stage_transaction);
    assert(fs.stage_transaction->released_count == 1);
    assert(fs.stage_transaction->ledger_count == 2);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING);
    assert_original();
    mgfs_stage_free(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = false; super.reference_count = 1;
    seed();
    media[1][MGFS_BITMAP_HEADER_BYTES] |= 1U << 3;
    seal();
    fail_write = 10;
    state.length = 3 * MGFS_BLOCK_BYTES;
    assert(mgfs_stage_and_cancel(&super, 2, 1, state.length,
                                 source, &state) == VFS_ERR_UNCERTAIN);
    assert(fs.stage_transaction && fs.stage_transaction->ledger_count == 4);
    const u8 *fragmented_proposal = fs.stage_transaction->proposed_table +
        MGFS_BITMAP_HEADER_BYTES + MGFS_RECORD_BYTES;
    assert(mgfs_validate_metadata_header(fs.stage_transaction->proposed_table,
           MGFS_METADATA_RECORD_TABLE, 0));
    assert(mgfs_validate_regular_file_record(fragmented_proposal));
    assert(mgfs_get_le64(fragmented_proposal + 40) == 3);
    assert(mgfs_get_le64(fragmented_proposal + 56) == 9);
    assert(mgfs_get_le64(media[9]) == MGFS_EXTENT_LIST_MAGIC);
    assert_original();
    mgfs_stage_free(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = false; super.reference_count = 1;
    seed(); fail_bitmap_read_after_writes = 1;
    state.length = 57;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_IO);
    assert(!fs.record_lifecycle_uncertain && !pin_live && !live_allocations);
    assert_original();
    seed(); fail_bitmap_read_after_writes = 3;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_UNCERTAIN);
    assert(fs.record_lifecycle_uncertain && pin_is_unresolved);
    assert(fs.stage_transaction->phase == MGFS_STAGE_RECLAIMING);
    assert(fs.stage_transaction->ledger_count == 1);
    mgfs_stage_free(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = false; super.reference_count = 1;
    seed(); fail_pin_finish_busy = fail_mutex_wait = true;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_is_unresolved && fs.stage_transaction);
    assert(fs.stage_transaction->released_count == 1);
    assert_original();
    mgfs_stage_free(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = false; super.reference_count = 1;
    seed();
    fail_read = 1;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) == VFS_ERR_IO);
    assert(!fs.record_lifecycle_uncertain && !pin_live && !writes && !live_allocations);
    read_only_tests();
    version_admission_tests();
    publication_tests();
    large_replacement_tests();
    create_contents_tests();
    large_create_tests();
    puts("MGFS staging, publication, and cancellation: production paths passed");
}
