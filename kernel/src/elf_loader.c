#include <elf_loader.h>
#include <process.h>
#include <vfs.h>
#include <vmm.h>
#include <pmm.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void *)0)
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
                    u64 size)
{
    u64 read;

    if (vfs_seek(handle, (i64)offset, VFS_SEEK_SET, NULL) != VFS_OK) {
        return false;
    }
    read = vfs_file_read(handle, size, buffer);
    return read == size;
}

static bool map_segment_page(process_t *process, vfs_file_handle_t *handle,
                             const elf64_phdr_t *phdr, u64 page,
                             u64 segment_end)
{
    u64 frame_start = page;
    u64 frame_end = page + 0x1000ULL;
    u64 file_start;
    u64 file_end;
    u64 copy_start;
    u64 copy_end;
    u64 frame;
    u64 flags = PTE_USER;
    u8 *memory;

    frame = (u64)pmm_alloc_frame();
    if (!frame) return false;

    memory = (u8 *)(uintptr_t)frame;
    memset(memory, 0, 0x1000);

    if (phdr->flags & ELF_PF_W) flags |= PTE_READWRITE;
    if (!(phdr->flags & ELF_PF_X)) flags |= PTE_NX;
    vmm_map(process->address_space, (void *)(uintptr_t)page,
            (void *)(uintptr_t)frame, flags);

    if (!phdr->filesz) return true;
    if (!add_ok(phdr->vaddr, phdr->filesz, &file_end)) return false;
    file_start = phdr->vaddr;
    if (file_start < frame_start) file_start = frame_start;
    copy_start = file_start;
    copy_end = file_end < frame_end ? file_end : frame_end;
    if (copy_start >= copy_end || copy_start >= segment_end) return true;

    if (!add_ok(phdr->offset, copy_start - phdr->vaddr, &file_start) ||
        !read_at(handle, file_start, memory + (copy_start - frame_start),
                 copy_end - copy_start)) {
        return false;
    }
    return true;
}

bool elf_load_process(struct process *process, const char *path,
                      uintptr_t *entry_point, uintptr_t *stack_pointer)
{
    vfs_file_handle_t *handle = NULL;
    elf64_header_t header;
    elf64_phdr_t *phdrs = NULL;
    u64 file_size;
    u64 phdr_bytes;
    bool entry_valid = false;
    u16 i;

    if (!process || !process->address_space || !path || !entry_point ||
        !stack_pointer || vfs_open(path, VFS_OPEN_READ, &handle) != VFS_OK ||
        !handle || !handle->node || handle->node->type != VFS_TYPE_FILE) {
        if (handle) vfs_close(handle);
        return false;
    }
    file_size = handle->node->size;
    if (file_size < sizeof(header) || !read_at(handle, 0, &header, sizeof(header)) ||
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
        return false;
    }

    phdrs = (elf64_phdr_t *)kmalloc((usize)header.phnum * sizeof(*phdrs));
    if (!phdrs || !read_at(handle, header.phoff, phdrs,
                           (u64)header.phnum * sizeof(*phdrs))) {
        if (phdrs) kfree(phdrs);
        vfs_close(handle);
        return false;
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
            return false;
        }
        if (phdr->align > 1 &&
            ((phdr->offset & (phdr->align - 1)) !=
             (phdr->vaddr & (phdr->align - 1)))) {
            kfree(phdrs);
            vfs_close(handle);
            return false;
        }
        if (!align_up_page(segment_end, &page_end)) {
            kfree(phdrs);
            vfs_close(handle);
            return false;
        }
        page = phdr->vaddr & ~0xfffULL;
        while (page < page_end) {
            if (!map_segment_page(process, handle, phdr, page, segment_end)) {
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
        return false;
    }

    for (i = 0; i < ELF_STACK_PAGES; i++) {
        u64 frame = (u64)pmm_alloc_frame();
        if (!frame) {
            kfree(phdrs);
            vfs_close(handle);
            return false;
        }
        vmm_map(process->address_space,
                (void *)(uintptr_t)(ELF_STACK_TOP - (i + 1) * 0x1000ULL),
                (void *)(uintptr_t)frame, PTE_USER | PTE_READWRITE | PTE_NX);
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
