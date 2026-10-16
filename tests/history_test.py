#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production Shoot history and ring functions; persistent files are mocked.
Real MGFS failure paths are tested separately in history_fs_test.py.
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
    brace = source.index('{', match.end()); end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[match.start():end] + '\n'
def without_includes(text):
    return re.sub(r'^#(?:include|pragma).*\n', '', text, flags=re.M)
pre = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <types.h>
#include <mg/utf8.h>
typedef long mg_result_t; typedef unsigned mg_handle_t;
#define MG_OK 0
#define MG_ERR_NOT_FOUND -2
#define MG_ERR_BAD_ARGUMENT -4
#define MG_ERR_NO_MEMORY -6
#define MG_ERR_ALREADY_EXISTS -9
#define MG_ERR_IO -12
#define MG_ERR_RETRY -32
#define MG_ERR_UNCERTAIN -33
#define MG_OPEN_READ 1U
#define MG_OPEN_RDWR 3U
#define MG_PATH_TYPE_DIRECTORY 2U
#define MG_FILE_LINES_MAX 4096U
#define MG_FILE_LINE_CAPACITY 512U
#define MG_FILE_LINES_RESET 1U
typedef struct {u32 type;} mg_path_info_t;
typedef struct {u64 identifier,revision;usize length,append_offset;} mg_file_lines_state_t;
typedef struct {bool history_persist,history_enabled,history_ignore_space,history_ignore_duplicates;u32 history_limit;} shoot_config_t;
'''
line_header = (ROOT / 'libc/include/mg/line_editor.h').read_text()
pre += line_header[line_header.index('typedef struct mg_line_history {'):line_header.index('/* Reusable single-line')]
pre += without_includes((ROOT / 'userspace/shoot/history.h').read_text())
pre += r'''
static char disk[8192];static usize disk_size;static u64 identifier=1,revision=1;
static unsigned io_calls,save_calls;static int failure;static bool exists=true;
static mg_result_t path_info(const char*p,mg_path_info_t*out){(void)p;io_calls++;out->type=2;return MG_OK;}
static mg_result_t directory_create(const char*p){(void)p;io_calls++;return MG_OK;}
static mg_result_t file_create(const char*p){(void)p;io_calls++;if(exists)return MG_ERR_ALREADY_EXISTS;exists=true;identifier++;revision=1;return MG_OK;}
static mg_result_t file_open(const char*p,u32 flags){(void)p;(void)flags;io_calls++;return exists?1:MG_ERR_NOT_FOUND;}
static mg_result_t handle_close(mg_handle_t handle){(void)handle;return MG_OK;}
static mg_result_t file_snapshot(mg_handle_t h,char*out,usize capacity,mg_file_lines_state_t*state){(void)h;io_calls++;assert(capacity>=disk_size);memcpy(out,disk,disk_size);*state=(mg_file_lines_state_t){identifier,revision,disk_size,0};return MG_OK;}
static mg_result_t file_append_lines_state(mg_handle_t h,const char*data,usize length,u32 limit,u32 flags,mg_file_lines_state_t*state){
 (void)h;save_calls++;if(failure==1)return MG_ERR_IO;
 if(flags&MG_FILE_LINES_RESET)disk_size=0;
 usize incoming=disk_size;if(length)memcpy(disk+disk_size,data,length);disk_size+=length;
 usize count=0,first=0;for(usize i=0;i<disk_size;i++)count+=disk[i]=='\n';
 while(count>limit){while(disk[first++]!='\n'){}count--;}
 if(first){memmove(disk,disk+first,disk_size-first);disk_size-=first;}
 revision++;
 *state=(mg_file_lines_state_t){identifier,revision,disk_size,incoming-first};
 return failure==2?MG_ERR_UNCERTAIN:MG_OK;
}
static bool shoot_config_paths(char*d,usize dc,char*c,usize cc,char*p,usize pc){(void)dc;(void)cc;(void)pc;strcpy(d,"/home/.shoot");strcpy(c,"/home/.shoot/config");strcpy(p,"/home/.shoot/history");return true;}
const char *line_editor_history_at(const mg_line_history_t*,usize);
'''
for name in ['history_reset_navigation','line_editor_history_init','line_editor_history_set_capacity',
             'line_editor_history_set_enabled','line_editor_history_set_automatic_recording',
             'line_editor_history_set_draft','line_editor_history_at','line_editor_history_last',
             'utf8_valid_prefix','line_editor_history_add_text']:
    pre += function('libc/src/line_editor.c', name)
pre += without_includes((ROOT / 'userspace/shoot/history.c').read_text())
pre += r'''
static void seed(const char*s){disk_size=strlen(s);memcpy(disk,s,disk_size);revision++;}
static void expect(const char*s){assert(disk_size==strlen(s)&&!memcmp(disk,s,disk_size));}
static void last(shoot_history_t*h,const char*s){assert(!strcmp(line_editor_history_last(&h->editor_history),s));}
int main(void){
 shoot_config_t config={true,true,true,true,8};shoot_history_t h;
 seed("loaded\n");assert(shoot_history_init(&h,&config));assert(h.editor_history.count==1);
 unsigned old_io=io_calls;shoot_history_record(&h,"A");shoot_history_record(&h,"A");shoot_history_record(&h," ignored");assert(io_calls==old_io);
 assert(!shoot_history_append(&h));expect("loaded\nA\n");unsigned saves=save_calls;assert(!shoot_history_append(&h)&&save_calls==saves);
 assert(!shoot_history_read(&h,true));assert(h.editor_history.count==2); // don't replay own append
 seed("loaded\nA\nB\n");assert(!shoot_history_read(&h,true));last(&h,"B");assert(!shoot_history_read(&h,true));assert(h.editor_history.count==3);
 shoot_history_record(&h,"pending");assert(!shoot_history_read(&h,false));assert(h.editor_history.count==7);last(&h,"B");
 assert(!shoot_history_append(&h));expect("loaded\nA\nB\npending\n");
 assert(shoot_history_delete(&h,4));assert(!shoot_history_delete(&h,0));assert(!shoot_history_delete(&h,100));
 shoot_history_record(&h,"remove");assert(shoot_history_delete(&h,h.editor_history.count));saves=save_calls;assert(!shoot_history_append(&h)&&save_calls==saves);
 old_io=io_calls;shoot_history_clear(&h);assert(io_calls==old_io&&h.editor_history.count==0);assert(!shoot_history_append(&h)&&save_calls==saves);expect("loaded\nA\nB\npending\n");
 seed("replacement\n");identifier++;assert(shoot_history_read(&h,true)==MG_ERR_RETRY);assert(h.editor_history.count==0);
 seed("replacement\nnew\n");assert(!shoot_history_read(&h,true));assert(h.editor_history.count==1);last(&h,"new");
 seed("short\n");assert(shoot_history_read(&h,true)==MG_ERR_RETRY);assert(h.editor_history.count==1);
 seed("short\nnext\n");assert(!shoot_history_read(&h,true));last(&h,"next");
 seed("short\nnext\n");assert(shoot_history_read(&h,true)==MG_ERR_RETRY); // same-size rewrite
 assert(!shoot_history_read(&h,false)); // explicit reread allows duplicates
 // A full reread after truncation must discard obsolete saved positions.
 shoot_history_clear(&h);seed("a\nb\nc\n");assert(!shoot_history_read(&h,false));
 seed("a\n");assert(!shoot_history_read(&h,false));
 seed("a\nb\n");usize before=h.editor_history.count;assert(!shoot_history_read(&h,true));assert(h.editor_history.count==before+1);last(&h,"b");
 // Legacy final records are readable explicitly, but never an append baseline.
 seed("legacy");before=h.editor_history.count;assert(!shoot_history_read(&h,false));assert(h.editor_history.count==before+1);last(&h,"legacy");
 seed("legacy\nnew\n");before=h.editor_history.count;assert(shoot_history_read(&h,true)==MG_ERR_RETRY&&h.editor_history.count==before);
 seed("legacy\nnew\nnext\n");assert(!shoot_history_read(&h,true));last(&h,"next");
 seed("short\nnext\n");assert(shoot_history_read(&h,true)==MG_ERR_RETRY);
 shoot_history_clear(&h);shoot_history_record(&h,"uncertain");failure=2;assert(shoot_history_append(&h)==MG_ERR_UNCERTAIN);expect("short\nnext\nuncertain\n");
 saves=save_calls;failure=0;assert(shoot_history_append(&h)==MG_ERR_UNCERTAIN&&save_calls==saves);assert(!shoot_history_save(&h)&&save_calls==saves);
 assert(!shoot_history_write(&h)&&!h.write_uncertain);expect("uncertain\n");assert(!shoot_history_append(&h));expect("uncertain\n");
 shoot_history_record(&h,"known failure");failure=1;assert(shoot_history_append(&h)==MG_ERR_IO&&!h.write_uncertain);failure=0;assert(!shoot_history_append(&h));expect("uncertain\nknown failure\n");
 shoot_history_clear(&h);assert(!shoot_history_write(&h));expect("");
 config.history_limit=3;shoot_history_apply_config(&h,&config);for(int i=0;i<8;i++){char text[16];snprintf(text,sizeof(text),"cmd%d",i);shoot_history_record(&h,text);}
 assert(h.editor_history.count==3);last(&h,"cmd7");assert(!shoot_history_append(&h));expect("cmd5\ncmd6\ncmd7\n");
 shoot_history_record(&h,"cmd8");config.history_limit=1;shoot_history_apply_config(&h,&config);assert(!shoot_history_append(&h));expect("cmd8\n");
 shoot_history_clear(&h);shoot_history_record(&h,"final");h.config.history_persist=false;saves=save_calls;assert(shoot_history_save(&h)&&save_calls==saves);
 shoot_history_destroy(&h);assert(save_calls==saves); // never implicitly retry
 puts("Shoot history: options, pending markers, imports, replacement/truncation, limits and uncertain-write blocking passed (mocked persistence)");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-history-') as directory:
    path=Path(directory)/'test.c';binary=path.with_suffix('');path.write_text(pre)
    subprocess.run([CC,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I',str(ROOT/'include'),str(path),str(ROOT/'libc/src/utf8.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
