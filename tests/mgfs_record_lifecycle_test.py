#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production MGFS ID index and create control flow with fault-injected fixtures.
Block layout/CRC, directory publication and VFS registration are fixtures here.
This is runtime failure testing, not power-loss recovery. Optional source root
and mode let the unchanged committed implementation reproduce either regression.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
mode = sys.argv[3] if len(sys.argv) > 3 else 'all'
source = (root / 'kernel/src/storage/mgfs.c').read_text()
def function(name):
    match = re.search(r'^static [^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    if not match: return ''
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[match.start():end] + '\n'

code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;typedef size_t usize;
#define MGFS_BLOCK_BYTES 4096U
#define MGFS_RECORD_BYTES 192U
#define MGFS_RECORD_CHECKSUM_OFFSET 184U
#define MGFS_SUPER_CHECKSUM_OFFSET 192U
#define MGFS_HEADER_BYTES 200U
#define MGFS_BITMAP_BITS_PER_BLOCK ((4096U-24U)*8U)
#define MGFS_BITMAP_HEADER_BYTES 24U
#define MGFS_METADATA_CHECKSUM_OFFSET 16U
#define MGFS_METADATA_RECORD_BITMAP 1U
#define MGFS_METADATA_ALLOCATION_BITMAP 2U
#define MGFS_METADATA_RECORD_TABLE 3U
#define MGFS_RECORDS_PER_TABLE_BLOCK 21U
#define MGFS_STATE_NEEDS_FSCK 2U
#define MGFS_STATE_CLEAN 1U
#define MGFS_U64_MAX UINT64_MAX
#define MGFS_RECORD_FILE 1U
#define MGFS_RECORD_DIRECTORY 2U
#define MGFS_DIRENT_IN_USE 1U
#define VFS_TYPE_DIRECTORY 2U
#define VFS_TYPE_FILE 1U
#define VFS_PERMISSION_KNOWN 15U
#define VFS_DEFAULT_DIRECTORY_PERMISSIONS 15U
#define VFS_DEFAULT_FILE_PERMISSIONS 7U
#define VFS_CHILD_MUTATION_OPEN 0U
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM -1
#define VFS_ERR_IO -3
#define VFS_ERR_NO_MEM -4
#define VFS_ERR_BAD_FORMAT -5
#define VFS_ERR_ACCESS_DENIED -6
#define VFS_ERR_UNCERTAIN -14
#define MGFS_EXTENT_DIRECTORY_METADATA 2U
#define MGFS_EXTENTS_PER_LIST_BLOCK 126U
typedef struct {u64 id,slot;} mgfs_record_ref_t;
typedef struct {u64 logical_start,physical_start,block_count,flags;} mgfs_extent_desc_t;
typedef struct {u64 record_count,record_bitmap_start,record_bitmap_blocks,record_table_start,record_table_blocks,data_start,data_blocks,allocation_bitmap_start,allocation_bitmap_blocks;} layout_t;
typedef struct {void *dev;u64 device_id;layout_t layout;u64 next_record_id;u8 *record_bitmap;mgfs_record_ref_t *record_ids;u64 record_id_capacity,record_id_count;bool record_lifecycle_uncertain;} mgfs_fs_t;
typedef struct {void *private_data;} vfs_super_t;
typedef struct {u32 type,owner_uid,permissions,ref_count;u64 inode;void *fs_data;vfs_super_t *super;const void *ops;} vfs_node_t;
static int mgfs_node_ops;
static const char *error="no error";
static u8 disk[16][4096],bitmap[4096];
static unsigned allocs,live,fail_alloc,reads,writes,fail_read,fail_write;
static bool torn,append_fail,append_uncertain,published,registered,append_real;
static void mgfs_set_error(const char *s){error=s;}
static const char *mgfs_last_error(void){return error;}
static void *kmalloc(usize n){if(++allocs==fail_alloc)return NULL;void *p=malloc(n);assert(p);live++;return p;}
static void kfree(void *p){if(p){assert(live);live--;free(p);}}
static u64 mgfs_get_le64(const u8 *p){u64 n;memcpy(&n,p,8);return n;}
static void mgfs_store_le64(u8 *p,u64 n){memcpy(p,&n,8);}
static bool mgfs_recompute_checksum(u8 *p,u32 o,usize n){(void)p;(void)o;(void)n;return true;}
static bool mgfs_read_block(void *d,u64 id,u64 n,u8 *p){(void)d;(void)id;assert(n<16);if(++reads==fail_read)return false;memcpy(p,disk[n],4096);return true;}
static bool mgfs_write_block(void *d,u64 id,u64 n,const u8 *p){(void)d;(void)id;assert(n<16);if(++writes==fail_write){if(torn)memcpy(disk[n],p,200);return false;}memcpy(disk[n],p,4096);return true;}
static bool mgfs_validate_metadata_header(const u8 *p,u64 type,u64 n){(void)p;(void)type;(void)n;return true;}
static bool mgfs_record_slot_allocated(const mgfs_fs_t *fs,u64 n){assert(n<fs->layout.record_count);return (fs->record_bitmap[24+n/8]&(1U<<(n%8)))!=0;}
static bool mgfs_valid_utf8_name(const u8 *p,u64 n){(void)p;return n>0&&n<=255;}
static bool vfs_current_uid(u32 *uid){*uid=9;return true;}
static u64 mgfs_security_flags(u32 uid,u32 perm,bool in,u32 policy){(void)in;(void)policy;return ((u64)uid<<8)|((u64)perm<<40);}
static bool mgfs_scan_directory(vfs_node_t *d,const char *n,u32 i,void *a,void *b){(void)d;(void)n;(void)i;(void)a;(void)b;return false;}
static void vfs_node_set_child_mutation_policy(vfs_node_t *n,u32 p){(void)n;(void)p;}
static void vfs_node_register(vfs_node_t *n){(void)n;registered=true;}
static u64 mgfs_ceil_div(u64 n,u64 d){return n/d+(n%d!=0);}
static void mgfs_dentry_cache_invalidate_parent(mgfs_fs_t *f,u64 id){(void)f;(void)id;}
static void mgfs_store_extent(u8 *r,u64 o,u64 l,u64 p,u64 n,u64 flags){mgfs_store_le64(r+o,l);mgfs_store_le64(r+o+8,p);mgfs_store_le64(r+o+16,n);mgfs_store_le64(r+o+24,flags);}
'''
# These helpers are production code; block contents and metadata CRCs are fixtures.
for name in ['mgfs_update_superblock','mgfs_set_state','mgfs_update_bitmap_bit',
             'mgfs_allocate_data_block','mgfs_allocate_record','mgfs_free_data_block',
             'mgfs_free_record_slot','mgfs_write_record_slot','mgfs_hash_id']:
    code += function(name)
# Original insert precedes find; corrected insert uses find. Forward declarations
# preserve the actual production implementations regardless of source order.
code += 'static bool mgfs_find_record_slot(const mgfs_fs_t *,u64,u64 *);\n'
code += function('mgfs_reserve_record_index') or 'static bool mgfs_reserve_record_index(mgfs_fs_t *f){(void)f;return true;}\n'
code += function('mgfs_insert_record_id') + function('mgfs_find_record_slot')
code += function('mgfs_remove_record_id') or 'static bool mgfs_remove_record_id(mgfs_fs_t *f,u64 id){(void)f;(void)id;return true;}\n'
code += function('mgfs_release_record') or 'static bool mgfs_release_record(mgfs_fs_t *f,u64 slot,u64 id){(void)id;return mgfs_free_record_slot(f,slot);}'
code += r'''
static bool mgfs_read_record(mgfs_fs_t *fs,u64 id,u8 *r){u64 slot;if(!mgfs_find_record_slot(fs,id,&slot))return false;memcpy(r,disk[2]+24+slot*192,192);return true;}
static bool mgfs_collect_file_extents(mgfs_fs_t *f,const u8 *r,mgfs_extent_desc_t *e,u64 cap,u64 *count,u64 flags){(void)f;(void)cap;(void)flags;*count=mgfs_get_le64(r+40);for(u64 i=0;i<*count;i++)e[i]=(mgfs_extent_desc_t){mgfs_get_le64(r+64+i*32),mgfs_get_le64(r+72+i*32),mgfs_get_le64(r+80+i*32),2};return true;}
static bool mgfs_find_directory_block(mgfs_fs_t *f,const u8 *r,u64 l,u64 *p){(void)f;for(u64 i=0;i<mgfs_get_le64(r+48);i++){u64 n=mgfs_get_le64(r+80+i*32);u64 base=mgfs_get_le64(r+64+i*32);if(l>=base&&l-base<n){*p=mgfs_get_le64(r+72+i*32)+l-base;return true;}}return false;}
static bool mgfs_write_extent_list(mgfs_fs_t *f,u64 id,u64 p,const mgfs_extent_desc_t *e,u64 count,u64 flags){(void)id;(void)e;(void)count;(void)flags;u8 data[4096]={0};if(!mgfs_write_block(f->dev,f->device_id,p,data)){f->record_lifecycle_uncertain=true;return false;}return true;}
'''
# Exercise real append failure/cleanup paths separately from the create fixtures.
append = function('mgfs_append_directory_entry').replace('mgfs_append_directory_entry(', 'mgfs_append_directory_entry_production(', 1)
code += append
code += r'''
static bool mgfs_append_directory_entry(mgfs_fs_t *fs,u64 id,const u8 *e,u64 n,u8 *out){
 if(append_real)return mgfs_append_directory_entry_production(fs,id,e,n,out);
 (void)id;(void)e;(void)n;(void)out;
 if(append_fail){fs->record_lifecycle_uncertain=append_uncertain;return false;}
 published=true;return true;
}
'''
code += function('mgfs_create_node')
code += r'''
static mgfs_fs_t fs;
static vfs_super_t super;
static vfs_node_t dir;
static void finish(void){kfree(fs.record_ids);assert(!live);}
static void seed(void){
 assert(!live);memset(&fs,0,sizeof(fs));memset(disk,0,sizeof(disk));memset(bitmap,0,sizeof(bitmap));
 reads=writes=allocs=fail_read=fail_write=fail_alloc=0;torn=append_fail=append_uncertain=published=registered=append_real=false;
 fs.layout=(layout_t){16,1,1,2,1,4,12,3,1};fs.next_record_id=2;fs.record_bitmap=bitmap;
 fs.record_id_capacity=16;fs.record_ids=kmalloc(16*sizeof(*fs.record_ids));memset(fs.record_ids,0,16*sizeof(*fs.record_ids));
 assert(mgfs_insert_record_id(&fs,1,0));bitmap[24]=disk[1][24]=1;
 u8 *r=disk[2]+24;mgfs_store_le64(r,2);mgfs_store_le64(r+16,1);mgfs_store_le64(r+24,1);
 super.private_data=&fs;dir=(vfs_node_t){.type=2,.fs_data=(void*)1,.super=&super};error="no error";
}
static unsigned occupied(void){unsigned n=0;for(unsigned i=0;i<16;i++)n+=mgfs_record_slot_allocated(&fs,i);return n;}
static void churn(void){
 seed();unsigned allocation_count=allocs;u64 slot;
 for(u64 id=2;id<40002;id++){
  assert(mgfs_insert_record_id(&fs,id,1));assert(mgfs_find_record_slot(&fs,id,&slot)&&slot==1);
  assert(mgfs_remove_record_id(&fs,id));assert(!mgfs_find_record_slot(&fs,id,&slot));
  assert(mgfs_find_record_slot(&fs,1,&slot)&&slot==0);
 }
 assert(allocs==allocation_count&&fs.record_id_capacity==16);finish();
 /* Growth, duplicate rejection, allocation failure and colliding wraparound clusters. */
 seed();for(u64 id=2;id<300;id++)assert(mgfs_insert_record_id(&fs,id,id));
 for(u64 id=1;id<300;id++)assert(mgfs_find_record_slot(&fs,id,&slot)&&slot==(id==1?0:id));
 assert(!mgfs_insert_record_id(&fs,42,3));
 for(u64 id=1;id<300;id+=2)assert(mgfs_remove_record_id(&fs,id));
 for(u64 id=1;id<300;id++){assert(mgfs_find_record_slot(&fs,id,&slot)==(id%2==0));}finish();
 seed();for(u64 id=2;id<=8;id++)assert(mgfs_insert_record_id(&fs,id,id));
 mgfs_record_ref_t *old=fs.record_ids;fail_alloc=allocs+1;assert(!mgfs_insert_record_id(&fs,9,9));assert(fs.record_ids==old&&fs.record_id_capacity==16);
 for(u64 id=1;id<=8;id++){assert(mgfs_find_record_slot(&fs,id,&slot));}finish();
 seed();u64 ids[7],found=0;for(u64 id=2;found<7;id++)if((mgfs_hash_id(id)&15)==15){ids[found++]=id;assert(mgfs_insert_record_id(&fs,id,id));}
 for(unsigned i=0;i<7;i++){assert(mgfs_remove_record_id(&fs,ids[i]));for(unsigned j=i+1;j<7;j++)assert(mgfs_find_record_slot(&fs,ids[j],&slot)&&slot==ids[j]);}finish();
}
static void cleanup(void){
 seed();append_fail=true;vfs_node_t *out=(void*)1;
 assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)!=VFS_OK);
 assert(occupied()==1&&!out&&!registered&&!fs.record_lifecycle_uncertain);u64 slot;assert(!mgfs_find_record_slot(&fs,2,&slot));
 assert(fs.next_record_id==3);finish();
 seed();fail_alloc=allocs+1;out=(void*)1;assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_ERR_NO_MEM);assert(!writes&&occupied()==1&&!out&&!published);finish();
 seed();for(u64 id=2;id<=8;id++)assert(mgfs_insert_record_id(&fs,id,id));fail_alloc=allocs+2;
 assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_ERR_NO_MEM);assert(!writes&&occupied()==1&&!out);finish();
 seed();assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_OK);unsigned read_count=reads,write_count=writes;assert(occupied()==2&&published&&registered&&out->inode==2);kfree(out);finish();
 for(unsigned n=1;n<=read_count;n++){
  seed();fail_read=n;int result=mgfs_create_node(&dir,"new",1,false,0,0,&out);assert(result!=VFS_OK&&!out&&!registered);
  if(!published)assert(occupied()==1);else assert(result==VFS_ERR_UNCERTAIN&&fs.record_lifecycle_uncertain&&occupied()==2);
  finish();
 }
 for(unsigned n=1;n<=write_count;n++)for(unsigned partial=0;partial<2;partial++){
  seed();fail_write=n;torn=partial;assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_ERR_UNCERTAIN);
  assert(!out&&!registered&&fs.record_lifecycle_uncertain);unsigned old_writes=writes;u64 next=fs.next_record_id;
  assert(mgfs_create_node(&dir,"again",1,false,0,0,&out)==VFS_ERR_UNCERTAIN);assert(writes==old_writes&&fs.next_record_id==next);
  assert(!mgfs_set_state(&fs,MGFS_STATE_CLEAN));finish();
 }
 seed();append_fail=append_uncertain=true;assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_ERR_UNCERTAIN);assert(occupied()==2&&fs.record_lifecycle_uncertain);finish();
 seed();append_fail=true;fail_write=4;assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_ERR_UNCERTAIN);assert(occupied()==2&&fs.record_lifecycle_uncertain);finish();
}
static void release_churn(void){
 seed();u64 previous=0;
 for(unsigned i=0;i<1000;i++){
  vfs_node_t *out;assert(!mgfs_create_node(&dir,"new",1,false,0,0,&out));
  u64 slot,id=out->inode;assert(id>previous&&mgfs_find_record_slot(&fs,id,&slot)&&slot==1);
  assert(mgfs_set_state(&fs,MGFS_STATE_NEEDS_FSCK));assert(mgfs_release_record(&fs,slot,id));
  assert(!mgfs_find_record_slot(&fs,id,&slot));assert(occupied()==1);
  assert(mgfs_set_state(&fs,MGFS_STATE_CLEAN));previous=id;kfree(out);
 }
 assert(fs.record_id_capacity==16&&fs.record_id_count==1);finish();
}
static void real_append(void){
 for(unsigned filled=0;filled<2;filled++){
  seed();append_real=true;
  u8 *parent=disk[2]+24;
  if(filled){mgfs_store_le64(parent+32,4096);mgfs_store_le64(parent+40,1);mgfs_store_le64(parent+48,1);mgfs_store_extent(parent,64,0,4,1,2);disk[3][24]=1;}
  vfs_node_t *out;assert(!mgfs_create_node(&dir,"new",1,false,0,0,&out));unsigned rc=reads,wc=writes;assert(occupied()==2);kfree(out);finish();
  for(unsigned n=1;n<=rc;n++){
   seed();append_real=true;parent=disk[2]+24;
   if(filled){mgfs_store_le64(parent+32,4096);mgfs_store_le64(parent+40,1);mgfs_store_le64(parent+48,1);mgfs_store_extent(parent,64,0,4,1,2);disk[3][24]=1;}
   fail_read=n;int result=mgfs_create_node(&dir,"new",1,false,0,0,&out);assert(result!=VFS_OK&&!out);
   if(!fs.record_lifecycle_uncertain){assert(occupied()==1);assert(disk[3][24]==(filled?1:0));}
   else assert(result==VFS_ERR_UNCERTAIN);
   finish();
  }
  for(unsigned n=1;n<=wc;n++)for(unsigned partial=0;partial<2;partial++){
   seed();append_real=true;parent=disk[2]+24;
   if(filled){mgfs_store_le64(parent+32,4096);mgfs_store_le64(parent+40,1);mgfs_store_le64(parent+48,1);mgfs_store_extent(parent,64,0,4,1,2);disk[3][24]=1;}
   fail_write=n;torn=partial;assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)==VFS_ERR_UNCERTAIN);assert(!out&&fs.record_lifecycle_uncertain);finish();
  }
 }
 seed();append_real=true;fail_alloc=allocs+2;vfs_node_t *out;
 assert(mgfs_create_node(&dir,"new",1,false,0,0,&out)!=VFS_OK);assert(!out&&occupied()==1&&!fs.record_lifecycle_uncertain);finish();
}
int main(int argc,char **argv){(void)mgfs_reserve_record_index;(void)mgfs_free_record_slot;(void)mgfs_release_record;assert(argc==2);
 if(!strcmp(argv[1],"exhaustion")){seed();for(u64 id=2;id<1000;id++){bitmap[24]|=2;disk[1][24]|=2;assert(mgfs_insert_record_id(&fs,id,1));assert(mgfs_release_record(&fs,1,id));}finish();}
 if(!strcmp(argv[1],"all")||!strcmp(argv[1],"index"))churn();
 if(!strcmp(argv[1],"all")||!strcmp(argv[1],"cleanup"))cleanup();
 if(!strcmp(argv[1],"all")){real_append();release_churn();}
 puts("MGFS record lifecycle: 40,000 ID handoffs, 1,000 create/release cycles, cluster deletion/growth, stale-ID rejection, failed-create rollback and uncertain-write retention passed (fault-injected block/VFS fixtures)");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-records-') as directory:
    path = Path(directory) / 'test.c';binary = path.with_suffix('');path.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary),mode],check=True)
