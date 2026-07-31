#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <block.h>
#include <heap.h>
#include <vfs.h>
#include <storage/mgfs.h>

static int same_bytes(const void *a,const void *b,size_t n) { const unsigned char *x=a,*y=b; while(n--) if(*x++!=*y++) return 0; return 1; }

void *kmalloc(usize n) { return malloc((size_t)n); }
void kfree(void *p) { free(p); }
bool block_read(block_device_t *d, u64 lba, u32 n, void *buf) { return d->read(d,lba,n,buf); }
bool block_write(block_device_t *d, u64 lba, u32 n, const void *buf) { return d->write(d,lba,n,buf); }
int vfs_register_fs(vfs_fs_type_t *f) { (void)f; return VFS_OK; }

static bool image_read(block_device_t *d, u64 lba, u32 count, void *buf) {
    FILE *f=(FILE *)d->driver_data;
    if (fseek(f,(long)(lba*512ULL),SEEK_SET)!=0) return false;
    return fread(buf,512,count,f)==count;
}
static bool check(const char *path, const char *expect, u64 offset, u64 request) {
    vfs_node_t *n; char b[9000]; u64 got;
    if (vfs_lookup(path,&n)!=VFS_OK || !n) { fprintf(stderr,"lookup failed: %s (%s)\n",path,mgfs_last_error()); return false; }
    got=vfs_read(n,offset,request,b);
    if (got!=strlen(expect) || !same_bytes(b,expect,(size_t)got)) { fprintf(stderr,"read mismatch: %s got=%llu want=%zu\n",path,(unsigned long long)got,strlen(expect)); return false; }
    return true;
}
static bool check_fill(const char *path, char value, u64 want, u64 offset, u64 request) {
    vfs_node_t *n; char b[128]; u64 got;
    if (vfs_lookup(path,&n)!=VFS_OK) return false;
    got=vfs_read(n,offset,request,b); if (got!=want) return false;
    for (u64 i=0;i<got;i++) if (b[i]!=value) return false; return true;
}

#include "../kernel/src/storage/mgfs.c"

static vfs_node_t *test_root;
int vfs_lookup(const char *path, vfs_node_t **out) {
    const char *p=path; vfs_node_t *n=test_root;
    while (*p=='/') p++;
    while (*p) { char c[256]; usize l=0; while (*p && *p!='/') { if(l<255)c[l++]=*p; p++; } c[l]=0; if(n->type!=VFS_TYPE_DIRECTORY) return VFS_ERR_NOT_FOUND; n=mgfs_finddir(n,c); if(!n)return VFS_ERR_NOT_FOUND; while(*p=='/')p++; }
    *out=n; return VFS_OK;
}
u64 vfs_read(vfs_node_t *n,u64 o,u64 s,void *b) { return n && n->ops && n->ops->read ? n->ops->read(n,o,s,b) : 0; }

int main(int argc,char **argv) {
    FILE *f; block_device_t d; vfs_super_t *sb=NULL; char *big;
    if (argc!=2) return 2; f=fopen(argv[1],"rb"); if(!f) return 3;
    memset(&d,0,sizeof(d)); d.sector_size=512; d.sector_count=131072; d.read=image_read; d.driver_data=f;
    if (mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK) { fprintf(stderr,"mount failed: %s\n",mgfs_last_error()); return 4; }
    test_root=sb->root_node;
    if (!check("/one-byte.txt","Z",0,99) || !check("/inline-56.txt","QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ",0,56) ||
        !check_fill("/extent-57.txt",'X',57,0,57) ||
        !check("/docs/nested.txt","nested",0,6)) return 5;
    big=malloc(13000); memset(big,'A',4096); memset(big+4096,'B',128);
    { vfs_node_t *n; if(vfs_lookup("/block-boundary.txt",&n)!=VFS_OK || vfs_read(n,4000,224,big)!=224 || big[96]!='B') return 6; }
    { vfs_node_t *n; if(vfs_lookup("/multi-extent.txt",&n)!=VFS_OK || vfs_read(n,4090,20,big)!=20) return 7; for (u64 i=0;i<6;i++) if(big[i]!='M') return 7; for (u64 i=6;i<20;i++) if(big[i]!='N') return 7; }
    { vfs_node_t *n; if(vfs_lookup("/extent-list.txt",&n)!=VFS_OK || vfs_read(n,0,12288,big)!=12288) return 8; for (u64 i=0;i<4096;i++) if(big[i]!='L') return 8; for (u64 i=4096;i<8192;i++) if(big[i]!='R') return 8; for (u64 i=8192;i<12288;i++) if(big[i]!='S') return 8; }
    { vfs_node_t *n; if(vfs_lookup("/empty.txt",&n)!=VFS_OK || vfs_read(n,0,20,big)!=0) return 9; }
    puts("MGFS regular-file read fixtures passed"); fclose(f); return 0;
}
