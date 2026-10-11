#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production VFS/MGFS merge and snapshot logic; reads/writes are mocked.
Actual MGFS mutation failure paths are exercised in mgfs_write_test.py.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else 'cc'


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    assert match, name
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


COMMON = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
typedef unsigned long long u64; typedef unsigned int u32; typedef unsigned char u8;
typedef long long i64; typedef size_t usize; typedef long isize;
#define MG_FILE_LINES_MAX 4096U
#define MG_FILE_LINE_CAPACITY 512U
#define MG_FILE_LINES_RESET 1U
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM -1
#define VFS_ERR_IO -3
#define VFS_ERR_NO_MEM -4
#define VFS_ERR_ACCESS_DENIED -8
#define VFS_ERR_DEVICE_GONE -11
#define VFS_ERR_UNSUPPORTED -6
#define VFS_ERR_UNCERTAIN -14
#define VFS_ERR_BUFFER_TOO_SMALL -15
#define VFS_ERR_BAD_FORMAT -9
#define VFS_PERMISSION_OWNER_READ 1U
#define VFS_PERMISSION_OTHER_READ 4U
#define VFS_PERMISSION_KNOWN 15U
typedef struct {u64 identifier,revision;usize length,append_offset;} mg_file_lines_state_t;
#define VFS_OPEN_RDWR 3U
#define VFS_OPEN_READ 1U
'''


def run(name, source):
    with tempfile.TemporaryDirectory(prefix='mangrove-launch-') as directory:
        path = Path(directory) / (name + '.c')
        binary = path.with_suffix('')
        path.write_text(COMMON + source)
        subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


merge = r'''
#define MGFS_RECORD_BYTES 192U
#define MG_FILE_CONTENT_MAX_BYTES (2U * 1024U * 1024U)
#define VFS_TYPE_FILE 1
struct vfs_node; typedef struct vfs_node vfs_node_t;
typedef struct {u64 record_id,generation,size;u32 owner_uid,permissions;} vfs_file_facts_t;
typedef struct {u32 uid;} process_credentials_t;
typedef struct {u32 unused;} process_t;
typedef struct {int (*truncate)(vfs_node_t*);int (*append_lines)(vfs_node_t*,const char*,usize,u32,u32,mg_file_lines_state_t*);int (*snapshot)(vfs_node_t*,char*,usize,mg_file_lines_state_t*);int (*file_facts)(vfs_node_t*,vfs_file_facts_t*);} vfs_ops_t;
typedef struct {void *private_data;pthread_mutex_t lock;} vfs_super_t;
struct vfs_node {int type; vfs_super_t *super; u64 inode, size; vfs_ops_t *ops;};
typedef struct {bool record_lifecycle_uncertain;void *stage_transaction;} mgfs_fs_t;
typedef struct {vfs_node_t *node;vfs_super_t *super;u32 flags;pthread_mutex_t offset_lock;u64 save_record_id,save_generation;bool save_expectation_valid;int save_expectation_error;} vfs_file_handle_t;
static bool mutex_lock(pthread_mutex_t *m){return pthread_mutex_lock(m)==0;}
static void mutex_unlock(pthread_mutex_t *m){assert(!pthread_mutex_unlock(m));}
static bool vfs_handle_valid(vfs_file_handle_t *h){return h&&h->node&&h->super;}
static bool vfs_super_is_live(vfs_super_t *s){return s!=NULL;}
#define VFS_ACCESS_READ_WRITE 3U
#define VFS_ACCESS_READ 1U
static bool vfs_check_access(vfs_node_t*n,u32 rights){(void)n;return rights==3||rights==1;}
static process_t *process_current(void){return NULL;}
static bool process_get_credentials(process_t *p,process_credentials_t *out){(void)p;(void)out;return false;}
static bool vfs_node_on_read_only_mount(vfs_node_t*n){(void)n;return false;}
static bool vfs_super_operation_lock(vfs_super_t*s){return mutex_lock(&s->lock);}
static void vfs_super_operation_unlock(vfs_super_t*s){mutex_unlock(&s->lock);}
static char disk[4*1024*1024]; static usize disk_size; static bool fail_alloc,fail_read;
static void *kmalloc(usize size) {return fail_alloc?NULL:malloc(size);}
static void kfree(void *ptr) {free(ptr);}
static bool mgfs_read_record(mgfs_fs_t *fs,u64 inode,u8 *out) {(void)fs;(void)inode;memset(out,0,192);memcpy(out+32,&disk_size,8);return true;}
static bool mgfs_validate_regular_file_record(const u8 *record) {(void)record;return true;}
static u64 mgfs_get_le64(const u8 *ptr) {u64 value;memcpy(&value,ptr,8);return value;}
static u64 mgfs_read(vfs_node_t *node,u64 offset,u64 size,void *out) {(void)node;if(fail_read||offset+size>disk_size)return 0;memcpy(out,disk+offset,size);return size;}
static u64 mgfs_write(vfs_node_t *node,u64 offset,u64 size,const void *data) {assert(!offset&&size<sizeof(disk));memcpy(disk,data,size);disk_size=size;node->size=size;return size;}
static int truncate_file(vfs_node_t *node) {disk_size=0;node->size=0;return VFS_OK;}
static void seed(const char *text) {disk_size=strlen(text);memcpy(disk,text,disk_size);}
static void expect(const char *text) {assert(disk_size==strlen(text)&&!memcmp(disk,text,disk_size));}
'''
merge += function('kernel/src/storage/mgfs.c', 'mgfs_mutation_unavailable')
merge += function('kernel/src/storage/mgfs.c', 'mgfs_append_lines')
merge += function('kernel/src/vfs.c', 'vfs_file_append_lines')
merge += function('kernel/src/storage/mgfs.c', 'mgfs_snapshot')
merge += function('kernel/src/vfs.c', 'vfs_file_snapshot')
merge += r'''
static void *save_session(void *arg){vfs_file_handle_t *handle=arg;const char *line=handle->node->inode==1?"A\n":"B\n";for(unsigned i=0;i<100;i++)assert(!vfs_file_append_lines(handle,line,2,1000,0,NULL));return NULL;}
int main(void) {
 mgfs_fs_t fs={0};vfs_super_t sb={&fs,PTHREAD_MUTEX_INITIALIZER};vfs_ops_t ops={.truncate=truncate_file,.append_lines=mgfs_append_lines,.snapshot=mgfs_snapshot};
 vfs_node_t a={VFS_TYPE_FILE,&sb,1,0,&ops},b=a;
 seed("old\n");assert(!mgfs_append_lines(&a,"A\n",2,4,0,NULL));assert(!mgfs_append_lines(&b,"B\n",2,4,0,NULL));expect("old\nA\nB\n");
 assert(!mgfs_append_lines(&a,"C\nD\n",4,3,0,NULL));expect("B\nC\nD\n");
 assert(!mgfs_append_lines(&a,NULL,0,2,0,NULL));expect("C\nD\n");
 assert(!mgfs_append_lines(&b,"fresh\n",6,2,MG_FILE_LINES_RESET,NULL));expect("fresh\n");
 assert(!mgfs_append_lines(&a,NULL,0,2,MG_FILE_LINES_RESET,NULL));expect("");
 seed("unfinished");assert(!mgfs_append_lines(&a,"next\n",5,2,0,NULL));expect("unfinished\nnext\n");
 assert(mgfs_append_lines(&a,"bad",3,2,0,NULL)==VFS_ERR_INVALID_PARAM);
 assert(mgfs_append_lines(&a,"x\0\n",3,2,0,NULL)==VFS_ERR_INVALID_PARAM);
 char oversized[513];memset(oversized,'x',512);oversized[512]='\n';assert(mgfs_append_lines(&a,oversized,513,2,0,NULL)==VFS_ERR_INVALID_PARAM);
 fail_alloc=true;assert(mgfs_append_lines(&a,"ok\n",3,2,0,NULL)==VFS_ERR_NO_MEM);expect("unfinished\nnext\n");fail_alloc=false;
 fail_read=true;assert(mgfs_append_lines(&a,"ok\n",3,2,0,NULL)==VFS_ERR_IO);expect("unfinished\nnext\n");fail_read=false;
 for(usize i=0;i<300;i++){disk[i*6]='a';memcpy(disk+i*6+1,"bcde\n",5);}disk_size=1800;
 assert(!mgfs_append_lines(&a,"tail\n",5,2,0,NULL));expect("abcde\ntail\n");
 seed("old\n");b.inode=2;
 vfs_file_handle_t ha={.node=&a,.super=&sb,.flags=3,.offset_lock=PTHREAD_MUTEX_INITIALIZER},hb={.node=&b,.super=&sb,.flags=3,.offset_lock=PTHREAD_MUTEX_INITIALIZER};
 pthread_t ta,tb;assert(!pthread_create(&ta,NULL,save_session,&ha));assert(!pthread_create(&tb,NULL,save_session,&hb));assert(!pthread_join(ta,NULL));assert(!pthread_join(tb,NULL));
 assert(disk_size==404&&!memcmp(disk,"old\n",4));unsigned ac=0,bc=0;for(usize i=4;i<disk_size;i+=2){assert(disk[i+1]=='\n');if(disk[i]=='A')ac++;else if(disk[i]=='B')bc++;else assert(false);}assert(ac==100&&bc==100);
 ha.flags=1;assert(vfs_file_append_lines(&ha,"bad\n",4,10,0,NULL)==VFS_ERR_ACCESS_DENIED);assert(disk_size==404);
 char snap[4096];mg_file_lines_state_t state;
 assert(!vfs_file_snapshot(&ha,snap,sizeof(snap),&state));assert(state.length==disk_size&&!memcmp(snap,disk,disk_size));
 assert(vfs_file_snapshot(&hb,snap,1,&state)==VFS_ERR_BUFFER_TOO_SMALL);
 ha.flags=0;assert(vfs_file_snapshot(&ha,snap,sizeof(snap),&state)==VFS_ERR_ACCESS_DENIED);
 assert(!pthread_mutex_destroy(&ha.offset_lock));assert(!pthread_mutex_destroy(&hb.offset_lock));assert(!pthread_mutex_destroy(&sb.lock));
 puts("history merge: concurrent sessions, limits, clear, malformed lines, allocation/read failures and access rights passed");
}
'''


run('history_fs', merge)
