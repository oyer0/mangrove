#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real MGFS write/truncate/merge control flow with fault-injected block I/O.
Record checksums, allocator and extent discovery are fixtures, not a disk image.
This tests cleanup and uncertain outcomes, not crash recovery.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source_root=Path(sys.argv[2]) if len(sys.argv)>2 else ROOT
source=(source_root/'kernel/src/storage/mgfs.c').read_text()
def function(name):
    match=next(m for m in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b'+name+r'\s*\(',source,re.M) if ';' not in source[m.end():source.index('{',m.end())])
    brace=source.index('{',match.end());end,depth=brace+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]+'\n'
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;typedef size_t usize;
#define MGFS_RECORD_BYTES 192U
#define MGFS_BLOCK_BYTES 4096U
#define MGFS_INLINE_DATA_BYTES 56U
#define MGFS_RECORD_CHECKSUM_OFFSET 184U
#define MGFS_RECORD_FILE 1U
#define MGFS_RECORD_INLINE_DATA 1U
#define MGFS_EXTENT_DATA 1U
#define MGFS_STATE_NEEDS_FSCK 2U
#define MGFS_STATE_CLEAN 1U
#define MGFS_U64_MAX UINT64_MAX
#define VFS_CHILD_MUTATION_OPEN 0U
#define VFS_TYPE_FILE 1U
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM -1
#define VFS_ERR_IO -3
#define VFS_ERR_NO_MEM -4
#define VFS_ERR_NO_SPACE -12
#define VFS_ERR_ACCESS_DENIED -8
#define VFS_ERR_UNCERTAIN -14
#define VFS_ERR_BUFFER_TOO_SMALL -15
#define MG_FILE_LINES_MAX 4096U
#define MG_FILE_LINE_CAPACITY 512U
#define MG_FILE_CONTENT_MAX_BYTES (2U * 1024U * 1024U)
#define MG_FILE_LINES_RESET 1U
typedef struct {u64 identifier,revision;usize length,append_offset;} mg_file_lines_state_t;
typedef int (*vfs_snapshot_sink_t)(void *,u64,const u8 *,usize);
typedef struct {u64 logical_start,physical_start,block_count,flags;} mgfs_extent_desc_t;
typedef struct {void*dev;u64 device_id;struct {u64 record_count;} layout;bool record_lifecycle_uncertain;void*stage_transaction;} mgfs_fs_t;
typedef struct {void*private_data;bool read_only;} vfs_super_t;
struct node;typedef struct node vfs_node_t;
typedef struct {int (*truncate)(vfs_node_t*);} vfs_ops_t;
struct node {u32 type;vfs_super_t*super;u64 inode,size;vfs_ops_t*ops;};
static void vfs_nodes_update_size(vfs_super_t*sb,u64 inode,u64 size){(void)sb;(void)inode;(void)size;}
static u8 record[192],blocks[513][4096];static unsigned next_block,io_count,fail_io,alloc_count,fail_alloc,live_allocs;static bool committed;static mgfs_fs_t fs;
static bool io(void){return ++io_count!=fail_io;}
static void*kmalloc(usize n){if(++alloc_count==fail_alloc)return NULL;void*p=malloc(n);assert(p);live_allocs++;return p;}
static void kfree(void*p){if(p){assert(live_allocs);live_allocs--;free(p);}}
static void mgfs_set_error(const char*s){(void)s;}
static u64 mgfs_get_le64(const u8*p){u64 n;memcpy(&n,p,8);return n;}
static void mgfs_store_le64(u8*p,u64 n){memcpy(p,&n,8);}
static bool mgfs_read_record(mgfs_fs_t*fs,u64 id,u8*out){(void)fs;(void)id;if(!io())return false;memcpy(out,record,192);return true;}
static bool mgfs_validate_regular_file_record(const u8*r){return mgfs_get_le64(r)==1;}
static u32 mgfs_record_owner(const u8*r){(void)r;return 1;}
static u32 mgfs_record_permissions(const u8*r){(void)r;return 3;}
static u64 mgfs_security_flags(u32 uid,u32 permissions,bool in,u32 policy){(void)uid;(void)permissions;(void)policy;return in?1:0;}
static void mgfs_recompute_checksum(u8*p,u32 offset,usize n){(void)p;(void)offset;(void)n;}
static bool mgfs_collect_file_extents(mgfs_fs_t*fs,const u8*r,mgfs_extent_desc_t*out,u64 cap,u64*count,u64 flags){(void)fs;(void)cap;(void)flags;if(!io())return false;*count=0;if(!(mgfs_get_le64(r+8)&1)){out[0]=(mgfs_extent_desc_t){0,1,(mgfs_get_le64(r+32)+4095)/4096,1};*count=out[0].block_count?1:0;}return true;}
static bool mgfs_collect_extent_list_blocks(mgfs_fs_t*fs,const u8*r,u64*out,u64 cap,u64*count){(void)fs;(void)r;(void)out;(void)cap;if(!io())return false;*count=0;return true;}
static bool mgfs_set_state(mgfs_fs_t*fs,u64 state){(void)state;return !fs->record_lifecycle_uncertain && io();}
static bool mgfs_find_record_slot(mgfs_fs_t*fs,u64 id,u64*out){(void)fs;(void)id;*out=0;return io();}
static bool mgfs_write_record_slot(mgfs_fs_t*fs,u64 slot,const u8*r){(void)fs;(void)slot;if(!io())return false;memcpy(record,r,192);committed=true;return true;}
static bool mgfs_reclaim_obsolete_blocks(mgfs_fs_t*fs,const mgfs_extent_desc_t*a,u64 ac,const u64*b,u64 bc,const mgfs_extent_desc_t*c,u64 cc,u64 head){(void)fs;(void)a;(void)ac;(void)b;(void)bc;(void)c;(void)cc;(void)head;return io();}
static bool mgfs_allocate_data_block(mgfs_fs_t*fs,u64*out){(void)fs;if(!io())return false;*out=next_block++;assert(*out<32);return true;}
static bool mgfs_read_block(void*dev,u64 id,u64 block,u8*out){(void)dev;(void)id;if(!io())return false;assert(block<32);memcpy(out,blocks[block],4096);return true;}
static bool mgfs_write_block(void*dev,u64 id,u64 block,const u8*data){(void)dev;(void)id;if(!io())return false;assert(block<32);memcpy(blocks[block],data,4096);return true;}
static bool mgfs_read_file_block(mgfs_fs_t*fs,const u8*r,u64 logical,u8*out){
 (void)fs;(void)r;if(!io()||logical>=512)return false;
 memcpy(out,blocks[logical+1],4096);return true;
}
static void mgfs_store_extent(u8*r,u64 off,u64 logical,u64 physical,u64 count,u64 flags){mgfs_store_le64(r+off,logical);mgfs_store_le64(r+off+8,physical);mgfs_store_le64(r+off+16,count);mgfs_store_le64(r+off+24,flags);}
static bool mgfs_write_extent_list(mgfs_fs_t*fs,u64 id,u64 block,mgfs_extent_desc_t*ex,u64 count,u64 flags){(void)fs;(void)id;(void)block;(void)ex;(void)count;(void)flags;return io();}
static u64 mgfs_read(vfs_node_t*n,u64 off,u64 length,void*out){(void)n;if(!io()||off+length>mgfs_get_le64(record+32))return 0;if(mgfs_get_le64(record+8)&1)memcpy(out,record+128+off,length);else for(u64 i=0;i<length;i++)((u8*)out)[i]=blocks[1+(off+i)/4096][(off+i)%4096];return length;}
'''
for name in ['mgfs_mutation_unavailable','mgfs_write_file_data','mgfs_write','mgfs_truncate','mgfs_append_lines','mgfs_snapshot','mgfs_snapshot_stream']:
    pre+=function(name)
pre+=r'''
static usize received;
static int stream_sink(void *context,u64 offset,const u8 *data,usize size){
 char *out=context;assert(offset==received);memcpy(out+offset,data,size);received+=size;return VFS_OK;
}
static int fail_sink(void *context,u64 offset,const u8 *data,usize size){
 (void)context;(void)offset;(void)data;(void)size;return VFS_ERR_INVALID_PARAM;
}
static void seed(bool big){assert(!live_allocs);fs.record_lifecycle_uncertain=false;memset(record,0,192);memset(blocks,0,sizeof(blocks));mgfs_store_le64(record,1);mgfs_store_le64(record+8,big?0:1);mgfs_store_le64(record+16,1);mgfs_store_le64(record+24,7);mgfs_store_le64(record+32,big?8192:4);if(big){memset(blocks[1],'a',4096);memset(blocks[2],'b',4096);next_block=3;}else{memcpy(record+128,"old\n",4);next_block=1;}io_count=alloc_count=fail_io=fail_alloc=0;committed=false;}
int main(void){
 fs.layout.record_count=32;vfs_super_t super={.private_data=&fs};vfs_ops_t ops={mgfs_truncate};vfs_node_t node={1,&super,1,0,&ops};char data[6000];memset(data,'x',sizeof(data));
 for(unsigned big=0;big<2;big++)for(unsigned large=0;large<2;large++){
  seed(big);usize len=large?sizeof(data):8;assert(mgfs_write(&node,0,len,data)==len);unsigned calls=io_count;
  assert(!live_allocs&&mgfs_get_le64(record+32)==len);char out[6000];mg_file_lines_state_t state;
  assert(!mgfs_snapshot(&node,out,sizeof(out),&state)&&state.length==len&&!memcmp(out,data,len));
  received=0;
  assert(!mgfs_snapshot_stream(&node,sizeof(out),stream_sink,out,&state));
  assert(received==len && !memcmp(out,data,len));
  assert(mgfs_snapshot_stream(&node,sizeof(out),fail_sink,out,&state)==VFS_ERR_INVALID_PARAM);
  bool after_commit=false;
  for(unsigned failure=1;failure<=calls;failure++){
   seed(big);fail_io=failure;assert(!mgfs_write(&node,0,len,data));assert(!live_allocs);after_commit|=committed;
   if(committed || next_block>(big?3U:1U))assert(fs.record_lifecycle_uncertain);
  }
  assert(after_commit); // zero return is possible after the record changed
  for(unsigned failure=1;failure<=3;failure++){
   seed(big);fail_alloc=failure;u64 result=mgfs_write(&node,0,len,data);assert(!live_allocs);assert(result==0||result==len);
  }
 }
 seed(false);assert(!mgfs_append_lines(&node,"new\n",4,10,0,NULL));assert(mgfs_get_le64(record+32)==8);
 seed(false);assert(!mgfs_append_lines(&node,"new\n",4,10,0,NULL));unsigned calls=io_count;bool uncertain_after_commit=false;
 for(unsigned failure=1;failure<=calls;failure++){
  seed(false);fail_io=failure;int result=mgfs_append_lines(&node,"new\n",4,10,0,NULL);assert(result!=VFS_OK&& !live_allocs);
  if(committed){assert(result==VFS_ERR_UNCERTAIN);uncertain_after_commit=true;}
 }
 assert(uncertain_after_commit);
 seed(true);assert(!mgfs_truncate(&node));calls=io_count;assert(mgfs_get_le64(record+32)==0&&!live_allocs);
 for(unsigned failure=1;failure<=calls;failure++){seed(true);fail_io=failure;assert(mgfs_truncate(&node)!=VFS_OK&&!live_allocs);if(committed)assert(fs.record_lifecycle_uncertain);}
 mgfs_extent_desc_t extent={0,1,2,1};
 for(unsigned failure=1;failure<=4;failure++){
  seed(true);fail_io=failure;
  assert(!mgfs_write_file_data(&fs,&extent,1,8192,0,(const u8*)data,sizeof(data),sizeof(data)));
  assert(fs.record_lifecycle_uncertain==(failure>=2));
 }
 // Failed second-block read after the first write must block all retries,
 // even though the persistent Record generation has not advanced.
 seed(true);assert(mgfs_write(&node,0,sizeof(data),data)==sizeof(data));calls=io_count;
 bool unchanged_generation=false;
 for(unsigned failure=1;failure<=calls;failure++){
  seed(true);fail_io=failure;assert(!mgfs_write(&node,0,sizeof(data),data));
  if(blocks[1][0]=='x' && mgfs_get_le64(record+24)==7){
   unchanged_generation=true;assert(fs.record_lifecycle_uncertain);
   unsigned before=io_count;fail_io=0;assert(!mgfs_write(&node,0,8,data));
   assert(mgfs_truncate(&node)==VFS_ERR_UNCERTAIN && io_count==before);
  }
  if(committed || next_block>3 || blocks[1][0]=='x')assert(fs.record_lifecycle_uncertain);
 }
 assert(unchanged_generation);
 seed(true);fail_io=1;assert(!mgfs_write(&node,0,sizeof(data),data));assert(!fs.record_lifecycle_uncertain);
 seed(true);fail_alloc=1;assert(!mgfs_write(&node,0,sizeof(data),data));assert(!fs.record_lifecycle_uncertain);
 seed(true);mgfs_store_le64(record+32,MG_FILE_CONTENT_MAX_BYTES);
 for(usize i=1;i<=512;i++)memset(blocks[i],(int)(i%251),4096);
 char *whole=malloc(MG_FILE_CONTENT_MAX_BYTES);assert(whole);received=0;
 mg_file_lines_state_t large_state={0};
 assert(mgfs_snapshot_stream(&node,MG_FILE_CONTENT_MAX_BYTES,stream_sink,whole,&large_state)==VFS_OK);
 assert(received==MG_FILE_CONTENT_MAX_BYTES&&large_state.length==received);
 assert((u8)whole[0]==1&&(u8)whole[MG_FILE_CONTENT_MAX_BYTES-1]==512%251);
 assert(mgfs_snapshot_stream(&node,MG_FILE_CONTENT_MAX_BYTES-1,stream_sink,whole,&large_state)==VFS_ERR_BUFFER_TOO_SMALL);
 assert(mgfs_snapshot_stream(&node,MG_FILE_CONTENT_MAX_BYTES+1,stream_sink,whole,&large_state)==VFS_ERR_INVALID_PARAM);
 free(whole);
 puts("MGFS production writes: inline/extent writes, shrinking, snapshot bytes, injected I/O/allocation failures, post-commit uncertainty and cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-write-') as directory:
    path=Path(directory)/'test.c';binary=path.with_suffix('');path.write_text(pre)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
