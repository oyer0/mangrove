#include <stdio.h>
#include <stdlib.h>
#include <block.h>
#include <heap.h>
#include <vfs.h>
#include <storage/mgfs.h>

void *kmalloc(usize n){return malloc((size_t)n);} void kfree(void*p){free(p);}
bool block_read(block_device_t*d,u64 l,u32 n,void*b){return d->read(d,l,n,b);}
bool block_write(block_device_t*d,u64 l,u32 n,const void*b){return d->write(d,l,n,b);}
int vfs_register_fs(vfs_fs_type_t*f){(void)f;return VFS_OK;}
static bool rd(block_device_t*d,u64 l,u32 n,void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fread(b,512,n,f)==n;}
static bool wr(block_device_t*d,u64 l,u32 n,const void*b){FILE*f=d->driver_data;fseek(f,(long)(l*512ULL),SEEK_SET);return fwrite(b,512,n,f)==n;}
static vfs_node_t *root; static vfs_node_t *mgfs_finddir(vfs_node_t*,const char*);
int vfs_lookup(const char*p,vfs_node_t**o){const char*q=p;vfs_node_t*n=root;while(*q=='/')q++;while(*q){char c[256];u64 l=0;while(*q&&*q!='/'){if(l<255)c[l++]=*q;q++;}c[l]=0;n=mgfs_finddir(n,c);if(!n)return VFS_ERR_NOT_FOUND;while(*q=='/')q++;}*o=n;return VFS_OK;}
int vfs_rename(vfs_node_t*a,const char*b,vfs_node_t*c,const char*d){return a&&a->ops&&a->ops->rename?a->ops->rename(a,b,c,d):VFS_ERR_UNSUPPORTED;}
bool vfs_readdir(vfs_node_t*n,u32 i,vfs_dirent_t*e){return n&&n->ops&&n->ops->readdir?n->ops->readdir(n,i,e):false;}
u64 vfs_read(vfs_node_t*n,u64 o,u64 s,void*b){return n&&n->ops&&n->ops->read?n->ops->read(n,o,s,b):0;}
u64 vfs_write(vfs_node_t*n,u64 o,u64 s,const void*b){return n&&n->ops&&n->ops->write?n->ops->write(n,o,s,b):0;}
#include "../kernel/src/storage/mgfs.c"

int main(int ac,char**av){
 FILE*f; block_device_t d; vfs_super_t*sb=0; vfs_node_t *a,*b,*old,*newn,*file,*sub,*tree,*found; char buf[16]={0};
 if(ac!=2)return 2; f=fopen(av[1],"r+b"); if(!f)return 3; memset(&d,0,sizeof d); d.sector_size=512; fseek(f,0,SEEK_END); d.sector_count=(u64)ftell(f)/512ULL; rewind(f); d.read=rd; d.write=wr; d.driver_data=f;
 if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 4; root=sb->root_node;
 if(mgfs_create(root,"old",&old)!=VFS_OK||vfs_write(old,0,5,"hello")!=5||
    vfs_rename(root,"old",root,"new")!=VFS_OK||vfs_lookup("/new",&newn)!=VFS_OK||
    vfs_lookup("/old",&found)==VFS_OK||newn->inode!=old->inode||vfs_read(newn,0,5,buf)!=5||strcmp(buf,"hello")!=0)return 5;
 {vfs_dirent_t e; u32 i=0; while(vfs_readdir(root,i++,&e));}
 if(mgfs_create(root,"old",&old)!=VFS_OK||vfs_write(old,0,9,"replacement")!=9||
    vfs_read(newn,0,5,buf)!=5||strcmp(buf,"hello")!=0)return 5;
 if(mgfs_mkdir(root,"a",&a)!=VFS_OK||mgfs_mkdir(root,"b",&b)!=VFS_OK||
    mgfs_create(a,"file",&file)!=VFS_OK||vfs_write(file,0,5,"mango")!=5||
    vfs_rename(a,"file",b,"file")!=VFS_OK||vfs_lookup("/a/file",&found)==VFS_OK||
    vfs_lookup("/b/file",&found)!=VFS_OK||vfs_read(found,0,5,buf)!=5||strcmp(buf,"mango")!=0)return 6;
 if(mgfs_mkdir(root,"oldtree",&old)!=VFS_OK||mgfs_mkdir(old,"sub",&sub)!=VFS_OK||
    mgfs_create(sub,"file",&file)!=VFS_OK||vfs_rename(root,"oldtree",root,"newtree")!=VFS_OK||
    vfs_lookup("/newtree/sub/file",&found)!=VFS_OK)return 7;
 if(mgfs_mkdir(root,"tree",&tree)!=VFS_OK||mgfs_mkdir(tree,"child",&sub)!=VFS_OK||
    vfs_rename(root,"tree",sub,"tree")!=VFS_ERR_INVALID_PARAM||
    vfs_rename(root,"newtree",root,"a")==VFS_OK||vfs_rename(root,"newtree",root,"newtree")!=VFS_OK)return 8;
 fflush(f); if(mgfs_unmount(sb)!=VFS_OK)return 9; rewind(f); if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 10; root=sb->root_node;
 if(vfs_lookup("/new",&found)!=VFS_OK||vfs_lookup("/b/file",&found)!=VFS_OK||vfs_lookup("/newtree/sub/file",&found)!=VFS_OK)return 11;
 puts("MGFS rename/move tests passed"); return 0;
}
