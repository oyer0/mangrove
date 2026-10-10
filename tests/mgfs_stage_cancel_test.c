/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host fixture includes the production MGFS implementation. Only block I/O,
 * heap, operation-lock, and recovery-pin dependencies are controlled here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/storage/mgfs.c"

#define FIXTURE_BLOCKS 1050U
static u8 media[FIXTURE_BLOCKS][MGFS_BLOCK_BYTES];
static u8 record_bits[MGFS_BLOCK_BYTES];
static mgfs_record_ref_t ids[64];
static block_device_t device;
static mgfs_fs_t fs;
static vfs_super_t super;
static unsigned reads, writes, fail_read, fail_write, allocations, fail_allocation;
static unsigned live_allocations, pin_begins, pin_finishes, pin_unresolved;
static bool operation_locked, pin_live, pin_is_unresolved;
static u64 last_pin_id;
static unsigned table_writes, original_data_writes;
static unsigned failed_write_persistence;
static unsigned fail_bitmap_read_after_writes;
static bool fail_pin_begin, fail_pin_finish_busy, fail_mutex_wait;
static bool fail_mutex_reacquire, fail_handoff_callback;
static bool attempt_mutations_during_finish, remove_device_during_finish;
static bool ordinary_mutation_test;
static unsigned blocked_handoff_mutations;
static vfs_recovery_handoff_t pin_handoff;
static void *pin_context;

/* Unused MGFS node-operation references kept by the included static tables. */
void vfs_node_register(vfs_node_t *node) { (void)node; assert(false); }
bool vfs_current_uid(u32 *out_uid) { (void)out_uid; assert(false); return false; }
void vfs_node_set_child_mutation_policy(
    vfs_node_t *node, vfs_child_mutation_policy_t policy)
{ (void)node; (void)policy; assert(false); }

void *kmalloc(usize size)
{
    if (++allocations == fail_allocation) return NULL;
    void *result = malloc(size);
    assert(result);
    live_allocations++;
    return result;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    assert(live_allocations);
    live_allocations--;
    free(ptr);
}

bool block_read_for_id(block_device_t *dev, u64 id, u64 lba,
                       u32 sectors, void *buffer)
{
    assert(dev == &device && id == device.id && sectors == 8 && lba % 8 == 0);
    assert(lba / 8 < FIXTURE_BLOCKS);
    if (lba / 8 == 1 && writes >= fail_bitmap_read_after_writes &&
        fail_bitmap_read_after_writes) return false;
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
    if (block == 2) table_writes++;
    if (block == 5) original_data_writes++;
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
        assert(readback == 0);
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
    mgfs_recompute_checksum(media[0], MGFS_SUPER_CHECKSUM_OFFSET,
                            MGFS_HEADER_BYTES);
}

static void seed(void)
{
    assert(!live_allocations);
    memset(media, 0, sizeof(media));
    memset(record_bits, 0, sizeof(record_bits));
    memset(ids, 0, sizeof(ids));
    memset(&fs, 0, sizeof(fs));
    memset(&super, 0, sizeof(super));
    memset(&device, 0, sizeof(device));
    device.id = 7;
    device.sector_size = 512;
    device.sector_count = FIXTURE_BLOCKS * 8ULL;
    fs.dev = &device;
    fs.device_id = device.id;
    fs.format_minor = MGFS_FORMAT_MINOR;
    fs.layout.record_count = 21;
    fs.layout.record_table_start = 2;
    fs.layout.record_table_blocks = 1;
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
    pin_begins = pin_finishes = pin_unresolved = 0;
    pin_live = pin_is_unresolved = operation_locked = false;
    table_writes = original_data_writes = 0;
    failed_write_persistence = 0;
    fail_bitmap_read_after_writes = 0;
    fail_pin_begin = fail_pin_finish_busy = fail_mutex_wait = false;
    fail_mutex_reacquire = fail_handoff_callback = false;
    pin_handoff = NULL; pin_context = NULL;
    attempt_mutations_during_finish = remove_device_during_finish = false;
    blocked_handoff_mutations = 0;
    ordinary_mutation_test = false;
    put(media[0] + 64, MGFS_STATE_CLEAN);
    put(media[0] + 96, fs.next_record_id);
    u8 *root = record_at(0);
    put(root, MGFS_RECORD_DIRECTORY);
    put(root + 8, 3ULL << MGFS_RECORD_PERMISSIONS_SHIFT);
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
    assert(pin_live && pin_is_unresolved && fs.stage_transaction);
    kfree(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = pin_is_unresolved = false;
    super.reference_count = 1;
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
    mgfs_store_extent(record_at(1), 64, 0, 5, 129, MGFS_EXTENT_DATA);
    for (u64 block = 5; block < 134; block++) mark_old_block(block);
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
    seed(); fs.format_minor = MGFS_FORMAT_MINOR_LEGACY;
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_UNSUPPORTED);
    assert(!reads && !writes);

    seed();
    ordinary_mutation_test = operation_locked = true;
    assert(mgfs_write(&node, 0, 1, "X") == 1);
    assert(mgfs_file_facts(&node, &facts) == VFS_OK && facts.generation == 2);
    assert(mgfs_truncate(&node) == VFS_OK);
    assert(mgfs_file_facts(&node, &facts) == VFS_OK &&
           facts.generation == 3 && facts.size == 0);
    operation_locked = ordinary_mutation_test = false;
    assert(!live_allocations && !fs.record_lifecycle_uncertain);

    seed();
    ordinary_mutation_test = operation_locked = true;
    fail_write = 2; /* The state write succeeds; the data write is ambiguous. */
    assert(mgfs_write(&node, 0, 1, "X") == 0);
    assert(fs.record_lifecycle_uncertain);
    assert(mgfs_file_facts(&node, &facts) == VFS_ERR_UNCERTAIN);
    assert(mgfs_get_le64(record_at(1) + 24) == 1);
    operation_locked = ordinary_mutation_test = false;
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

        assert(mgfs_write(&node, 0, 1, "X") == 1);
        assert(mgfs_file_facts(&node, &facts) == VFS_OK &&
               facts.generation == MGFS_U64_MAX);
        memcpy(snapshot, media, sizeof(media));
        unsigned before_writes = writes;
        u64 before_size = node.size;

        assert(mgfs_write(&node, 0, 1, "Y") == 0);
        assert(writes == before_writes &&
               memcmp(media, snapshot, sizeof(media)) == 0);
        assert(mgfs_truncate(&node) == VFS_ERR_NO_SPACE);
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

int main(void)
{
    file_facts_tests();
    generation_overflow_tests();
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
    source_state_t state = {MGFS_STAGE_MAX_BLOCKS * MGFS_BLOCK_BYTES + 1, 0, 0, 0};
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
    seed(); fs.format_minor = MGFS_FORMAT_MINOR_LEGACY;
    state.length = 57;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_UNSUPPORTED);
    assert(!reads && !writes && !pin_live);
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
            assert(mgfs_validate_record(&fs, proposed_record));
            assert(mgfs_validate_regular_file_record(proposed_record));
            assert(mgfs_get_le64(proposed_record + 24) == 2);
            assert(mgfs_get_le64(proposed_record + 32) == 57);
        }
        assert_original();
        /* Deliberately retained by design; release fixture memory only. */
        kfree(fs.stage_transaction);
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
    kfree(fs.stage_transaction);
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
    kfree(fs.stage_transaction);
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
    kfree(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = false; super.reference_count = 1;
    seed(); fail_pin_finish_busy = fail_mutex_wait = true;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) ==
           VFS_ERR_UNCERTAIN);
    assert(pin_live && pin_is_unresolved && fs.stage_transaction);
    assert(fs.stage_transaction->released_count == 1);
    assert_original();
    kfree(fs.stage_transaction);
    fs.stage_transaction = NULL;
    pin_live = false; super.reference_count = 1;
    seed();
    fail_read = 1;
    assert(mgfs_stage_and_cancel(&super, 2, 1, 57, source, &state) == VFS_ERR_IO);
    assert(!fs.record_lifecycle_uncertain && !pin_live && !writes && !live_allocations);
    publication_tests();
    puts("MGFS staging, publication, and cancellation: production paths passed");
}
