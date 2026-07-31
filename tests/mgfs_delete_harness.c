#include <stdio.h>
#include <stdlib.h>
#include <block.h>
#include <heap.h>
#include <vfs.h>
#include <storage/mgfs.h>
void *kmalloc(usize n){return malloc((size_t)n);} void kfree(void*p){free(p);} bool block_read(block_device_t*d,u64 l,u32 n,void*b){return d->read(d,l,n,b);} bool block_write(block_device_t*d,u64 l,u32 n,const void*b){return d->write(d,l,n,b);} int vfs_register_fs(vfs_fs_type_t*f){(void)f;return VFS_OK;}
static bool rd(block_device_t*d,u64 l,u32 n,void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fread(b,512,n,f)==n;} static bool wr(block_device_t*d,u64 l,u32 n,const void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fwrite(b,512,n,f)==n;}
static vfs_node_t*root; static vfs_node_t*mgfs_finddir(vfs_node_t*,const char*); int vfs_lookup(const char*p,vfs_node_t**o){const char*q=p;vfs_node_t*n=root;while(*q=='/')q++;while(*q){char c[256];u64 l=0;while(*q&&*q!='/'){if(l<255)c[l++]=*q;q++;}c[l]=0;n=mgfs_finddir(n,c);if(!n)return VFS_ERR_NOT_FOUND;while(*q=='/')q++;}*o=n;return VFS_OK;}
u64 vfs_read(vfs_node_t*n,u64 o,u64 s,void*b){return n&&n->ops&&n->ops->read?n->ops->read(n,o,s,b):0;} u64 vfs_write(vfs_node_t*n,u64 o,u64 s,const void*b){return n&&n->ops&&n->ops->write?n->ops->write(n,o,s,b):0;}
bool vfs_readdir(vfs_node_t*n,u32 i,vfs_dirent_t*e){return n&&n->ops&&n->ops->readdir?n->ops->readdir(n,i,e):false;}
#include "../kernel/src/storage/mgfs.c"
int main(int ac,char**av){FILE*f;block_device_t d;vfs_super_t*sb=0;vfs_node_t*a,*b,*dir,*child,*multi,*live,*keep,*gone,*reuse;char data[5000];if(ac!=2)return 2;f=fopen(av[1],"r+b");if(!f)return 3;memset(&d,0,sizeof d);d.sector_size=512;d.sector_count=131072;d.read=rd;d.write=wr;d.driver_data=f;if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 4;root=sb->root_node;
if(mgfs_create(root,"a",&a)!=VFS_OK||mgfs_create(root,"b",&b)!=VFS_OK||mgfs_mkdir(root,"dir",&dir)!=VFS_OK)return 5;
if(mgfs_create(dir,"child",&child)!=VFS_OK||mgfs_unlink(dir,"child")!=VFS_OK||vfs_readdir(dir,0,(vfs_dirent_t[1]){{0}})||mgfs_rmdir(root,"dir")!=VFS_OK)return 6;
if(mgfs_mkdir(root,"multi",&multi)!=VFS_OK||mgfs_create(multi,"one",&gone)!=VFS_OK||mgfs_create(multi,"two",&child)!=VFS_OK||mgfs_unlink(multi,"one")!=VFS_OK||mgfs_unlink(multi,"two")!=VFS_OK||mgfs_rmdir(root,"multi")!=VFS_OK)return 7;
if(mgfs_mkdir(root,"live",&live)!=VFS_OK||mgfs_create(live,"keep",&keep)!=VFS_OK||mgfs_create(live,"gone",&gone)!=VFS_OK||mgfs_unlink(live,"gone")!=VFS_OK||mgfs_rmdir(root,"live")==VFS_OK)return 8;
memset(data,'X',5000);if(vfs_write(a,0,5000,data)!=5000)return 9;if(mgfs_unlink(root,"a")!=VFS_OK||mgfs_unlink(root,"a")==VFS_OK)return 10;if(mgfs_rmdir(root,"/")==VFS_OK||mgfs_rmdir(root,"b")==VFS_OK||mgfs_unlink(root,"live")==VFS_OK)return 11;if(mgfs_create(root,"reuse",&reuse)!=VFS_OK)return 12;
fflush(f);if(mgfs_unmount(sb)!=VFS_OK)return 13;rewind(f);if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 14;root=sb->root_node;if(vfs_lookup("/b",&b)!=VFS_OK||vfs_lookup("/a",&a)==VFS_OK||vfs_lookup("/dir",&dir)==VFS_OK||vfs_lookup("/multi",&multi)==VFS_OK||vfs_lookup("/reuse",&reuse)!=VFS_OK||vfs_lookup("/live/keep",&keep)!=VFS_OK)return 15;puts("MGFS deletion/remount tests passed");return 0;}
