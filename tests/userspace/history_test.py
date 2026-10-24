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
ROOT = Path(__file__).resolve().parents[2]
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
#define MG_ERR_ACCESS_DENIED -3
#define MG_ERR_BAD_ARGUMENT -4
#define MG_ERR_NO_MEMORY -6
#define MG_ERR_ALREADY_EXISTS -9
#define MG_ERR_IO -12
#define MG_ERR_RETRY -32
#define MG_ERR_UNCERTAIN -33
#define MG_ERR_END_OF_FILE -1
#define MG_OPEN_READ 1U
#define MG_OPEN_RDWR 3U
#define MG_PATH_TYPE_FILE 1U
#define MG_PATH_TYPE_DIRECTORY 2U
#define MG_APP_DIRECTORY_STATE 2
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
static char disk[8192],legacy_disk[8192];static usize disk_size,legacy_size;static u64 identifier=1,revision=1;
static unsigned io_calls,save_calls,directory_calls;static int failure;static bool exists=true,legacy_exists=false,legacy_info_failure=false;static mg_result_t path_failure=MG_OK,move_failure=MG_OK;
static const char*current_path="/home/tester/.state/shoot/history";static const char*old_path="/home/tester/.shoot/history";
static mg_result_t directory_create(const char*);
static bool command_expand_home_path(const char*in,bool allow,char*out,usize cap){(void)in;(void)allow;if(cap<sizeof("/home/tester/.shoot/history"))return false;strcpy(out,"/home/tester/.shoot/history");return true;}
static mg_result_t app_directory_path(int kind,const char*app,char*out,usize cap){(void)kind;(void)app;if(cap<sizeof("/home/tester/.state/shoot"))return MG_ERR_BAD_ARGUMENT;strcpy(out,"/home/tester/.state/shoot");return MG_OK;}
static mg_result_t app_directory_ensure(int kind,const char*app,char*out,usize cap){mg_result_t r=app_directory_path(kind,app,out,cap);if(r<0)return r;char root[256];strcpy(root,"/home/tester/.state");directory_create(root);directory_create(out);return MG_OK;}
static mg_result_t path_info(const char*p,mg_path_info_t*out){io_calls++;if(!strcmp(p,current_path)){if(path_failure<0)return path_failure;if(!exists)return MG_ERR_NOT_FOUND;out->type=1;return MG_OK;}if(!strcmp(p,old_path)){if(legacy_info_failure)return MG_ERR_IO;if(!legacy_exists)return MG_ERR_NOT_FOUND;out->type=1;return MG_OK;}out->type=2;return MG_OK;}
static mg_result_t directory_create(const char*p){(void)p;io_calls++;directory_calls++;return MG_OK;}
static mg_result_t file_create(const char*p){io_calls++;if(strcmp(p,current_path))return MG_ERR_BAD_ARGUMENT;if(exists)return MG_ERR_ALREADY_EXISTS;exists=true;identifier++;revision=1;return MG_OK;}
static mg_result_t path_move(const char*from,const char*to){if(strcmp(from,old_path)||strcmp(to,current_path))return MG_ERR_BAD_ARGUMENT;if(move_failure<0)return move_failure;if(exists)return MG_ERR_ALREADY_EXISTS;if(!legacy_exists)return MG_ERR_NOT_FOUND;memcpy(disk,legacy_disk,legacy_size);disk_size=legacy_size;exists=true;legacy_exists=false;identifier=2;revision=1;return MG_OK;}
static mg_result_t file_open(const char*p,u32 flags){(void)flags;io_calls++;if(!strcmp(p,current_path))return exists?1:MG_ERR_NOT_FOUND;if(!strcmp(p,old_path))return legacy_exists?2:MG_ERR_NOT_FOUND;return MG_ERR_NOT_FOUND;}
static mg_result_t handle_close(mg_handle_t handle){(void)handle;return MG_OK;}
static mg_result_t file_snapshot(mg_handle_t h,char*out,usize capacity,mg_file_lines_state_t*state){io_calls++;if(h==2){assert(capacity>=legacy_size);memcpy(out,legacy_disk,legacy_size);*state=(mg_file_lines_state_t){2,1,legacy_size,0};return MG_OK;}assert(h==1&&capacity>=disk_size);memcpy(out,disk,disk_size);*state=(mg_file_lines_state_t){identifier,revision,disk_size,0};return MG_OK;}
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
const char *line_editor_history_at(const mg_line_history_t*,usize);
'''
for name in ['history_reset_navigation','line_editor_history_init','line_editor_history_set_capacity',
             'line_editor_history_set_enabled','line_editor_history_set_automatic_recording',
             'line_editor_history_set_draft','line_editor_history_at','line_editor_history_last',
             'utf8_valid_prefix','line_editor_history_add_text']:
    pre += function('libc/src/line_editor.c', name)
pre += without_includes((ROOT / 'userspace/shoot/history.c').read_text())
pre += r'''
#include <setjmp.h>
typedef struct {shoot_history_t *history;} shell_state_t;
static jmp_buf exit_environment;
static int exit_status, exit_messages;
static void console_write_string(const char *text)
{ assert(text); exit_messages++; }
static void process_exit(int status)
{ exit_status=status; longjmp(exit_environment,1); }
'''
pre += function('userspace/shoot/shell.c', 'shell_exit')
pre += r'''
static void seed(const char*s){disk_size=strlen(s);memcpy(disk,s,disk_size);revision++;}
static void expect(const char*s){if(disk_size!=strlen(s)||memcmp(disk,s,disk_size)){fprintf(stderr,"expect failed: got %.*s expected %s\n",(int)disk_size,disk,s);abort();}}
static void store_reset(bool current,bool old,const char*current_text,const char*old_text){memset(disk,0,sizeof(disk));memset(legacy_disk,0,sizeof(legacy_disk));disk_size=current_text?strlen(current_text):0;legacy_size=old_text?strlen(old_text):0;if(current_text)memcpy(disk,current_text,disk_size);if(old_text)memcpy(legacy_disk,old_text,legacy_size);exists=current;legacy_exists=old;legacy_info_failure=false;identifier=1;revision=1;directory_calls=0;failure=0;path_failure=MG_OK;move_failure=MG_OK;}
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
 shoot_history_clear(&h);h.config.history_persist=true;
 shoot_history_record(&h,"shutdown command");
 shell_state_t state={.history=&h};
 if(setjmp(exit_environment)==0)shell_exit(&state,0);
 assert(state.history==NULL&&exit_status==0&&exit_messages==0);
 expect("shutdown command\n");assert(save_calls==saves+1);
 assert(shoot_history_init(&h,&config));shoot_history_record(&h,"unsaved on error");
 state.history=&h;failure=1;exit_messages=0;
 if(setjmp(exit_environment)==0)shell_exit(&state,1);
 assert(state.history==NULL&&exit_status==1&&exit_messages==1);
 failure=0;
 config.history_limit=8;
 // A legacy-only file is read without creating ~/.state. It moves on the
 // first actual save, then the pending command appends to the same file.
 store_reset(false,true,NULL,"old-command\n");
 assert(shoot_history_init(&h,&config));last(&h,"old-command");
 assert(directory_calls==0&&exists==false&&legacy_exists);
 shoot_history_record(&h,"new-command");assert(!shoot_history_append(&h));
 expect("old-command\nnew-command\n");assert(exists&&!legacy_exists&&directory_calls==2);
 shoot_history_destroy(&h);
 // If both paths were already present, combine them in memory in old-to-new
 // order, append only to the new file, and retain the legacy file unchanged.
 store_reset(true,true,"new-command\n","old-command\n");
 assert(shoot_history_init(&h,&config));
 assert(h.editor_history.count==2);
 assert(!strcmp(line_editor_history_at(&h.editor_history,0),"old-command"));
 last(&h,"new-command");
 shoot_history_record(&h,"later");assert(!shoot_history_append(&h));
 expect("new-command\nlater\n");assert(legacy_exists&&legacy_size==12U);
 shoot_history_destroy(&h);
 // A read error at the legacy path does not suppress a valid new-path file.
 store_reset(true,true,"current-command\n","legacy-command\n");
 legacy_info_failure=true;
 assert(shoot_history_init(&h,&config));
 assert(h.editor_history.count==1);
 last(&h,"current-command");
 assert(h.baseline_valid);
 shoot_history_destroy(&h);
 // Empty homes remain empty until a history record is actually saved.
 store_reset(false,false,NULL,NULL);
 assert(shoot_history_init(&h,&config));
 assert(h.editor_history.count==0&&h.baseline_valid&&directory_calls==0);
 shoot_history_destroy(&h);
 // A malformed legacy log remains untouched and is not copied or appended.
 store_reset(false,true,NULL,NULL);legacy_disk[0]='b';legacy_disk[1]='a';
 legacy_disk[2]='d';legacy_disk[3]='\0';legacy_size=4;
 assert(shoot_history_init(&h,&config));
 assert(h.editor_history.count==0&&!h.baseline_valid&&directory_calls==0);
 assert(legacy_exists&&!exists);
 shoot_history_destroy(&h);
 // An I/O error reading the legacy path is not interpreted as an empty log.
 store_reset(false,false,NULL,NULL);legacy_info_failure=true;
 assert(shoot_history_init(&h,&config));
 assert(h.editor_history.count==0&&!h.baseline_valid&&directory_calls==0);
 shoot_history_destroy(&h);path_failure=MG_OK;
 // If another session establishes the new file after this session reads the
 // legacy file, append to the winner and preserve the still-present legacy.
 store_reset(false,true,NULL,"old-command\n");
 assert(shoot_history_init(&h,&config));
 exists=true;disk_size=strlen("other-session\n");
 memcpy(disk,"other-session\n",disk_size);
 shoot_history_record(&h,"local-session");
 assert(!shoot_history_append(&h));
 expect("other-session\nlocal-session\n");assert(legacy_exists);
 shoot_history_destroy(&h);
 // An uncertain migration move blocks append; the original remains available
 // and the pending command is retained for explicit recovery.
 store_reset(false,true,NULL,"old-command\n");
 assert(shoot_history_init(&h,&config));
 shoot_history_record(&h,"pending");move_failure=MG_ERR_UNCERTAIN;
 assert(shoot_history_append(&h)==MG_ERR_UNCERTAIN);
 assert(h.write_uncertain&&legacy_exists&&!exists);
 assert(h.editor_history.count&&
        h.entries[entry_slot(&h,h.editor_history.count-1U)].pending);
 shoot_history_destroy(&h);
 puts("Shoot history: options, pending markers, imports, replacement/truncation, limits, shutdown exit persistence, and save-error reporting passed (mocked persistence)");
}
'''
shell_source = (ROOT / 'userspace/shoot/shell.c').read_text()
assert 'if (input_result == MG_ERR_SHUTDOWN) shell_exit(&state, 0);' in shell_source
assert 'if (wait_result == MG_ERR_SHUTDOWN)\n            shell_exit(state, 0);' in shell_source
with tempfile.TemporaryDirectory(prefix='mangrove-history-') as directory:
    path=Path(directory)/'test.c';binary=path.with_suffix('');path.write_text(pre)
    subprocess.run([CC,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I',str(ROOT/'include'),str(path),str(ROOT/'libc/src/utf8.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
