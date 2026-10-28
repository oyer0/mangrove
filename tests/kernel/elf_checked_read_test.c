/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production ELF/VFS/MGFS; controlled host paging and transport. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mg/error.h>
#include "../filesystems/mfqs/host.h"
#include "storage/mgfs.c"
#define ELF_LOADER_DEBUG
#include "elf_loader.c"
#include "elf_native_error.inc"
void kprint(const char *format,...){(void)format;}
static u8 frames[16][4096] __attribute__((aligned(4096)));
static unsigned allocated,mapped;
static usize tls_file_size,tls_memory_size,tls_alignment;
static u8 tls_observed[16];
static bool tls_observed_present,stack_reservation_observed;
phys_addr_t pmm_alloc_frame(void)
{assert(allocated<16);return (uintptr_t)frames[allocated++];}
void pmm_free_frame(phys_addr_t frame){(void)frame;}
void *phys_to_virt(phys_addr_t frame){return (void *)(uintptr_t)frame;}
bool vmm_direct_map_valid(phys_addr_t frame)
{return frame>=(uintptr_t)frames&&frame<(uintptr_t)(frames+16);}
bool process_user_register_initial_stack(process_t *process,uintptr_t top,
                                         usize pages)
{assert(process&&top==PROCESS_USER_STACK_TOP&&pages==8U);stack_reservation_observed=true;return true;}
bool process_user_tls_template_adopt(process_t *process,u8 *image,
                                     usize file_size,usize memory_size,
                                     usize alignment)
{
    assert(process&&file_size<=sizeof(tls_observed));
    tls_file_size=file_size;tls_memory_size=memory_size;tls_alignment=alignment;
    memset(tls_observed,0,sizeof(tls_observed));
    if(file_size)memcpy(tls_observed,image,file_size);
    tls_observed_present=true;
    free(image);
    return true;
}
bool vmm_map_user_page(page_table_t *table,void *address,phys_addr_t frame,u64 flags)
{assert(table==(void *)1 && address && vmm_direct_map_valid(frame) && (flags&PTE_USER));mapped++;return true;}
static int load(vfs_node_t *node)
{
    allocated=mapped=0;memset(frames,0xcc,sizeof(frames));
    tls_file_size=tls_memory_size=tls_alignment=0;
    tls_observed_present=false;stack_reservation_observed=false;
    process_t process={0};process.address_space=(void *)1;
    uintptr_t entry=111,stack=222;
    int result=elf_load_process_node_result(&process,node,&entry,&stack);
    if(result==VFS_OK){
        assert(process.image_loaded && entry==0x400000 && allocated==10 && mapped==10);
        assert(stack_reservation_observed);
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

    /* Exercise both initialized static TLS and PT_TLS containing only
       zero-filled storage. The latter has no file-offset congruence rule. */
    elf64_phdr_t tls={.type=7,.flags=4,.offset=4104,.vaddr=0x400008,
        .filesz=4,.memsz=16,.align=8};
    header.phnum=2;
    memcpy(contents,&header,sizeof(header));
    memcpy(contents+64,&phdr,sizeof(phdr));
    memcpy(contents+64+sizeof(phdr),&tls,sizeof(tls));
    assert(vfs_file_write_status(handle,sizeof(contents),contents,&bytes)==VFS_OK&&bytes==sizeof(contents));
    assert(load(node)==VFS_OK);
    assert(tls_observed_present&&tls_file_size==4&&tls_memory_size==16&&tls_alignment==8);
    for(unsigned i=0;i<4;i++)assert(tls_observed[i]==contents[4104+i]);

    tls.offset=sizeof(contents);tls.vaddr=0x400020;tls.filesz=0;
    tls.memsz=9;tls.align=8;
    memcpy(contents+64+sizeof(phdr),&tls,sizeof(tls));
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(handle,sizeof(contents),contents,&bytes)==VFS_OK&&bytes==sizeof(contents));
    assert(load(node)==VFS_OK);
    assert(tls_observed_present&&tls_file_size==0&&tls_memory_size==9&&tls_alignment==8);

    /* GNU ld emits a zero-sized PT_TLS header for an executable with no TLS
       input sections when the native linker script declares the segment. */
    tls=(elf64_phdr_t){.type=7,.flags=4,.align=8};
    memcpy(contents+64+sizeof(phdr),&tls,sizeof(tls));
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(handle,sizeof(contents),contents,&bytes)==VFS_OK&&bytes==sizeof(contents));
    assert(load(node)==VFS_OK);
    assert(!tls_observed_present);

    tls.offset=1;
    memcpy(contents+64+sizeof(phdr),&tls,sizeof(tls));
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(handle,sizeof(contents),contents,&bytes)==VFS_OK&&bytes==sizeof(contents));
    assert(load(node)==VFS_ERR_BAD_FORMAT);

    tls=(elf64_phdr_t){.type=7,.flags=4,.offset=4104,.vaddr=0x400008,
        .filesz=4,.memsz=16,.align=8};
    tls.align=3;
    memcpy(contents+64+sizeof(phdr),&tls,sizeof(tls));
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(handle,sizeof(contents),contents,&bytes)==VFS_OK&&bytes==sizeof(contents));
    assert(load(node)==VFS_ERR_BAD_FORMAT);

    tls.align=8;
    header.phnum=3;
    memcpy(contents,&header,sizeof(header));
    memcpy(contents+64+2*sizeof(phdr),&tls,sizeof(tls));
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(handle,64+3*sizeof(phdr),contents,&bytes)==VFS_OK&&bytes==64+3*sizeof(phdr));
    assert(load(node)==VFS_ERR_BAD_FORMAT);

    header.phnum=1;
    memcpy(contents,&header,sizeof(header));
    /* Executable segments must never occupy the reserved argument/stack area. */
    header.entry=phdr.vaddr=PROCESS_USER_ARGUMENT_BASE;
    memcpy(contents,&header,sizeof(header));memcpy(contents+64,&phdr,sizeof(phdr));
    assert(vfs_file_write_status(handle,120,contents,&bytes)==VFS_OK&&bytes==120);
    assert(load(node)==VFS_ERR_BAD_FORMAT && !allocated && !mapped);
    header.entry=phdr.vaddr=0x400000;
    memcpy(contents,&header,sizeof(header));memcpy(contents+64,&phdr,sizeof(phdr));
    assert(vfs_seek(handle,0,VFS_SEEK_SET,NULL)==VFS_OK);
    assert(vfs_file_write_status(handle,120,contents,&bytes)==VFS_OK&&bytes==120);
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
