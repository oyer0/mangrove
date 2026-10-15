#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production MGFS ownership validation against a bounded block image.

Only block I/O and heap allocation are substituted. Checksums, record lookup,
record/extent validation, bitmap reads, and ownership decisions are production
functions extracted from mgfs.c. This is not a mount or hardware race test.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
source = (root / "kernel/src/storage/mgfs.c").read_text()


def function(name):
    match = re.search(r"^static [^\n;{}]*\b" + name + r"\s*\(", source, re.M)
    if not match:
        raise RuntimeError(f"production function {name} is absent")
    start = source.index("{", match.end())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef size_t usize;
#define MGFS_BLOCK_BYTES 4096U
#define MGFS_RECORD_BYTES 192U
#define MGFS_RECORDS_PER_TABLE_BLOCK 21ULL
#define MGFS_BITMAP_HEADER_BYTES 24U
#define MGFS_BITMAP_BITS_PER_BLOCK 32576ULL
#define MGFS_RECORD_CHECKSUM_OFFSET 184U
#define MGFS_METADATA_CHECKSUM_OFFSET 16U
#define MGFS_METADATA_ALLOCATION_BITMAP 1ULL
#define MGFS_METADATA_RECORD_TABLE 3ULL
#define MGFS_RECORD_FILE 1ULL
#define MGFS_RECORD_DIRECTORY 2ULL
#define MGFS_RECORD_INLINE_DATA 1ULL
#define MGFS_RECORD_OWNER_SHIFT 1U
#define MGFS_RECORD_OWNER_MASK (0xFFFFFFFFULL << MGFS_RECORD_OWNER_SHIFT)
#define MGFS_RECORD_PERMISSIONS_SHIFT 33U
#define MGFS_RECORD_PERMISSIONS_MASK (0xFULL << MGFS_RECORD_PERMISSIONS_SHIFT)
#define MGFS_RECORD_CHILD_MUTATION_OWNER_RESTRICTED (1ULL << 41)
#define MGFS_RECORD_FLAGS_KNOWN (MGFS_RECORD_INLINE_DATA | MGFS_RECORD_OWNER_MASK | MGFS_RECORD_PERMISSIONS_MASK | MGFS_RECORD_CHILD_MUTATION_OWNER_RESTRICTED)
#define MGFS_EXTENT_DATA 1ULL
#define MGFS_EXTENT_DIRECTORY_METADATA 2ULL
#define MGFS_EXTENT_LIST_MAGIC 0x315458455346474DULL
#define MGFS_EXTENT_LIST_HEADER_BYTES 64ULL
#define MGFS_EXTENTS_PER_LIST_BLOCK 126ULL
#define MGFS_INLINE_DATA_BYTES 56ULL
#define MGFS_CRC64_POLYNOMIAL 0x42F0E1EBA9EA3693ULL
#define MGFS_U64_MAX UINT64_MAX
#define VFS_PERMISSION_KNOWN 15U
typedef struct {u64 id,slot;} mgfs_record_ref_t;
typedef struct {u64 record_count,record_table_start,record_table_blocks,allocation_bitmap_start,data_start,data_blocks;} mgfs_layout_t;
typedef struct {u64 id;u32 sector_size;u64 sector_count;} block_device_t;
typedef struct {u64 record_count;u8 records[3][MGFS_RECORD_BYTES];} mgfs_ordinary_transaction_t;
typedef struct {
    mgfs_ordinary_transaction_t *ordinary_transaction;bool mutation_admitted;
    block_device_t *dev;u64 device_id;mgfs_layout_t layout;u8 *record_bitmap;
    mgfs_record_ref_t *record_ids;u64 record_id_capacity;
    bool record_lifecycle_uncertain;
} mgfs_fs_t;
static const char *mgfs_error;
static u8 disk[32][MGFS_BLOCK_BYTES],before[32][MGFS_BLOCK_BYTES],record_bits[MGFS_BLOCK_BYTES];
static mgfs_record_ref_t ids[64];
static mgfs_fs_t fs;
static block_device_t device={7,512,32*8};
static unsigned reads,fail_read,allocs,fail_alloc,live;
static void *kmalloc(usize n){if(++allocs==fail_alloc)return NULL;void *p=malloc(n);assert(p);live++;return p;}
static void kfree(void *p){if(p){assert(live);live--;free(p);}}
static bool block_read_for_id(block_device_t *dev,u64 id,u64 lba,u32 sectors,void *out){
    assert(dev==&device && id==device.id && sectors==8 && lba%8==0 && lba/8<32);
    if(++reads==fail_read)return false;
    memcpy(out,disk[lba/8],MGFS_BLOCK_BYTES);return true;
}
'''
for name in [
    "mgfs_get_le64", "mgfs_record_permissions", "mgfs_set_error",
    "mgfs_crc64", "mgfs_checksum_block", "mgfs_read_block",
    "mgfs_store_le64", "mgfs_recompute_checksum",
    "mgfs_validate_metadata_header", "mgfs_record_slot_allocated",
    "mgfs_hash_id", "mgfs_find_record_slot", "mgfs_validate_record",
    "mgfs_validate_regular_file_record", "mgfs_ownership_ranges_overlap",
    "mgfs_ownership_sift_down", "mgfs_ownership_sort_ranges",
    "mgfs_ownership_intersects_target", "mgfs_ownership_claim",
    "mgfs_ownership_scan_record", "mgfs_ownership_preflight_type",
    "mgfs_ownership_preflight",
]:
    if name == "mgfs_ownership_ranges_overlap":
        code += source[source.index("typedef enum {\n    MGFS_OWNERSHIP_VALID"):source.index("static bool mgfs_ownership_ranges_overlap")]
    code += function(name)

code += r'''
static void put(u8 *p,u64 value){mgfs_store_le64(p,value);}
static u8 *record_at(unsigned slot){assert(slot<21);return disk[2]+24+slot*192;}
static void allocate_block(u64 block){assert(block>=4&&block<32);u64 bit=block-4;disk[1][24+bit/8]|=(u8)(1U<<(bit%8));}
static void index_record(u64 id,u64 slot){
    u64 index=mgfs_hash_id(id)&63;
    while(ids[index].id)index=(index+1)&63;
    ids[index]=(mgfs_record_ref_t){id,slot};
    record_bits[24+slot/8]|=(u8)(1U<<(slot%8));
}
static void set_extent(u8 *p,u64 logical,u64 physical,u64 blocks,u64 type){
    put(p,logical);put(p+8,physical);put(p+16,blocks);put(p+24,type);
    for(u64 i=0;i<blocks && physical+i<32;i++)if(physical+i>=4)allocate_block(physical+i);
}
static void make_record(unsigned slot,u64 id,u64 type,u64 size,u64 extents,u64 inline_count,u64 list){
    u8 *r=record_at(slot);memset(r,0,192);
    put(r,type);put(r+8,((u64)3<<MGFS_RECORD_PERMISSIONS_SHIFT));
    put(r+16,id);put(r+24,1);put(r+32,size);put(r+40,extents);
    put(r+48,inline_count);put(r+56,list);index_record(id,slot);
}
static void make_list(u64 block,u64 id,u64 logical,u64 physical,u64 count,u64 type){
    assert(block>=4&&block<32);allocate_block(block);
    u8 *p=disk[block];memset(p,0,4096);
    put(p,MGFS_EXTENT_LIST_MAGIC);put(p+8,id);put(p+24,1);
    set_extent(p+64,logical,physical,count,type);
    mgfs_recompute_checksum(p,32,4096);
}
static void seal(void){
    for(unsigned slot=0;slot<21;slot++)if(record_bits[24+slot/8]&(1U<<(slot%8)))
        mgfs_recompute_checksum(record_at(slot),184,192);
    put(disk[1],MGFS_METADATA_ALLOCATION_BITMAP);put(disk[1]+8,0);
    mgfs_recompute_checksum(disk[1],16,4096);
    put(disk[2],MGFS_METADATA_RECORD_TABLE);put(disk[2]+8,0);
    mgfs_recompute_checksum(disk[2],16,4096);
}
static void seed(void){
    assert(!live);memset(&fs,0,sizeof(fs));memset(disk,0,sizeof(disk));
    memset(record_bits,0,sizeof(record_bits));memset(ids,0,sizeof(ids));
    reads=fail_read=allocs=fail_alloc=0;mgfs_error="no error";
    fs.layout=(mgfs_layout_t){21,2,1,1,4,28};
    fs.record_bitmap=record_bits;fs.record_ids=ids;fs.record_id_capacity=64;
    fs.dev=&device;fs.device_id=device.id;
    make_record(0,1,MGFS_RECORD_DIRECTORY,0,0,0,0);
    make_record(1,2,MGFS_RECORD_FILE,4096,1,1,0);
    set_extent(record_at(1)+64,0,5,1,MGFS_EXTENT_DATA);
}
static void target_fragmented(void){
    make_record(1,2,MGFS_RECORD_FILE,12288,3,2,20);
    set_extent(record_at(1)+64,0,5,1,MGFS_EXTENT_DATA);
    set_extent(record_at(1)+96,1,7,1,MGFS_EXTENT_DATA);
    make_list(20,2,2,9,1,MGFS_EXTENT_DATA);
}
static void other_file(u64 physical){
    make_record(2,3,MGFS_RECORD_FILE,4096,1,1,0);
    set_extent(record_at(2)+64,0,physical,1,MGFS_EXTENT_DATA);
}
static void check_sealed(mgfs_ownership_result_t expected){
    memcpy(before,disk,sizeof(disk));
    mgfs_ownership_result_t actual=mgfs_ownership_preflight(&fs,2);
    assert(actual==expected);
    assert(fs.record_lifecycle_uncertain==(expected==MGFS_OWNERSHIP_CORRUPT));
    assert(!live&&!memcmp(before,disk,sizeof(disk)));
}
static void check(mgfs_ownership_result_t expected){seal();check_sealed(expected);}
int main(void){
    seed();check(MGFS_OWNERSHIP_VALID);
    seed();target_fragmented();check(MGFS_OWNERSHIP_VALID);
    seed();make_record(1,2,MGFS_RECORD_FILE,0,0,0,0);check(MGFS_OWNERSHIP_VALID);
    seed();make_record(1,2,MGFS_RECORD_FILE,3,0,0,0);
    put(record_at(1)+8,mgfs_get_le64(record_at(1)+8)|MGFS_RECORD_INLINE_DATA);
    memcpy(record_at(1)+128,"abc",3);check(MGFS_OWNERSHIP_VALID);
    seed();other_file(5);check(MGFS_OWNERSHIP_CORRUPT);
    seed();make_record(2,3,MGFS_RECORD_DIRECTORY,4096,1,1,0);
    set_extent(record_at(2)+64,0,5,1,MGFS_EXTENT_DIRECTORY_METADATA);
    check(MGFS_OWNERSHIP_CORRUPT); /* directory data versus target data */
    seed();target_fragmented();make_record(2,3,MGFS_RECORD_FILE,12288,3,2,20);
    set_extent(record_at(2)+64,0,11,1,MGFS_EXTENT_DATA);
    set_extent(record_at(2)+96,1,12,1,MGFS_EXTENT_DATA);
    check(MGFS_OWNERSHIP_CORRUPT); /* shared list block */
    seed();target_fragmented();other_file(20);check(MGFS_OWNERSHIP_CORRUPT);
    seed();target_fragmented();make_record(2,3,MGFS_RECORD_FILE,12288,3,2,5);
    set_extent(record_at(2)+64,0,11,1,MGFS_EXTENT_DATA);
    set_extent(record_at(2)+96,1,12,1,MGFS_EXTENT_DATA);
    make_list(5,3,2,13,1,MGFS_EXTENT_DATA);
    check(MGFS_OWNERSHIP_CORRUPT); /* other list versus target data */
    seed();target_fragmented();set_extent(disk[20]+64,2,20,1,MGFS_EXTENT_DATA);
    mgfs_recompute_checksum(disk[20],32,4096);
    check(MGFS_OWNERSHIP_CORRUPT); /* target list versus its own data */
    seed();target_fragmented();set_extent(record_at(1)+96,1,5,1,MGFS_EXTENT_DATA);
    check(MGFS_OWNERSHIP_CORRUPT); /* target data versus data */
    seed();disk[1][24+1/8]&=(u8)~(1U<<(1%8));check(MGFS_OWNERSHIP_CORRUPT);
    seed();make_record(1,2,MGFS_RECORD_FILE,0,0,0,0);other_file(8);
    disk[1][24]&=(u8)~(1U<<(8-4));check(MGFS_OWNERSHIP_CORRUPT);
    seed();make_record(2,3,MGFS_RECORD_FILE,12288,3,2,21);
    set_extent(record_at(2)+64,0,11,1,MGFS_EXTENT_DATA);
    set_extent(record_at(2)+96,1,12,1,MGFS_EXTENT_DATA);
    make_list(21,3,2,13,1,MGFS_EXTENT_DATA);
    disk[1][24+(21-4)/8]&=(u8)~(1U<<((21-4)%8));
    check(MGFS_OWNERSHIP_CORRUPT); /* unrelated extent-list block marked free */
    seed();set_extent(record_at(1)+64,0,32,1,MGFS_EXTENT_DATA);
    check(MGFS_OWNERSHIP_CORRUPT);
    seed();put(record_at(1)+64,1);check(MGFS_OWNERSHIP_CORRUPT);
    seed();put(record_at(1)+40,0);put(record_at(1)+48,0);
    check(MGFS_OWNERSHIP_CORRUPT); /* no coverage */
    seed();target_fragmented();disk[20][80]^=1;
    check(MGFS_OWNERSHIP_CORRUPT); /* list checksum */
    seed();target_fragmented();put(record_at(1)+56,32);
    check(MGFS_OWNERSHIP_CORRUPT); /* list address outside data area */
    seed();other_file(11);put(record_at(2)+64,1);
    check(MGFS_OWNERSHIP_CORRUPT); /* malformed unrelated mapping */
    seed();other_file(11);put(record_at(2)+40,UINT64_MAX);
    check(MGFS_OWNERSHIP_CORRUPT); /* impossible chain length is bounded */
    seed();seal();disk[1][100]^=1;
    check_sealed(MGFS_OWNERSHIP_CORRUPT); /* bitmap checksum */
    seed();target_fragmented();other_file(11);seal();
    unsigned total_reads;
    assert(mgfs_ownership_preflight(&fs,2)==MGFS_OWNERSHIP_VALID);
    total_reads=reads;assert(!live);
    for(unsigned n=1;n<=total_reads;n++){
        seed();target_fragmented();other_file(11);seal();fail_read=n;
        check(MGFS_OWNERSHIP_IO);assert(!fs.record_lifecycle_uncertain);
    }
    for(unsigned n=1;n<=2;n++){
        seed();target_fragmented();seal();fail_alloc=n;
        check(MGFS_OWNERSHIP_NO_MEM);assert(!fs.record_lifecycle_uncertain);
    }
    seed();seal();assert(mgfs_ownership_preflight(&fs,99)==MGFS_OWNERSHIP_NOT_FOUND);
    assert(!reads&&!live&&!fs.record_lifecycle_uncertain);
    puts("MGFS ownership preflight: production record, list, bitmap and checksum checks passed");
}
'''
with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-ownership-") as directory:
    path = Path(directory) / "test.c"
    binary = path.with_suffix("")
    path.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "cc", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
