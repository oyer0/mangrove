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
    assert(block < FIXTURE_BLOCKS && operation_locked && pin_live);
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
    if (fail_mutex_wait) return false;
    operation_locked = true;
    return true;
}

bool mutex_unlock(mutex_t *mutex)
{
    assert(mutex == &super.operation_lock && operation_locked);
    operation_locked = false;
    return true;
}

int vfs_recovery_pin_begin(vfs_super_t *sb, u64 *out_id)
{
    assert(sb == &super && operation_locked && !pin_live);
    if (fail_pin_begin) return VFS_ERR_NO_SPACE;
    pin_begins++;
    pin_live = true;
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

int vfs_recovery_pin_finish(u64 id)
{
    assert(!operation_locked && pin_live && !pin_is_unresolved && id == last_pin_id);
    if (fail_pin_finish_busy) return VFS_ERR_BUSY;
    pin_live = false;
    pin_finishes++;
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

int main(void)
{
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
    puts("MGFS staging/cancellation: production paths and failure boundaries passed");
}
