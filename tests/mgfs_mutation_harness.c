#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static vfs_node_t*root;
static int same(const void*a,const void*b,u64 n){const u8*x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
static vfs_node_t *mgfs_finddir(vfs_node_t *, const char *);
int vfs_lookup(const char*p,vfs_node_t**o){const char*q=p;vfs_node_t*n=root;while(*q=='/')q++;while(*q){char c[256];u64 l=0;while(*q&&*q!='/'){if(l<255)c[l++]=*q;q++;}c[l]=0;if(n->type!=VFS_TYPE_DIRECTORY)return VFS_ERR_NOT_FOUND;n=mgfs_finddir(n,c);if(!n)return VFS_ERR_NOT_FOUND;while(*q=='/')q++;}*o=n;return VFS_OK;}
u64 vfs_read(vfs_node_t*n,u64 o,u64 s,void*b){return n&&n->ops&&n->ops->read?n->ops->read(n,o,s,b):0;}
u64 vfs_write(vfs_node_t*n,u64 o,u64 s,const void*b){return n&&n->ops&&n->ops->write?n->ops->write(n,o,s,b):0;}
#include "../kernel/src/storage/mgfs.c"
int main(int argc,char**argv){FILE*f;block_device_t d;vfs_super_t*sb=0;vfs_node_t*n=0,*docs=0,*nested=0,*longf;u64 hello_id; char buf[128], q[56]; if(argc!=2)return 2;f=fopen(argv[1],"r+b");if(!f)return 3;memset(&d,0,sizeof d);d.sector_size=512;d.sector_count=131072;d.read=rd;d.write=wr;d.driver_data=f;if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 4;root=sb->root_node;if(mgfs_create(root,"hello",&n)!=VFS_OK){fprintf(stderr,"create: %s\n",mgfs_last_error());return 5;}hello_id=n->inode;if(vfs_write(n,0,3,"abc")!=3||vfs_read(n,0,3,buf)!=3||!same(buf,"abc",3)){fprintf(stderr,"write: %s got=%llu\n",mgfs_last_error(),(unsigned long long)vfs_read(n,0,3,buf));return 6;}if(mgfs_create(root,"long",&longf)!=VFS_OK)return 7;memset(q,'Q',56);if(vfs_write(longf,0,56,q)!=56||vfs_write(longf,56,1,"R")!=1||vfs_read(longf,0,57,buf)!=57||buf[56]!='R')return 8;if(mgfs_create(root,"hello",&n)==VFS_OK||mgfs_create(root,"bad/name",&n)==VFS_OK)return 9;if(mgfs_mkdir(root,"docs2",&docs)!=VFS_OK){fprintf(stderr,"mkdir: %s\n",mgfs_last_error());return 10;}if(mgfs_create(docs,"nested",&nested)!=VFS_OK){fprintf(stderr,"nested: %s\n",mgfs_last_error());return 11;}fflush(f);if(mgfs_unmount(sb)!=VFS_OK)return 12;rewind(f);if(mgfs_mount(&mgfs_fs_type,&d,&sb)!=VFS_OK)return 13;root=sb->root_node;if(vfs_lookup("/hello",&n)!=VFS_OK||n->inode!=hello_id||vfs_read(n,0,3,buf)!=3||!same(buf,"abc",3)||vfs_lookup("/docs2/nested",&n)!=VFS_OK)return 14;puts("MGFS mutation/write/remount tests passed");return 0;}
