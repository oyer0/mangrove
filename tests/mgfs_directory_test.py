#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production tombstone/checksum code with simulated blocks and injected failures.
Not a disk transaction or power-loss test. Optional second argument selects a
baseline source tree for reproducing the original sanitizer failure.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/storage/mgfs.c').read_text()

def function(name):
    # The admission wrapper is a dependency of this focused body fixture;
    # mgfs_mount_ownership_test.py exercises the complete production boundary.
    if name == 'mgfs_mutation_unavailable':
        return 'static bool mgfs_mutation_unavailable(const mgfs_fs_t *f){return f->record_lifecycle_uncertain || f->stage_transaction != NULL;}\n'
    original = name
    if re.search(r'^static [^\n;{}]*\b'+name+r'_impl\s*\(', source, re.M):
        name += '_impl'
    match = re.search(r'^static [^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    start = source.index('{', match.end())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end].replace(name+'(', original+'(', 1).replace('mgfs_write_mounted_block(fs,', 'mgfs_write_block(fs->dev, fs->device_id,') + '\n'

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize;
#define MGFS_BLOCK_BYTES 4096U
#define MGFS_RECORD_BYTES 192U
#define MGFS_DIRENT_TOMBSTONE 2ULL
#define MGFS_CRC64_POLYNOMIAL 0x42F0E1EBA9EA3693ULL
typedef struct {void *dev;u64 device_id;bool record_lifecycle_uncertain;} mgfs_fs_t;
static u8 disk[2][4096], before[2][4096];
static unsigned reads,writes,allocs,live,fail_read,fail_write,fail_map;
static bool fail_alloc,torn,alias;
static void mgfs_set_error(const char *s){(void)s;}
static void *kmalloc(usize n){allocs++;if(fail_alloc)return NULL;void *p=malloc(n);assert(p);live++;return p;}
static void kfree(void *p){if(p){assert(live);live--;free(p);}}
static u64 mgfs_get_le64(const u8 *p){u64 n=0;for(unsigned i=0;i<8;i++)n|=(u64)p[i]<<(i*8);return n;}
static bool mgfs_find_directory_block(mgfs_fs_t *f,const u8 *r,u64 n,u64 *p){
 (void)f;(void)r;assert(n<2);if(fail_map==n+1)return false;*p=n==0||alias?7:19;return true;}
static unsigned index_of(u64 p){assert(p==7||p==19);return p==7?0:1;}
static bool mgfs_read_block(void *d,u64 id,u64 p,u8 *out){(void)d;(void)id;reads++;if(reads==fail_read)return false;memcpy(out,disk[index_of(p)],4096);return true;}
static bool mgfs_write_block(void *d,u64 id,u64 p,const u8 *in){(void)d;(void)id;writes++;if(writes==fail_write){if(torn)memcpy(disk[index_of(p)],in,4093);return false;}memcpy(disk[index_of(p)],in,4096);return true;}
'''
for name in ['mgfs_crc64', 'mgfs_store_le64', 'mgfs_recompute_checksum',
             'mgfs_checksum_block', 'mgfs_publish_tombstone']:
    code += function(name)
code += r'''
static u8 record[192],original_entry[288];
static mgfs_fs_t fs;
static void seed(usize offset,usize length){
 assert(!live);fs.record_lifecycle_uncertain=false;reads=writes=allocs=fail_read=fail_write=fail_map=0;
 fail_alloc=torn=alias=false;
 for(unsigned b=0;b<2;b++)for(unsigned i=0;i<4096;i++)disk[b][i]=(u8)(i*17+b*31);
 memset(record,0,sizeof(record));mgfs_store_le64(record+32,8192);
 memset(original_entry,0,sizeof(original_entry));mgfs_store_le64(original_entry,42);
 mgfs_store_le64(original_entry+8,length-32);mgfs_store_le64(original_entry+16,1);
 memset(original_entry+32,'a',length-32);mgfs_recompute_checksum(original_entry,24,length);
 for(usize i=0;i<length;i++)disk[(offset+i)/4096][(offset+i)%4096]=original_entry[i];
 memcpy(before,disk,sizeof(disk));
}
static void surrounding_unchanged(usize offset,usize length){
 for(usize i=0;i<8192;i++)if(i<offset||i>=offset+length)
  assert(disk[i/4096][i%4096]==before[i/4096][i%4096]);
}
int main(void){
 (void)mgfs_set_error; (void)kmalloc; (void)kfree;
 /* Every legal entry length, every aligned split, and exact boundaries. */
 for(usize length=40;length<=288;length+=8){
  for(usize tail=8;tail<=length+8;tail+=8){
   usize offset=4096-tail;seed(offset,length);
   assert(mgfs_publish_tombstone(&fs,record,offset,length));assert(!live);
   bool cross=length>tail;assert(reads==(cross?2U:1U)&&writes==(cross?2U:1U)&&allocs==(cross?1U:0U));
   u8 entry[288];for(usize i=0;i<length;i++)entry[i]=disk[(offset+i)/4096][(offset+i)%4096];
   assert(mgfs_get_le64(entry)==42&&mgfs_get_le64(entry+8)==length-32);
   assert(mgfs_get_le64(entry+16)==2&&mgfs_checksum_block(entry,24,length));
   assert(!memcmp(entry+32,original_entry+32,length-32));surrounding_unchanged(offset,length);
   assert(mgfs_publish_tombstone(&fs,record,offset,length)&&!live);
  }
 }
 for(unsigned failure=1;failure<=2;failure++){
  seed(4064,48);fail_read=failure;assert(!mgfs_publish_tombstone(&fs,record,4064,48));assert(!writes&&!live&&!fs.record_lifecycle_uncertain&&!memcmp(before,disk,sizeof(disk)));
  seed(4064,48);fail_map=failure;assert(!mgfs_publish_tombstone(&fs,record,4064,48));assert(!writes&&!live&&!fs.record_lifecycle_uncertain&&!memcmp(before,disk,sizeof(disk)));
  for(unsigned partial=0;partial<2;partial++){
   seed(4064,48);fail_write=failure;torn=partial;
   assert(!mgfs_publish_tombstone(&fs,record,4064,48));assert(writes==failure&&!live&&fs.record_lifecycle_uncertain);surrounding_unchanged(4064,48);
   if(failure==1&&!partial)assert(!memcmp(before,disk,sizeof(disk)));
   if(failure==2)assert(memcmp(before[0],disk[0],4096));
  }
 }
 for(unsigned partial=0;partial<2;partial++){
  seed(4000,48);fail_write=1;torn=partial;
  assert(!mgfs_publish_tombstone(&fs,record,4000,48));assert(fs.record_lifecycle_uncertain&&writes==1&&!live);surrounding_unchanged(4000,48);
 }
 seed(4064,48);fail_alloc=true;assert(!mgfs_publish_tombstone(&fs,record,4064,48));assert(!writes&&!live&&!fs.record_lifecycle_uncertain&&!memcmp(before,disk,sizeof(disk)));
 seed(4064,48);alias=true;assert(!mgfs_publish_tombstone(&fs,record,4064,48));assert(!writes&&!live);
 const u64 invalid[][2]={{0,0},{0,32},{0,296},{1,48},{0,41},{UINT64_MAX-7,48},{8192,40},{8184,40}};
 for(usize i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  seed(4064,48);assert(!mgfs_publish_tombstone(&fs,record,invalid[i][0],invalid[i][1]));assert(!reads&&!writes&&!allocs);
 }
 puts("MGFS tombstones: all aligned boundary splits, checksums, neighbouring data, repeated updates, bounds and allocation/map/read/torn-write failures passed (simulated block I/O)");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-directory-') as directory:
    path = Path(directory) / 'test.c'
    binary = path.with_suffix('')
    path.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'cc', '-std=c11',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
