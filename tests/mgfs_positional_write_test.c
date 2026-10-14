/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production MGFS plus extracted VFS; controlled media/heap/locks. */
/* The runner omits the included fixture's main in its temporary copy. */
#define mutex_lock fixture_mutex_lock
#define mutex_unlock fixture_mutex_unlock
#define vfs_check_access fixture_check_access
#include "mgfs_stage_cancel_test.c"
#undef mutex_lock
#undef mutex_unlock
#undef vfs_check_access

static vfs_node_t file_node;
static vfs_file_handle_t file_handle;
static bool offset_locked;
static u8 expected[MG_FILE_CONTENT_MAX_BYTES + 8192U];
static u8 readback[sizeof(expected)];
static u64 expected_length;
static u32 initial_blocks[MGFS_REPLACE_MAX_BLOCKS];
static u32 initial_count;
static u8 original_bitmap[MGFS_BLOCK_BYTES];
static u8 original_media[sizeof(media)];

bool mutex_lock(mutex_t *lock)
{
    if (lock == &file_handle.offset_lock) {
        assert(!offset_locked); offset_locked = true; return true;
    }
    return fixture_mutex_lock(lock);
}
bool mutex_unlock(mutex_t *lock)
{
    if (lock == &file_handle.offset_lock) {
        assert(offset_locked); offset_locked = false; return true;
    }
    return fixture_mutex_unlock(lock);
}
bool vfs_super_is_live(const vfs_super_t *sb)
{ return sb == &super && sb->state != VFS_SUPER_DEAD; }
bool vfs_check_access(const vfs_node_t *node, u32 access)
{ return node && node->super == &super && node->inode == 2 && (node->permissions & access) == access; }
static bool vfs_handle_authorized(const vfs_file_handle_t *h, u32 access)
{ (void)h; (void)access; return false; }
static bool vfs_node_on_read_only_mount(const vfs_node_t *node)
{ return !node || node->super->read_only; }
#include "positional_vfs.inc"

static void used(u32 block)
{
    u32 bit = block - (u32)fs.layout.data_start;
    media[1][MGFS_BITMAP_HEADER_BYTES + bit/8U] |= (u8)(1U << (bit%8U));
}

static void prepare_file(u64 length, bool fragmented)
{
    seed();
    assert(length <= MG_FILE_CONTENT_MAX_BYTES);
    u8 *record = record_at(1);
    memset(record, 0, MGFS_RECORD_BYTES);
    put(record, MGFS_RECORD_FILE);
    put(record+8, (7ULL << MGFS_RECORD_OWNER_SHIFT) |
        (3ULL << MGFS_RECORD_PERMISSIONS_SHIFT) |
        (length <= MGFS_INLINE_DATA_BYTES ? MGFS_RECORD_INLINE_DATA : 0U));
    put(record+16, 2); put(record+24, 1); put(record+32, length);
    memset(media[1]+MGFS_BITMAP_HEADER_BYTES, 0, MGFS_BLOCK_BYTES-MGFS_BITMAP_HEADER_BYTES);
    initial_count = length <= MGFS_INLINE_DATA_BYTES ? 0U :
        (u32)(length/MGFS_BLOCK_BYTES + (length%MGFS_BLOCK_BYTES != 0));
    mgfs_extent_desc_t extents[MGFS_REPLACE_MAX_BLOCKS];
    if(fragmented && initial_count>fs.layout.record_count)fs.layout.record_count=initial_count;
    for (u64 i = 0; i < length; i++) expected[i] = (u8)(1U+i%251U);
    expected_length = length;
    if (!initial_count) memcpy(record+128, expected, (usize)length);
    else {
        for (u32 i = 0; i < initial_count; i++) {
            u32 physical = 5U + i*(fragmented ? 2U : 1U);
            initial_blocks[i] = physical; used(physical);
            memset(media[physical], 0xa5, MGFS_BLOCK_BYTES);
            u64 start = (u64)i*MGFS_BLOCK_BYTES;
            u64 bytes = length-start < MGFS_BLOCK_BYTES ? length-start : MGFS_BLOCK_BYTES;
            memcpy(media[physical], expected+start, (usize)bytes);
            if (fragmented) extents[i] = (mgfs_extent_desc_t){i,physical,1,MGFS_EXTENT_DATA};
        }
        if (!fragmented) extents[0] = (mgfs_extent_desc_t){0,5,initial_count,MGFS_EXTENT_DATA};
        u32 count = fragmented ? initial_count : 1U;
        put(record+40, count); put(record+48, count < 2U ? count : 2U);
        for (u32 i = 0; i < count && i < 2U; i++)
            mgfs_store_extent(record, 64U+i*32U, extents[i].logical_start,
                extents[i].physical_start, extents[i].block_count, MGFS_EXTENT_DATA);
        if (count > 2U) {
            used(2000); put(record+56, 2000);
            ordinary_mutation_test = operation_locked = true;
            u32 lists=(count-2U+MGFS_EXTENTS_PER_LIST_BLOCK-1U)/MGFS_EXTENTS_PER_LIST_BLOCK;
            for(u32 j=0;j<lists;j++) {
                u8 *list=media[2000U+j]; used(2000U+j);
                u32 start=2U+j*MGFS_EXTENTS_PER_LIST_BLOCK;
                u32 entries=count-start<MGFS_EXTENTS_PER_LIST_BLOCK?count-start:MGFS_EXTENTS_PER_LIST_BLOCK;
                put(list,MGFS_EXTENT_LIST_MAGIC); put(list+8,2);
                put(list+16,j+1U<lists?2001U+j:0); put(list+24,entries);
                for(u32 k=0;k<entries;k++) {
                    mgfs_extent_desc_t *e=&extents[start+k];
                    mgfs_store_extent(list,MGFS_EXTENT_LIST_HEADER_BYTES+k*32U,
                        e->logical_start,e->physical_start,e->block_count,e->flags);
                }
                mgfs_recompute_checksum(list,32,MGFS_BLOCK_BYTES);
            }
            operation_locked = false;
        }
    }
    seal(); memcpy(original_bitmap, media[1], MGFS_BLOCK_BYTES);
    file_node = (vfs_node_t){.super=&super,.inode=2,.type=VFS_TYPE_FILE,
        .owner_uid=7,.permissions=3,.size=length,.ops=&mgfs_node_ops};
    file_handle = (vfs_file_handle_t){.node=&file_node,.super=&super,
        .valid=VFS_FILE_HANDLE_VALID,.flags=VFS_OPEN_RDWR};
    tracked_node = &file_node; ordinary_mutation_test = true; offset_locked = false;
    reads = writes = allocations = table_writes = 0; peak_bytes = 0;
}

static u64 write_at(u64 at, const void *bytes, usize length)
{
    assert(at <= 0x7fffffffffffffffULL);
    assert(vfs_seek(&file_handle, (i64)at, VFS_SEEK_SET, NULL) == VFS_OK);
    u64 written = 99;
    assert(vfs_file_write_status(&file_handle, length, bytes, &written) == VFS_OK);
    assert(!operation_locked && !offset_locked && !pin_live && !live_allocations);
    return written;
}

static void verify_contents(void)
{
    vfs_file_facts_t facts;
    assert(mgfs_file_facts(&file_node, &facts) == VFS_OK);
    assert(facts.record_id == 2 && facts.owner_uid == 7 && facts.permissions == 3);
    assert(facts.size == expected_length && file_node.size == expected_length);
    assert(vfs_seek(&file_handle, 0, VFS_SEEK_SET, NULL) == VFS_OK);
    u64 received = vfs_file_read(&file_handle, expected_length+1U, readback);
    if (received != expected_length)
        fprintf(stderr,"read length=%llu received=%llu flags=%llu error=%s\n",
            (unsigned long long)expected_length,(unsigned long long)received,
            (unsigned long long)mgfs_get_le64(record_at(1)+8),mgfs_last_error());
    assert(received == expected_length);
    assert(!memcmp(readback, expected, (usize)expected_length));
    assert(!vfs_file_read(&file_handle, 1, readback) && !operation_locked && !offset_locked);
}

static void overwrite_tests(void)
{
    const u64 lengths[] = {0,1,8,55,56,57,4095,4096,4097,12288,65535,MG_FILE_CONTENT_MAX_BYTES};
    unsigned cases = 0;
    for (unsigned layout = 0; layout < 2; layout++)
        for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
            if (layout && lengths[i] > 65535U) continue;
            prepare_file(lengths[i], layout);
            const u64 offsets[] = {0, lengths[i]/2, lengths[i], lengths[i] ? lengths[i]-1 : 0};
            for (unsigned j = 0; j < 4; j++) {
                const char data[] = "positional";
                u64 at = offsets[j];
                assert(write_at(at, data, sizeof(data)-1) == sizeof(data)-1);
                memcpy(expected+at, data, sizeof(data)-1);
                if (at+sizeof(data)-1 > expected_length) expected_length = at+sizeof(data)-1;
                verify_contents(); cases++;
                for (u32 k = 0; k < initial_count; k++) {
                    u32 bit = initial_blocks[k] - (u32)fs.layout.data_start;
                    assert(media[1][MGFS_BITMAP_HEADER_BYTES+bit/8U] & (1U << (bit%8U)));
                }
            }
        }
    for (unsigned layout = 0; layout < 2; layout++) {
        prepare_file(16384, layout);
        assert(write_at(4095, "cross", 5) == 5); memcpy(expected+4095, "cross", 5);
        assert(!memcmp(media[1], original_bitmap, MGFS_BLOCK_BYTES));
        verify_contents();
        assert(write_at(12287, "XY", 2) == 2); memcpy(expected+12287, "XY", 2);
        assert(!memcmp(media[1], original_bitmap, MGFS_BLOCK_BYTES)); verify_contents();
    }
    /* Existing allocation past EOF is owned too, not disposable on overwrite. */
    prepare_file(12288, true);
    put(record_at(1)+32,5000); seal(); file_node.size=expected_length=5000;
    assert(write_at(1,"XY",2)==2); memcpy(expected+1,"XY",2);
    assert(!memcmp(media[1],original_bitmap,MGFS_BLOCK_BYTES)); verify_contents();
    prepare_file(12288, true);
    put(record_at(1)+32,8); seal(); file_node.size=expected_length=8;
    assert(write_at(1,"XY",2)==2); memcpy(expected+1,"XY",2);
    assert(!memcmp(media[1],original_bitmap,MGFS_BLOCK_BYTES)); verify_contents();
    assert(!(mgfs_get_le64(record_at(1)+8)&MGFS_RECORD_INLINE_DATA));
    /* Free lower holes do not satisfy ascending regular-file growth. */
    prepare_file(8192, false); fs.layout.data_blocks=3;
    assert(write_at(8192,"X",1)==0 && file_node.size==8192 &&
        !fs.record_lifecycle_uncertain && !memcmp(media[1],original_bitmap,MGFS_BLOCK_BYTES));
    verify_contents();
    assert(write_at(0,"X",1)==1); expected[0]='X'; verify_contents();
    prepare_file(4096, false);
    assert(write_at(0, "X", 1) == 1); expected[0]='X';
    printf("MGFS prefix overwrite: reads=%u writes=%u allocations=%u peak=%zu length=%llu\n",
        reads,writes,allocations,(size_t)peak_bytes,(unsigned long long)file_node.size);
    verify_contents();
    assert(vfs_resize_handle(&file_handle, 7) == VFS_ERR_UNSUPPORTED);
    unsigned before = writes;
    assert(write_at(expected_length+1U, "X", 1) == 0 && writes == before);
    assert(write_at(0, NULL, 0) == 0 && writes == before); verify_contents();
    u64 written=99;
    assert(vfs_seek(&file_handle,1,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(&file_handle,MGFS_U64_MAX,"X",&written)==VFS_OK &&
        !written && writes==before); verify_contents();
    assert(vfs_truncate_handle(&file_handle) == VFS_OK);
    expected_length=0; verify_contents();
    printf("MGFS positional content: %u repeated inline/extent/append writes, crossing blocks, ownership and explicit truncation passed\n", cases);
}

static void history_tests(void)
{
    prepare_file(0, false);
    mg_file_lines_state_t state;
    assert(vfs_file_append_lines(&file_handle, "one\ntwo\nthree\n", 14, 3, MG_FILE_LINES_RESET, &state) == VFS_OK);
    assert(state.length==14 && state.revision==2);
    vfs_node_t stale=file_node; stale.size=0; file_handle.node=&stale;
    assert(vfs_file_append_lines(&file_handle, "four\n", 5, 2, 0, &state) == VFS_OK);
    file_handle.node=&file_node; /* Merge must use the current persistent Record. */
    memcpy(expected,"three\nfour\n",11); expected_length=11;
    assert(state.length==11 && state.revision==3 && state.append_offset==6); verify_contents();
    assert(vfs_file_append_lines(&file_handle, "x\n", 2, 2, MG_FILE_LINES_RESET, &state) == VFS_OK);
    memcpy(expected,"x\n",2); expected_length=2; assert(state.length==2); verify_contents();
    assert(vfs_file_append_lines(&file_handle, NULL, 0, 2, MG_FILE_LINES_RESET, &state) == VFS_OK);
    expected_length=0; assert(!state.length); verify_contents();
    /* A long old history must release trailing allocation only explicitly. */
    prepare_file(12288, true);
    char lines[12288]; for (usize i=0;i<sizeof(lines);i++) lines[i]= i%2 ? '\n' : 'a';
    assert(write_at(0,lines,sizeof(lines))==sizeof(lines));
    assert(vfs_file_append_lines(&file_handle,"last\n",5,3,0,&state)==VFS_OK);
    memcpy(expected,"a\na\nlast\n",9); expected_length=9; assert(state.length==9); verify_contents();
    assert(!(mgfs_get_le64(record_at(1)+40)) && mgfs_get_le64(record_at(1)+8)&MGFS_RECORD_INLINE_DATA);
    puts("MGFS history exact replacement/reset/empty/oldest trimming and extent-to-inline transition passed");
}

static void linked_mapping_tests(void)
{
    for(u32 count=130;count<=512;count=512) {
        prepare_file((u64)count*MGFS_BLOCK_BYTES,true);
        memcpy(original_media,media,sizeof(media));
        assert(write_at(1,"XY",2)==2);memcpy(expected+1,"XY",2);
        assert(!memcmp(media[1],original_bitmap,MGFS_BLOCK_BYTES));
        assert(!memcmp(media[2000],original_media+2000U*MGFS_BLOCK_BYTES,5U*MGFS_BLOCK_BYTES));
        printf("MGFS linked overwrite: extents=%u reads=%u writes=%u allocations=%u peak=%zu\n",
            count,reads,writes,allocations,(size_t)peak_bytes);
        verify_contents();
        unsigned attempts=writes;
        assert(!write_at(expected_length,"Z",1) && writes==attempts+1U && !fs.record_lifecycle_uncertain);
        verify_contents(); /* Unsupported mapping growth changed no contents. */
        if(count==512)break;
    }
    mg_file_lines_state_t state;
    assert(vfs_file_append_lines(&file_handle,"x\n",2,3,MG_FILE_LINES_RESET,&state)==VFS_OK);
    memcpy(expected,"x\n",2);expected_length=2;verify_contents();
    for(u32 i=0;i<initial_count;i++) {
        u32 bit=initial_blocks[i]-(u32)fs.layout.data_start;
        assert(!(media[1][MGFS_BITMAP_HEADER_BYTES+bit/8U]&(1U<<(bit%8U))));
    }
    puts("MGFS linked-list overwrite preserves suffix allocation and list bytes; growth limit and explicit history reclamation passed");
}

static void prepare_fault(unsigned op)
{
    prepare_file(op == 0 ? 56 : op == 1 ? 16384 : 12288, op >= 2 && op != 6);
    if (op >= 4) {
        for (u32 i=0;i<12288;i++) expected[i]=media[initial_blocks[i/4096]][i%4096]=i%2?'\n':'a';
    }
    reads=writes=allocations=0; peak_bytes=0;
    memcpy(original_media,media,sizeof(media));
}

static bool fault_operation(unsigned op)
{
    if (op < 4) {
        const u64 at[] = {1,4095,4095,12288};
        return write_at(at[op],"XYZXYZ",6)==6;
    }
    mg_file_lines_state_t state;
    return vfs_file_append_lines(&file_handle,op==5?"x\n":"last\n",op==5?2:5,
        3,op==5?MG_FILE_LINES_RESET:0,&state)==VFS_OK;
}

static void verify_fault_success(unsigned op)
{
    if(op<4) {
        const u64 at[]={1,4095,4095,12288};
        memcpy(expected+at[op],"XYZXYZ",6);
        if(at[op]+6>expected_length)expected_length=at[op]+6;
    } else {
        expected_length=op==5?2:9;
        memcpy(expected,op==5?"x\n":"a\na\nlast\n",(usize)expected_length);
    }
    verify_contents();
}

static void fault_tests(void)
{
    unsigned cases=0, after_data_before_generation=0, fallback_successes=0;
    for (unsigned op=0;op<7;op++) {
        prepare_fault(op); assert(fault_operation(op));
        const unsigned totals[] = {reads,writes,allocations};
        for (unsigned kind=0;kind<3;kind++)
            for (unsigned fault=1;fault<=totals[kind];fault++)
                for (unsigned persistence=0;persistence<(kind==1?3U:1U);persistence++) {
                    prepare_fault(op);
                    if(kind==0)fail_read=fault;
                    if(kind==1){fail_write=fault;failed_write_persistence=persistence;}
                    if(kind==2)fail_allocation=fault;
                    bool success=fault_operation(op);
                    fail_read=fail_write=fail_allocation=0;
                    if(success) {
                        assert(kind!=1); /* Only read/prefetch or buffer-allocation fallback. */
                        verify_fault_success(op); fallback_successes++;
                    }
                    else {
                        bool changed=memcmp(media[1],original_media+MGFS_BLOCK_BYTES,sizeof(media)-MGFS_BLOCK_BYTES)!=0;
                        if(changed || kind==1) assert(fs.record_lifecycle_uncertain);
                        if(fs.record_lifecycle_uncertain) {
                            if(mgfs_get_le64(record_at(1)+24)==1 && changed)
                                after_data_before_generation++;
                            unsigned attempts=writes;
                            assert(!write_at(0,"Y",1));
                            assert(vfs_truncate_handle(&file_handle)==VFS_ERR_UNCERTAIN);
                            assert(vfs_file_append_lines(&file_handle,"y\n",2,3,0,NULL)==VFS_ERR_UNCERTAIN);
                            assert(writes==attempts);
                        } else {
                            assert(!changed);
                            assert(fault_operation(op)); /* Clean pre-mutation failure is retryable. */
                            verify_fault_success(op);
                        }
                    }
                    assert(!pin_live && !live_allocations && !operation_locked && !offset_locked);
                    cases++;
                }
    }
    assert(after_data_before_generation);
    printf("MGFS positional/history faults: %u read/write/allocation cases, %u changes before generation, %u verified read/buffer fallbacks; torn/completed-failure writes, quarantine and clean retry passed\n",
        cases,after_data_before_generation,fallback_successes);
}

static void baseline_tests(void)
{
    prepare_file(4096, false);
    assert(write_at(0,"X",1)==1 && file_node.size==1);
    u32 bit=5U-(u32)fs.layout.data_start;
    assert(!(media[1][MGFS_BITMAP_HEADER_BYTES+bit/8U] & (1U << (bit%8U))));
    printf("Baseline production VFS/MGFS: one-byte overwrite shrinks 4096 -> 1 and frees original block; reads=%u writes=%u allocations=%u peak=%zu\n",
        reads,writes,allocations,(size_t)peak_bytes);
    prepare_file(12288, true);
    verify_contents(); /* The imported ascending mapping reads correctly. */
    assert(write_at(12288,"append",6)==6 && file_node.size==12294);
    assert(vfs_seek(&file_handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(!vfs_file_read(&file_handle,12294,readback));
    assert(!fs.record_lifecycle_uncertain && mgfs_get_le64(media[0]+64)==MGFS_STATE_CLEAN);
    puts("Baseline growth: successful append chooses a lower free block, then production read rejects physical extent order (mount still clean)");
}

int main(void)
{
#ifdef MGFS_POSITIONAL_BASELINE
    baseline_tests();
#else
    overwrite_tests(); history_tests(); linked_mapping_tests(); fault_tests();
#endif
    return 0;
}
