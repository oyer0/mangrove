#include <stdio.h>
#include <stdlib.h>
#include <block.h>
#include <heap.h>
#include <vfs.h>
#include <storage/mgfs.h>
void *kmalloc(usize n){return malloc((size_t)n);} void kfree(void*p){free(p);}
bool block_read(block_device_t*d,u64 l,u32 n,void*b){return d->read(d,l,n,b);} bool block_write(block_device_t*d,u64 l,u32 n,const void*b){return d->write(d,l,n,b);}
static bool rd(block_device_t*d,u64 l,u32 n,void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fread(b,512,n,f)==n;}
static bool wr(block_device_t*d,u64 l,u32 n,const void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fwrite(b,512,n,f)==n;}
#include "../kernel/src/vfs.c"
#include "../kernel/src/storage/mgfs.c"
int main(int ac,char**av){FILE*f;block_device_t d;vfs_node_t *root,*old,*node,*a,*b,*file,*pre_a,*pre_b;vfs_super_t*sb;char buf[16]={0};vfs_dirent_t e;u32 i;
 if(ac!=2)return 2;f=fopen(av[1],"r+b");if(!f)return 3;memset(&d,0,sizeof d);d.sector_size=512;fseek(f,0,SEEK_END);d.sector_count=(u64)ftell(f)/512ULL;rewind(f);d.read=rd;d.write=wr;d.driver_data=f;vfs_init();mgfs_init();if(vfs_mount_root("mgfs",&d)!=VFS_OK)return 4;root=vfs_get_root_node();
 if(vfs_mkdir(root,"pre_a",&pre_a)!=VFS_OK||vfs_mkdir(root,"pre_b",&pre_b)!=VFS_OK||
    vfs_lookup("/pre_a",&pre_a)!=VFS_OK||vfs_create(pre_a,"file",&file)!=VFS_OK||
    vfs_write(file,0,5,"hello")!=5||vfs_read(file,0,5,buf)!=5||strcmp(buf,"hello")!=0)return 5;
 if(vfs_create(root,"old",&old)!=VFS_OK||vfs_write(old,0,5,"hello")!=5||vfs_rename(root,"old",root,"new")!=VFS_OK)return 6;
 if(vfs_lookup("/new",&node)!=VFS_OK||vfs_lookup("/old",&node)==VFS_OK)return 7;
 i=0;while(vfs_readdir(root,i++,&e));
 if(vfs_mkdir(root,"a",&a)!=VFS_OK||vfs_mkdir(root,"b",&b)!=VFS_OK||vfs_lookup("/a",&a)!=VFS_OK||vfs_create(a,"file",&file)!=VFS_OK||vfs_write(file,0,8,"mangrove")!=8||vfs_lookup("/a/file",&node)!=VFS_OK||vfs_read(node,0,8,buf)!=8||strcmp(buf,"mangrove")!=0)return 8;
 {int rc=vfs_create(root,"old",&old);u64 wrn,rdn;if(rc!=VFS_OK)return 9;wrn=vfs_write(old,0,11,"replacement");rdn=vfs_read(old,0,11,buf);buf[11]='\0';if(wrn!=11||rdn!=11||strcmp(buf,"replacement")!=0||vfs_lookup("/new",&node)!=VFS_OK||vfs_read(node,0,5,buf)!=5)return 9;buf[5]='\0';if(strcmp(buf,"hello")!=0)return 9;}
 puts("MGFS VFS fresh-create/rename regression passed");return 0;}
