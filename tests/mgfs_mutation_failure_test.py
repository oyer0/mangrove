#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production delete/rename control flow with deterministic phase failures.
Namespace publication, reads and reclamation are fixtures. Real tombstone,
bitmap and Record writes are covered by the companion MGFS regressions.
No disk-image integration or crash-recovery guarantees are tested here.
"""
import ast
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source_root = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT
source = (source_root / 'kernel/src/storage/mgfs.c').read_text()
# Reuse the host type/I/O fixture, without running its test or its main().
module = ast.parse((ROOT / 'tests/mgfs_write_test.py').read_text())
pre = next(ast.literal_eval(n.value) for n in module.body
           if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'pre' for t in n.targets))
pre = pre.replace('void*stage_transaction;} mgfs_fs_t;', 'void*stage_transaction;u64 root_record_id,record_id_count;} mgfs_fs_t;')
pre = pre.replace('vfs_ops_t*ops;};', 'vfs_ops_t*ops;void*fs_data;};')
# Remove unused write-specific fixture functions to keep -Werror useful.
pre = pre.replace('static ', '')

def function(name):
    # Admission itself is covered by complete VFS/MGFS mount tests.
    if name == 'mgfs_mutation_unavailable':
        return 'static bool mgfs_mutation_unavailable(const mgfs_fs_t *f){return f->record_lifecycle_uncertain || f->stage_transaction != NULL;}\n'
    original = name
    if re.search(r'^static [^\n;{}]*\b'+name+r'_impl\s*\(', source, re.M): name += '_impl'
    match = re.search(r'^static [^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    start = source.index('{', match.end()); end = start + 1; depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[match.start():end].replace(name+'(', original+'(', 1) + '\n'

code = pre + r'''
#define VFS_TYPE_DIRECTORY 2U
#define MGFS_RECORD_DIRECTORY 2U
#define MGFS_EXTENT_DIRECTORY_METADATA 2U
#define MGFS_DIRENT_IN_USE 1U
#define VFS_ERR_NOT_FOUND -7
#define VFS_ERR_NOT_EMPTY -8
#define VFS_ERR_BAD_FORMAT -9
typedef struct {u64 target_record_id,total_bytes;} mgfs_directory_entry_t;
static bool published;
static const char *error;
const char *mgfs_last_error(void){return error;}
static bool mgfs_locate_directory_entry(mgfs_fs_t*f,u64 id,const char*n,u8*p,u64*off,mgfs_directory_entry_t*e,u8*c){(void)f;(void)id;(void)n;if(!io())return false;memcpy(p,record,192);memcpy(c,record,192);*off=0;*e=(mgfs_directory_entry_t){2,40};return true;}
static bool mgfs_directory_has_live_entries(mgfs_fs_t*f,const u8*r,bool*out){(void)f;(void)r;*out=false;return io();}
static void mgfs_dentry_cache_invalidate_parent(mgfs_fs_t*f,u64 id){(void)f;(void)id;}
static bool mgfs_release_record(mgfs_fs_t*f,u64 slot,u64 id){(void)f;(void)slot;(void)id;return io();}
static bool mgfs_publish_tombstone(mgfs_fs_t*f,const u8*r,u64 off,u64 len){(void)f;(void)r;(void)off;(void)len;if(!io())return false;published=true;return true;}
static bool mgfs_valid_utf8_name(const u8*n,usize len){(void)n;return len>0;}
static bool mgfs_scan_directory(vfs_node_t*d,const char*n,u64 i,void*a,void*b){(void)d;(void)n;(void)i;(void)a;(void)b;return false;}
static bool mgfs_record_in_subtree(mgfs_fs_t*f,u64 id,u64 dst,u64*v,u64 cap,u64*count,bool*found){(void)f;(void)id;(void)dst;(void)v;(void)cap;(void)count;*found=false;return io();}
static bool mgfs_append_directory_entry(mgfs_fs_t*f,u64 id,const u8*e,u64 len,u8*out){(void)f;(void)id;(void)e;(void)len;(void)out;if(!io())return false;published=true;return true;}
static bool mgfs_touch_directory_record(mgfs_fs_t*f,u64 id){(void)f;(void)id;return io();}
static void mgfs_refresh_directory_node(mgfs_fs_t*f,vfs_node_t*n){(void)f;(void)n;}
'''
# Existing error fixture must preserve diagnostic text for rename's lookup check.
code = code.replace('void mgfs_set_error(const char*s){(void)s;}', 'const char *error;void mgfs_set_error(const char*s){error=s;}')
code = code.replace('static const char *error;', '')
code += function('mgfs_mutation_unavailable') + function('mgfs_delete_entry') + function('mgfs_rename_entry')
code += r'''
static void seed(void){assert(!live_allocs);memset(&fs,0,sizeof(fs));fs.layout.record_count=32;fs.root_record_id=1;memset(record,0,192);mgfs_store_le64(record,1);mgfs_store_le64(record+8,1);io_count=fail_io=alloc_count=fail_alloc=0;published=false;error="no error";}
int main(void){vfs_super_t super={.private_data=&fs};vfs_node_t dir={.type=2,.super=&super,.fs_data=(void*)1};
 for(unsigned rename=0;rename<2;rename++){
  seed();int result=rename?mgfs_rename_entry(&dir,"old",&dir,"new"):mgfs_delete_entry(&dir,"old",false);assert(result==VFS_OK);unsigned calls=io_count;
  bool post=false,pre=false;
  for(unsigned failure=1;failure<=calls;failure++){
   seed();fail_io=failure;result=rename?mgfs_rename_entry(&dir,"old",&dir,"new"):mgfs_delete_entry(&dir,"old",false);assert(result!=VFS_OK&&!live_allocs);
   if(published){post=true;assert(fs.record_lifecycle_uncertain&&result==VFS_ERR_UNCERTAIN);unsigned before=io_count;
    fail_io=0;assert(mgfs_delete_entry(&dir,"old",false)==VFS_ERR_UNCERTAIN);assert(mgfs_rename_entry(&dir,"old",&dir,"new")==VFS_ERR_UNCERTAIN);assert(before==io_count);
   }else{pre=true;assert(!fs.record_lifecycle_uncertain);}
  }
  assert(post&&pre);
 }
 puts("MGFS mutation failures: delete/rename post-publication reads, reclamation and clean-state failures quarantine; pre-publication failures remain retryable (phase fixtures)");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-mutations-') as directory:
    path=Path(directory)/'test.c';binary=path.with_suffix('');path.write_text(code)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
