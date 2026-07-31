#include <stdio.h>
#include <stdlib.h>
#include <block.h>
#include <heap.h>
#include <vfs.h>
#include <storage/mgfs.h>
void *kmalloc(usize n){return malloc((size_t)n);} void kfree(void*p){free(p);}
bool block_read(block_device_t*d,u64 l,u32 n,void*b){return d->read(d,l,n,b);} bool block_write(block_device_t*d,u64 l,u32 n,const void*b){return d->write(d,l,n,b);} int vfs_register_fs(vfs_fs_type_t*f){(void)f;return VFS_OK;}
static bool rd(block_device_t*d,u64 l,u32 n,void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fread(b,512,n,f)==n;} static bool wr(block_device_t*d,u64 l,u32 n,const void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fwrite(b,512,n,f)==n;}
static vfs_node_t*root; static vfs_node_t*mgfs_finddir(vfs_node_t*,const char*);
int vfs_lookup(const char*p,vfs_node_t**o){const char*q=p;vfs_node_t*n=root;while(*q=='/')q++;while(*q){char c[256];u64 l=0;while(*q&&*q!='/'){if(l<255)c[l++]=*q;q++;}c[l]=0;n=mgfs_finddir(n,c);if(!n)return VFS_ERR_NOT_FOUND;while(*q=='/')q++;}*o=n;return VFS_OK;}
u64 vfs_read(vfs_node_t*n,u64 o,u64 s,void*b){return n&&n->ops&&n->ops->read?n->ops->read(n,o,s,b):0;} u64 vfs_write(vfs_node_t*n,u64 o,u64 s,const void*b){return n&&n->ops&&n->ops->write?n->ops->write(n,o,s,b):0;}
#include "../kernel/src/storage/mgfs.c"
static int same(const u8*a,const u8*b,u64 n){while(n--)if(*a++!=*b++)return 0;return 1;}
int main(int ac,char**av){FILE*f;block_device_t d;vfs_super_t*sb=0;vfs_node_t*n;u8 a[5000],b[5000];u64 id; if(ac!=2)return 2;f=fopen(av[1],"r+b");if(!f)return 3;memset(&d,0,sizeof d);d.sector_size=512;d.sector_count=131072;d.read=rd;d.write=wr;d.driver_data=f;if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 4;root=sb->root_node;if(mgfs_create(root,"hello",&n)!=VFS_OK)return 5;id=n->inode;if(vfs_write(n,0,9,"abcdefghi")!=9||vfs_write(n,0,2,"hi")!=2||n->size!=2||vfs_read(n,0,500,a)!=2||!same(a,(u8*)"hi",2))return 6;memset(a,'Q',56);if(vfs_write(n,0,56,a)!=56||vfs_write(n,0,3,"hey")!=3||n->size!=3||vfs_read(n,0,57,b)!=3)return 7;memset(a,'X',57);if(vfs_write(n,0,57,a)!=57||vfs_write(n,0,3,"hey")!=3||n->size!=3||vfs_read(n,0,58,b)!=3)return 8;memset(a,'Z',5000);if(vfs_write(n,0,5000,a)!=5000||vfs_write(n,0,3,"end")!=3||n->size!=3)return 9;fflush(f);if(mgfs_unmount(sb)!=VFS_OK)return 10;rewind(f);if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 11;root=sb->root_node;if(vfs_lookup("/hello",&n)!=VFS_OK||n->inode!=id||n->size!=3||vfs_write(n,0,3,"fin")!=3||n->size!=3)return 12;puts("MGFS shrink/write tests passed");return 0;}
