/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fsstress.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
static int error(void)
{switch(errno){case ENOENT:return FS_MISSING;case EEXIST:return FS_EXISTS;case ENOTEMPTY:return FS_NOTEMPTY;case ENOMEM:return -6;case ENOSPC:return -36;case EACCES:case EROFS:return -8;default:return FS_IO;}}
int io_native(void){return 0;}
int io_enter(const char *root){(void)root;return 0;}
int io_open(const char *p,int64_t *h){int f=open(p,O_RDWR);if(f<0)return error();*h=f;return 0;}
int io_close(int64_t h){return close((int)h)?error():0;}
int io_create(const char *p){int h=open(p,O_CREAT|O_EXCL|O_RDWR,0600);return h<0?error():io_close(h);}
int io_mkdir(const char *p){return mkdir(p,0700)?error():0;}
int io_remove(const char *p,int d){return (d?rmdir(p):unlink(p))?error():0;}
int io_move(const char *a,const char *b){return rename(a,b)?error():0;}
int io_stat(const char *p,fs_info *out){struct stat s;if(lstat(p,&s))return error();*out=(fs_info){S_ISREG(s.st_mode)?1U:S_ISDIR(s.st_mode)?2U:0,(uint64_t)s.st_size,(uint64_t)s.st_ino};return 0;}
int io_seek(int64_t h,uint32_t n){return lseek((int)h,n,SEEK_SET)<0?error():0;}
fs_result io_read(int64_t h,void *p,uint32_t n){ssize_t r=read((int)h,p,n);return (fs_result){r<0?error():0,r<0?0:(uint32_t)r};}
fs_result io_write(int64_t h,const void *p,uint32_t n){ssize_t r=write((int)h,p,n);return (fs_result){r<0?error():0,r<0?0:(uint32_t)r};}
int io_resize(int64_t h,uint32_t n){return ftruncate((int)h,n)?error():0;}
int io_list(const char *p,fs_entry *out,uint32_t cap,uint32_t *count)
{
    DIR *d=opendir(p);if(!d)return error();*count=0;int result=0;struct dirent *item;
    errno=0;while((item=readdir(d))) {
        if(!strcmp(item->d_name,".") || !strcmp(item->d_name,".."))continue;
        if(*count==cap || strlen(item->d_name)>=sizeof(out[0].name)){result=-10;break;}
        char path[256];size_t a=strlen(p),b=strlen(item->d_name);if(a+b+2>sizeof(path)){result=-10;break;}
        memcpy(path,p,a);path[a]='/';memcpy(path+a+1,item->d_name,b+1);
        fs_info info={0};if((result=io_stat(path,&info)))break;
        strcpy(out[*count].name,item->d_name);out[(*count)++].type=info.type;errno=0;
    }
    if(!result && errno)result=error();if(closedir(d) && !result)result=error();return result;
}
int io_snapshot(int64_t h,unsigned char *p,uint32_t n,fs_version *v){(void)h;(void)p;(void)n;(void)v;return FS_UNSUPPORTED;}
int io_replace(int64_t h,const unsigned char *p,uint32_t n){(void)h;(void)p;(void)n;return FS_UNSUPPORTED;}
int io_contents(const char *p,const unsigned char *data,uint32_t n,int64_t *h){(void)p;(void)data;(void)n;(void)h;return FS_UNSUPPORTED;}
int io_trace_save(const char *p,const unsigned char *data,uint32_t n)
{
    int h=open(p,O_CREAT|O_EXCL|O_WRONLY,0600);if(h<0)return error();
    ssize_t amount=write(h,data,n);int result=amount==(ssize_t)n?0:amount<0?error():FS_IO;
    if(!result && fsync(h))result=error();int closed=io_close(h);return result?result:closed;
}
int io_trace_load(const char *p,unsigned char *data,uint32_t cap,uint32_t *count)
{
    int h=open(p,O_RDONLY);if(h<0)return error();struct stat s;int result=0;
    if(fstat(h,&s) || !S_ISREG(s.st_mode) || s.st_size<0 || (uint64_t)s.st_size>cap)result=-10;
    if(!result){ssize_t n=read(h,data,(size_t)s.st_size);if(n!=s.st_size)result=n<0?error():FS_IO;else *count=(uint32_t)n;}
    int closed=io_close(h);return result?result:closed;
}
uint64_t io_milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return ~0ULL;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
