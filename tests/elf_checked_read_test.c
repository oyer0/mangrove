/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production ELF/VFS/MGFS; controlled host paging and transport. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mg/error.h>
#include "mfqs/host.h"
#include "storage/mgfs.c"
#define ELF_LOADER_DEBUG
#include "elf_loader.c"
#include "elf_native_error.inc"
void kprint(const char *format,...){(void)format;}
static u8 frames[16][4096] __attribute__((aligned(4096)));
static unsigned allocated,mapped;
phys_addr_t pmm_alloc_frame(void)
{assert(allocated<16);return (uintptr_t)frames[allocated++];}
void pmm_free_frame(phys_addr_t frame){(void)frame;}
void *phys_to_virt(phys_addr_t frame){return (void *)(uintptr_t)frame;}
bool vmm_direct_map_valid(phys_addr_t frame)
{return frame>=(uintptr_t)frames&&frame<(uintptr_t)(frames+16);}
bool vmm_map_user_page(page_table_t *table,void *address,phys_addr_t frame,u64 flags)
{assert(table==(void *)1 && address && vmm_direct_map_valid(frame) && (flags&PTE_USER));mapped++;return true;}
static int load(vfs_node_t *node)
{
    allocated=mapped=0;memset(frames,0xcc,sizeof(frames));
    process_t process={0};process.address_space=(void *)1;
    uintptr_t entry=111,stack=222;
    int result=elf_load_process_node_result(&process,node,&entry,&stack);
    if(result==VFS_OK){
        assert(process.image_loaded && entry==0x400000 && allocated==10 && mapped==10);
        for(unsigned i=0;i<4096;i++)assert(frames[0][i]==(u8)(i%251+1));
        assert(frames[1][0]==(u8)(4096%251+1));
        for(unsigned i=1;i<4096;i++)assert(!frames[1][i]);
    }else assert(!process.image_loaded && entry==111 && stack==222);
    return result;
}
int main(int argc,char **argv)
{
    assert(argc==2&&mfqs_media_open(argv[1]));vfs_init();assert(mgfs_init()==VFS_OK);
    assert(vfs_mount_root("mgfs",mfqs_device())==VFS_OK);
    u8 contents[8193]={0};
    elf64_header_t header={.ident={0x7f,'E','L','F',2,1,1},.type=2,.machine=62,.version=1,
        .entry=0x400000,.phoff=64,.ehsize=64,.phentsize=56,.phnum=1};
    elf64_phdr_t phdr={.type=1,.flags=5,.offset=4096,.vaddr=0x400000,.filesz=4097,.memsz=8192,.align=4096};
    memcpy(contents,&header,sizeof(header));memcpy(contents+64,&phdr,sizeof(phdr));
    for(unsigned i=4096;i<sizeof(contents);i++)contents[i]=(u8)((i-4096)%251+1);
    vfs_node_t *node=NULL;vfs_file_handle_t *handle=NULL;u64 bytes=0;
    assert(vfs_create(vfs_get_root_node(),"native.elf",&node)==VFS_OK);
    assert(vfs_open_node(node,VFS_OPEN_RDWR,&handle)==VFS_OK);
    assert(vfs_file_write_status(handle,sizeof(contents),contents,&bytes)==VFS_OK&&bytes==sizeof(contents));
    bool failed=false,success=false;
    for(u64 boundary=1;boundary<40;boundary++){
        mfqs_fail_repeated('r',boundary,2);
        int result=load(node);
        if(result==VFS_OK)success=true;
        else {assert(result==VFS_ERR_IO && syscall_spawn_error(result)==MG_ERR_IO);failed=true;}
    }
    assert(failed&&success);
    mfqs_fail_after('r',0,false,false);
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    char bad=0;assert(vfs_file_write_status(handle,1,&bad,&bytes)==VFS_OK&&bytes==1);
    assert(load(node)==VFS_ERR_BAD_FORMAT && syscall_spawn_error(VFS_ERR_BAD_FORMAT)==MG_ERR_INVALID_EXEC);
    assert(vfs_close(handle)==VFS_OK);
    mfqs_fail_after('r',1,false,true);
    assert(load(node)==VFS_ERR_DEVICE_GONE && syscall_spawn_error(VFS_ERR_DEVICE_GONE)==MG_ERR_DEVICE_GONE);
    vfs_device_removed(mfqs_device());mfqs_media_close();
    puts("Production ELF: exact segment bytes, all read boundaries, malformed format and removal preserve native status PASS");
    return 0;
}
