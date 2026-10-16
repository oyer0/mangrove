/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fsstress.h"
#include <mangrove.h>
#include <mg/time.h>
#include <string.h>
/* Retain the actual parent location for the process lifetime. All workload
 * paths beneath it use native anchored resolution, never a check-then-lookup. */
static char parent[161];
static const char *anchored(const char *path)
{size_t n=strlen(parent);if(n==1 && parent[0]=='/' && path[0]=='/')return path+1;return n && !strncmp(path,parent,n) && path[n]=='/'?path+n+1:path;}
int io_native(void){return 1;}
int io_enter(const char *root)
{
    size_t n=strlen(root);while(n && root[n-1]!='/')n--;
    if(!n || n>sizeof(parent))return -4;
    if(n==1){parent[0]='/';parent[1]=0;}
    else {memcpy(parent,root,n-1);parent[n-1]=0;}
    mg_result_t r=process_chdir(parent);if(r)parent[0]=0;return (int)r;
}
int io_open(const char *p,int64_t *h){mg_result_t r=file_open(anchored(p),MG_OPEN_RDWR);if(r<=0)return r?(int)r:FS_IO;*h=r;return 0;}
int io_close(int64_t h){return (int)handle_close((mg_handle_t)h);}
int io_create(const char *p){return (int)file_create(anchored(p));}
int io_mkdir(const char *p){return (int)directory_create(anchored(p));}
int io_remove(const char *p,int d){(void)d;return (int)path_remove(anchored(p));}
int io_move(const char *a,const char *b){return (int)path_move(anchored(a),anchored(b));}
int io_stat(const char *p,fs_info *out){mg_path_info_t i;mg_result_t r=path_info(anchored(p),&i);if(!r)*out=(fs_info){i.type,i.size,i.identifier};return (int)r;}
int io_seek(int64_t h,uint32_t n){return (int)file_seek((mg_handle_t)h,n,MG_SEEK_SET);}
fs_result io_read(int64_t h,void *p,uint32_t n){mg_result_t r=object_read((mg_handle_t)h,p,n);return (fs_result){r<0?(int)r:r>(mg_result_t)n?FS_IO:0,r<0 || r>(mg_result_t)n?0:(uint32_t)r};}
fs_result io_write(int64_t h,const void *p,uint32_t n){mg_result_t r=object_write((mg_handle_t)h,p,n);return (fs_result){r<0?(int)r:r>(mg_result_t)n?FS_IO:0,r<0 || r>(mg_result_t)n?0:(uint32_t)r};}
int io_resize(int64_t h,uint32_t n){return n?FS_UNSUPPORTED:(int)file_truncate((mg_handle_t)h);}
int io_list(const char *p,fs_entry *out,uint32_t cap,uint32_t *count)
{
    mg_result_t h=directory_open(anchored(p));if(h<=0)return h?(int)h:FS_IO;*count=0;
    mg_directory_entry_t entry;mg_result_t r;
    while((r=directory_read((mg_handle_t)h,&entry))==MG_OK){
        if(*count==cap || strlen(entry.name)>=sizeof(out[0].name)){r=MG_ERR_BUFFER_TOO_SMALL;break;}
        strcpy(out[*count].name,entry.name);out[(*count)++].type=entry.type;}
    mg_result_t closed=handle_close((mg_handle_t)h);
    return r==MG_ERR_END_OF_FILE?(int)closed:(int)r;
}
int io_snapshot(int64_t h,unsigned char *p,uint32_t n,fs_version *out)
{mg_file_lines_state_t s={0};mg_result_t r=file_snapshot((mg_handle_t)h,(char *)p,n,&s);if(!r)*out=(fs_version){s.identifier,s.revision,s.length};return (int)r;}
int io_replace(int64_t h,const unsigned char *p,uint32_t n){return (int)file_replace((mg_handle_t)h,p,n);}
int io_contents(const char *p,const unsigned char *data,uint32_t n,int64_t *h)
{int64_t before=*h;mg_handle_t file=(mg_handle_t)before;mg_result_t r=file_create_contents(anchored(p),data,n,&file);if(!r || file!=(mg_handle_t)before)*h=file;return (int)r;}
int io_trace_save(const char *p,const unsigned char *data,uint32_t n)
{int64_t h=-1;int r=io_contents(p,data,n,&h);return r?r:io_close(h);}
int io_trace_load(const char *p,unsigned char *data,uint32_t cap,uint32_t *count)
{
    mg_path_info_t i;mg_result_t r=path_info(p,&i);
    if(r || i.type!=MG_PATH_TYPE_FILE || i.size>cap)return r?(int)r:MG_ERR_BUFFER_TOO_SMALL;
    r=file_open(p,MG_OPEN_READ);if(r<=0)return r?(int)r:FS_IO;mg_handle_t h=(mg_handle_t)r;
    r=object_read(h,data,(usize)i.size);int closed=(int)handle_close(h);
    if(r<0 || r!=(mg_result_t)i.size)return r<0?(int)r:FS_IO;
    *count=(uint32_t)r;return closed;
}
uint64_t io_milliseconds(void){mg_monotonic_time_t t;if(mg_clock_monotonic(&t)!=MG_OK)return ~0ULL;return t.milliseconds;}
