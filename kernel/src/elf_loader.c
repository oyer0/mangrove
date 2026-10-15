/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <elf_loader.h>
#include <process.h>
#include <vfs.h>
#include <vmm.h>
#include <pmm.h>
#include <heap.h>
#include <string.h>
#include <kprint.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifdef ELF_LOADER_DEBUG
#define ELF_BOOT_DIAG(...) kprint(__VA_ARGS__)
#else
#define ELF_BOOT_DIAG(...) ((void)0)
#endif

#define ELF_CLASS_64       2
#define ELF_DATA_LSB       1
#define ELF_VERSION        1
#define ELF_TYPE_EXEC      2
#define ELF_MACHINE_X86_64 62
#define ELF_PT_LOAD        1
#define ELF_PF_X           1U
#define ELF_PF_W           2U
#define ELF_PF_R           4U
#define ELF_USER_LIMIT     0x0000800000000000ULL
#define ELF_STACK_TOP      0x00007ffffff00000ULL
#define ELF_STACK_PAGES    8U

typedef struct {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_header_t;

typedef struct {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr_t;

static bool elf_load_fail(const char *reason)
{
    kprint("[ELF] load failed: %s\n", reason);
    return false;
}

static bool add_ok(u64 left, u64 right, u64 *result)
{
    if (left > ~(u64)0 - right) return false;
    *result = left + right;
    return true;
}

static bool align_up_page(u64 value, u64 *result)
{
    if (value > ~(u64)0 - 0xfffULL) return false;
    *result = (value + 0xfffULL) & ~0xfffULL;
    return true;
}

static bool read_at(vfs_file_handle_t *handle, u64 offset, void *buffer,
                    u64 size, int *error)
{
    int seek_result;
    u64 read;

    seek_result = vfs_seek(handle, (i64)offset, VFS_SEEK_SET, NULL);
    if (seek_result != VFS_OK) {
        ELF_BOOT_DIAG("[ELF-READ] seek off=%llu size=%llu result=%d\n",
                      offset, size, seek_result);
        *error = seek_result;
        return false;
    }
    int result = vfs_file_read_status(handle, size, buffer, &read);
    ELF_BOOT_DIAG("[ELF-READ] off=%llu size=%llu got=%llu node=%llu\n",
                  offset, size, read, handle && handle->node
                      ? handle->node->size : 0ULL);
    if (result != VFS_OK || read != size) {
        *error = result != VFS_OK ? result : VFS_ERR_IO;
        return false;
    }
    return true;
}

static bool map_segment_page(process_t *process, vfs_file_handle_t *handle,
                             const elf64_phdr_t *phdr, u64 page,
                             u64 segment_end, int *error)
{
    u64 frame_start = page;
    u64 frame_end = page + 0x1000ULL;
    u64 file_start;
    u64 file_end;
    u64 copy_start;
    u64 copy_end;
    phys_addr_t frame;
    u64 flags = PTE_USER;
    u8 *memory;

    frame = pmm_alloc_frame();
    if (!frame) { *error=VFS_ERR_NO_MEM; return elf_load_fail("segment frame allocation"); }

    memory = (u8 *)phys_to_virt(frame);
    if (!memory || !vmm_direct_map_valid(frame)) {
        return elf_load_fail("segment frame direct map");
    }
    memset(memory, 0, 0x1000);

    if (phdr->flags & ELF_PF_W) flags |= PTE_READWRITE;
    if (!(phdr->flags & ELF_PF_X)) flags |= PTE_NX;
    if (!vmm_map_user_page(process->address_space,
                           (void *)(uintptr_t)page, frame, flags)) {
        pmm_free_frame(frame);
        return elf_load_fail("segment user mapping");
    }

    if (!phdr->filesz) return true;
    if (!add_ok(phdr->vaddr, phdr->filesz, &file_end)) {
        return elf_load_fail("segment file range overflow");
    }
    file_start = phdr->vaddr;
    if (file_start < frame_start) file_start = frame_start;
    copy_start = file_start;
    copy_end = file_end < frame_end ? file_end : frame_end;
    if (copy_start >= copy_end || copy_start >= segment_end) return true;

    if (!add_ok(phdr->offset, copy_start - phdr->vaddr, &file_start) ||
        !read_at(handle, file_start, memory + (copy_start - frame_start),
                 copy_end - copy_start, error)) {
        return elf_load_fail("segment file read");
    }
    return true;
}

static bool elf_load_process_node_impl(struct process *process, vfs_node_t *node,
                      uintptr_t *entry_point, uintptr_t *stack_pointer, int *error)
{
    vfs_file_handle_t *handle = NULL;
    elf64_header_t header;
    elf64_phdr_t *phdrs = NULL;
    u64 file_size;
    u64 phdr_bytes;
    bool entry_valid = false;
    u16 i;
    int open_result;

    if (!process || !process->address_space || !node || !entry_point ||
        !stack_pointer) {
        return elf_load_fail("invalid loader arguments");
    }
    open_result = vfs_open_node(node, VFS_OPEN_READ, &handle);
    if (open_result != VFS_OK) {
        *error = open_result;
        kprint("[ELF] vfs_open failed: %d\n", open_result);
        return false;
    }
    if (!handle || !handle->node) {
        if (handle) vfs_close(handle);
        return elf_load_fail("open returned no node");
    }
    if (handle->node->type != VFS_TYPE_FILE) {
        vfs_close(handle);
        return elf_load_fail("opened node is not a file");
    }
    file_size = handle->node->size;
    ELF_BOOT_DIAG("[ELF] loading inode=%llu size=%llu\n", node->inode, file_size);
    if (file_size < sizeof(header) || !read_at(handle, 0, &header, sizeof(header), error) ||
        header.ident[0] != 0x7f || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != ELF_CLASS_64 || header.ident[5] != ELF_DATA_LSB ||
        header.ident[6] != ELF_VERSION || header.type != ELF_TYPE_EXEC ||
        header.machine != ELF_MACHINE_X86_64 || header.version != ELF_VERSION ||
        header.ehsize != sizeof(header) || header.phentsize != sizeof(elf64_phdr_t) ||
        !header.phnum || !add_ok(header.phoff,
                                 (u64)header.phnum * header.phentsize,
                                 &phdr_bytes) || phdr_bytes > file_size) {
        vfs_close(handle);
        return elf_load_fail("ELF header validation");
    }

    phdrs = (elf64_phdr_t *)kmalloc((usize)header.phnum * sizeof(*phdrs));
    if (!phdrs) *error=VFS_ERR_NO_MEM;
    if (!phdrs || !read_at(handle, header.phoff, phdrs,
                           (u64)header.phnum * sizeof(*phdrs), error)) {
        if (phdrs) kfree(phdrs);
        vfs_close(handle);
        return elf_load_fail("program header read");
    }

    for (i = 0; i < header.phnum; i++) {
        elf64_phdr_t *phdr = &phdrs[i];
        u64 segment_end;
        u64 page_end;
        u64 page;

        if (phdr->type != ELF_PT_LOAD) continue;
        if (!phdr->memsz || phdr->filesz > phdr->memsz ||
            !add_ok(phdr->offset, phdr->filesz, &segment_end) ||
            segment_end > file_size ||
            !add_ok(phdr->vaddr, phdr->memsz, &segment_end) ||
            phdr->vaddr < 0x1000 || segment_end > ELF_USER_LIMIT ||
            (phdr->flags & ~(ELF_PF_R | ELF_PF_W | ELF_PF_X)) ||
            (phdr->align && (phdr->align & (phdr->align - 1)))) {
            kfree(phdrs);
            vfs_close(handle);
            return elf_load_fail("load segment validation");
        }
        if (phdr->align > 1 &&
            ((phdr->offset & (phdr->align - 1)) !=
             (phdr->vaddr & (phdr->align - 1)))) {
            kfree(phdrs);
            vfs_close(handle);
            return elf_load_fail("load segment alignment");
        }
        ELF_BOOT_DIAG("[ELF-SEG] inode=%llu off=%llu filesz=%llu vaddr=%llu memsz=%llu\n",
                      node->inode, phdr->offset, phdr->filesz, phdr->vaddr,
                      phdr->memsz);
        if (!align_up_page(segment_end, &page_end)) {
            kfree(phdrs);
            vfs_close(handle);
            return elf_load_fail("load segment end alignment");
        }
        page = phdr->vaddr & ~0xfffULL;
        while (page < page_end) {
            if (!map_segment_page(process, handle, phdr, page, segment_end, error)) {
                kfree(phdrs);
                vfs_close(handle);
                return false;
            }
            page += 0x1000;
        }
        if (header.entry >= phdr->vaddr && header.entry < segment_end &&
            (phdr->flags & ELF_PF_X)) {
            entry_valid = true;
        }
    }

    if (!entry_valid || header.entry >= ELF_USER_LIMIT) {
        kfree(phdrs);
        vfs_close(handle);
        return elf_load_fail("entry point validation");
    }

    for (i = 0; i < ELF_STACK_PAGES; i++) {
        phys_addr_t frame = pmm_alloc_frame();
        if (!frame) {
            *error=VFS_ERR_NO_MEM;
            kfree(phdrs);
            vfs_close(handle);
            return elf_load_fail("stack frame allocation");
        }
        if (!vmm_direct_map_valid(frame)) {
            kfree(phdrs);
            vfs_close(handle);
            return elf_load_fail("stack frame direct map");
        }
        if (i == 0) {
            process->top_stack_frame = frame;
        }
        if (!vmm_map_user_page(process->address_space,
                               (void *)(uintptr_t)
                                   (ELF_STACK_TOP - (i + 1) * 0x1000ULL),
                               frame, PTE_USER | PTE_READWRITE | PTE_NX)) {
            pmm_free_frame(frame);
            kfree(phdrs);
            vfs_close(handle);
            return elf_load_fail("stack user mapping");
        }
    }

    process->entry_point = (uintptr_t)header.entry;
    process->user_stack_top = ELF_STACK_TOP;
    process->image_loaded = true;
    *entry_point = process->entry_point;
    *stack_pointer = process->user_stack_top - 16;
    kfree(phdrs);
    vfs_close(handle);
    return true;
}

int elf_load_process_node_result(struct process *process, vfs_node_t *node,
                                  uintptr_t *entry_point, uintptr_t *stack_pointer)
{
    int error=VFS_ERR_BAD_FORMAT;
    return elf_load_process_node_impl(process,node,entry_point,stack_pointer,&error)
        ? VFS_OK : error;
}

bool elf_load_process_node(struct process *process, vfs_node_t *node,
                            uintptr_t *entry_point, uintptr_t *stack_pointer)
{
    return elf_load_process_node_result(process,node,entry_point,stack_pointer)==VFS_OK;
}

static bool elf_path_access(const vfs_node_t *node, void *context)
{
    (void)context;
    return vfs_check_access(node, VFS_ACCESS_READ);
}

/* Kernel callers with absolute paths retain the resolved identity throughout
 * loading, just like native process launch. */
bool elf_load_process(struct process *process, const char *path,
                      uintptr_t *entry_point, uintptr_t *stack_pointer)
{
    VFS_LOCATION_AUTO vfs_location_t *location = NULL;
    if (!path || path[0] != '/' ||
        vfs_location_lookup(NULL, path, elf_path_access, NULL, &location) != VFS_OK)
        return false;
    return elf_load_process_node(process, vfs_location_node(location),
                                  entry_point, stack_pointer);
}
