#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production read functions with mocked devices, DMA and allocation."""
from pathlib import Path
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
work = tempfile.TemporaryDirectory(prefix="mangrove-storage-")
TMP = Path(work.name)
import subprocess
block=(ROOT / 'kernel/src/block.c').read_text()
mgfs=(ROOT / 'kernel/src/storage/mgfs.c').read_text()
def function(source,name):
    import re
    match=re.search(r'^(?:static )?[^\n]+\b'+name+r'\s*\(',source,re.M)
    assert match,name
    start=match.start(); brace=source.index('{',match.end()); depth=1; end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]+'\n'
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8; typedef size_t usize;
#define BLOCK_CACHE_ENTRY_BYTES 4096U
#define BLOCK_CACHE_ENTRY_COUNT 64U
#define MGFS_BLOCK_BYTES 4096U
#define MGFS_EXTENT_DATA 1ULL
#define MGFS_U64_MAX UINT64_MAX
struct block_device; typedef struct block_device block_device_t;
struct block_device { u64 id; u32 sector_size; u64 sector_count; bool read_only; bool live; bool (*read)(block_device_t*,u64,u32,void*); bool (*write)(block_device_t*,u64,u32,const void*); };
typedef struct { u64 storage_id; u64 lba; u32 sector_size; u32 sector_count,byte_count; u64 age; bool valid; u8 data[4096]; } block_cache_entry_t;
typedef struct {u64 storage_id,first_lba,last_lba;u32 sector_size;bool read_only;} block_physical_range_t;
static bool block_device_physical_range(block_device_t*d,u64 id,block_physical_range_t*out){if(!d->live||d->id!=id)return false;*out=(block_physical_range_t){id,0,d->sector_count-1,d->sector_size,d->read_only};return true;}
static block_cache_entry_t block_cache[64]; static u64 block_cache_age;
static struct {u64 read_requests,device_reads,cache_hits,cache_misses,write_requests;} block_stats;
static bool block_device_is_live(block_device_t *d) {return d->live;}
static block_device_t *test_device;
static bool block_capture_live_id(block_device_t*d,u64*id){*id=d->id;return d->live;}
static bool block_device_id_is_live(u64 id){return test_device->id==id && test_device->live;}
static bool block_device_matches_live_id(block_device_t*d,u64 id){return d==test_device&&d->id==id&&d->live;}
static bool block_io_acquire(void) {return true;} static void block_io_release(void) {}
static void block_callback_enter(void) {} static void block_callback_leave(void) {}
static u8 backing[1024*512]; static unsigned reads; static u32 last_sectors;static bool fail_prefetch,fail_single;
static bool backend_read(block_device_t*d,u64 l,u32 n,void*b) {(void)d; assert(l+n<=1024); reads++;last_sectors=n;if((n>8&&fail_prefetch)||(n==8&&fail_single))return false;memcpy(b,backing+l*512,n*512);return true;}
static bool backend_write(block_device_t*d,u64 l,u32 n,const void*b) {(void)d;memcpy(backing+l*512,b,n*512);return true;}
typedef struct {block_device_t *dev; u64 device_id; struct {u64 data_start,data_blocks,record_count;} layout;} mgfs_fs_t;
static u64 mgfs_get_le64(const u8*p) {u64 n;memcpy(&n,p,8);return n;}
static void mgfs_set_error(const char*s) {(void)s;}
static bool fail_alloc;
static void *kmalloc(usize n) {return fail_alloc?NULL:malloc(n);} static void kfree(void*p){free(p);}
'''
source=pre
for name in ['block_cache_range_overlaps','block_cache_request_size','block_cache_find','block_cache_victim','block_read_internal','block_read','block_read_for_id','block_write_internal','block_write']:
    source+=function(block,name)
source+='static bool mgfs_read_block(block_device_t*d,u64 id,u64 b,u8*out){return block_read_for_id(d,id,b*8,8,out);}\n'
source+=function(mgfs,'mgfs_validate_file_extent')
source+=r'''
static bool get(mgfs_fs_t *fs,u8 *extent,u64 index,u8 *out) {u64 logical=0,physical=0,count=0;bool found=false;return mgfs_validate_file_extent(fs,extent,&logical,&physical,&count,index,out,&found)&&found;}
int main(void) {
 for(usize i=0;i<sizeof(backing);i++)backing[i]=(u8)(i/512);
 block_device_t d={0,512,1024,false,true,backend_read,backend_write};test_device=&d;
 u8 big[32768],small[4096],patch[512];
 assert(block_read(&d,80,64,big)&&reads==1);
 for(u32 i=0;i<8;i++){assert(block_read(&d,80+i*8,8,small));assert(!memcmp(small,backing+(80+i*8)*512,4096));}
 assert(reads==1);assert(block_read(&d,83,2,small)&&!memcmp(small,backing+83*512,1024)&&reads==1);
 assert(block_read(&d,80,64,big)&&reads==1);
 memset(patch,0xa5,sizeof(patch));assert(block_write(&d,83,1,patch));assert(block_read(&d,80,8,small)&&reads==2);assert(!memcmp(small+1536,patch,512));
 memset(block_cache,0,sizeof(block_cache));reads=0;
 mgfs_fs_t fs={&d,0,{1,100,100}};u64 values[4]={0,20,10,MGFS_EXTENT_DATA};u8 *extent=(u8*)values;
 for(u64 i=0;i<10;i++){assert(get(&fs,extent,i,small));assert(!memcmp(small,backing+(20+i)*4096,4096));}
 assert(reads==2&&last_sectors==16);
 assert(get(&fs,extent,0,small)&&reads==2);
 memset(block_cache,0,sizeof(block_cache));fail_alloc=true;assert(get(&fs,extent,3,small));assert(last_sectors==8&&!memcmp(small,backing+23*4096,4096));
 fail_alloc=false;memset(block_cache,0,sizeof(block_cache));fail_prefetch=true;unsigned before=reads;
 assert(get(&fs,extent,3,small)&&reads==before+2&&last_sectors==8);assert(!memcmp(small,backing+23*4096,4096));
 memset(block_cache,0,sizeof(block_cache));fail_single=true;assert(!get(&fs,extent,3,small));fail_single=false;fail_prefetch=false;
 memset(block_cache,0,sizeof(block_cache));assert(block_read(&d,80,8,small));before=reads;assert(block_read(&d,80,64,big)&&reads==before+1); // partial cache miss refreshes complete request
 values[2]=101;assert(!get(&fs,extent,0,small));
 d.live=false;assert(!block_read(&d,80,8,small));
 puts("PASS: batched reads, cached subranges, write invalidation, partial extent groups, allocation/prefetch fallback, requested-read failure, partial cache misses, invalid extents, device removal");
}
'''
(TMP / 'mangrove_read_ahead_test.c').write_text(source)
subprocess.run([CC,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(TMP / 'mangrove_read_ahead_test.c'),'-o',str(TMP / 'mangrove_read_ahead_test')],check=True)
subprocess.run([str(TMP / 'mangrove_read_ahead_test')],check=True)
