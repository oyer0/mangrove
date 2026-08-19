#include <bootstrap.h>
#include <address_layout.h>

#define BOOT_P  (1ULL << 0)
#define BOOT_W  (1ULL << 1)
#define BOOT_PS (1ULL << 7)
#define BOOT_NX (1ULL << 63)
#define BOOT_FRAME_MASK 0x000ffffffffff000ULL
#define HUGE_PAGE_SIZE 0x200000ULL

typedef struct { u64 entries[512]; } boot_table_t;

static void clear_table(boot_table_t *table)
{
    for (u32 i = 0; i < 512; i++) table->entries[i] = 0;
}

static EFI_STATUS alloc_table(EFI_PHYSICAL_ADDRESS *phys,
                              boot_table_t **table)
{
    EFI_STATUS status = memory_allocate_pages(
        AllocateAnyPages, EFI_LOADER_DATA, 1, phys);
    if (status != EFI_SUCCESS) return status;
    *table = (boot_table_t *)(uintptr_t)*phys;
    clear_table(*table);
    return EFI_SUCCESS;
}

static EFI_STATUS child_table(boot_table_t *parent, u32 index,
                              boot_table_t **child)
{
    EFI_PHYSICAL_ADDRESS phys;
    EFI_STATUS status;
    if (parent->entries[index] & BOOT_P) {
        if (parent->entries[index] & BOOT_PS) return EFI_UNSUPPORTED;
        *child = (boot_table_t *)(uintptr_t)
            (parent->entries[index] & BOOT_FRAME_MASK);
        return EFI_SUCCESS;
    }
    status = alloc_table(&phys, child);
    if (status != EFI_SUCCESS) return status;
    parent->entries[index] = phys | BOOT_P | BOOT_W;
    return EFI_SUCCESS;
}

static EFI_STATUS map_page(boot_table_t *root, u64 virt, u64 phys, u64 flags)
{
    boot_table_t *pdpt, *pd, *pt;
    u32 pml4_i = (virt >> 39) & 0x1ff;
    u32 pdpt_i = (virt >> 30) & 0x1ff;
    u32 pd_i = (virt >> 21) & 0x1ff;
    u32 pt_i = (virt >> 12) & 0x1ff;
    EFI_STATUS status;

    status = child_table(root, pml4_i, &pdpt);
    if (status != EFI_SUCCESS) return status;
    status = child_table(pdpt, pdpt_i, &pd);
    if (status != EFI_SUCCESS) return status;
    if (pd->entries[pd_i] & BOOT_PS) {
        u64 mapped = pd->entries[pd_i] & ~((u64)HUGE_PAGE_SIZE - 1);
        return mapped + (virt & (HUGE_PAGE_SIZE - 1)) == phys
            ? EFI_SUCCESS : EFI_UNSUPPORTED;
    }
    status = child_table(pd, pd_i, &pt);
    if (status != EFI_SUCCESS) return status;
    pt->entries[pt_i] = (phys & BOOT_FRAME_MASK) | flags | BOOT_P;
    return EFI_SUCCESS;
}

static EFI_STATUS map_huge(boot_table_t *root, u64 virt, u64 phys, u64 flags)
{
    boot_table_t *pdpt, *pd;
    EFI_STATUS status = child_table(root, (virt >> 39) & 0x1ff, &pdpt);
    if (status != EFI_SUCCESS) return status;
    status = child_table(pdpt, (virt >> 30) & 0x1ff, &pd);
    if (status != EFI_SUCCESS) return status;
    u32 index = (virt >> 21) & 0x1ff;
    u64 entry = (phys & ~(HUGE_PAGE_SIZE - 1)) | flags | BOOT_P | BOOT_PS;
    if (pd->entries[index] && pd->entries[index] != entry)
        return EFI_UNSUPPORTED;
    pd->entries[index] = entry;
    return EFI_SUCCESS;
}

static EFI_STATUS map_range(boot_table_t *root, u64 virt, u64 phys, u64 size,
                            u64 flags)
{
    u64 end = phys + size;
    while (phys < end) {
        if (!(virt & (HUGE_PAGE_SIZE - 1)) &&
            !(phys & (HUGE_PAGE_SIZE - 1)) &&
            end - phys >= HUGE_PAGE_SIZE) {
            EFI_STATUS status = map_huge(root, virt, phys, flags);
            if (status == EFI_SUCCESS) {
                virt += HUGE_PAGE_SIZE;
                phys += HUGE_PAGE_SIZE;
                continue;
            }
            /* A preceding unaligned descriptor may already have created a
             * PT for this PDE.  Fall back to its 4K leaves rather than
             * rejecting an otherwise compatible adjacent range. */
        }
        EFI_STATUS status = map_page(root, virt, phys, flags);
        if (status != EFI_SUCCESS) return status;
        virt += EFI_PAGE_SIZE;
        phys += EFI_PAGE_SIZE;
    }
    return EFI_SUCCESS;
}

static bool direct_map_type(u32 type)
{
    return type == EFI_LOADER_CODE ||
           type == EFI_LOADER_DATA ||
           type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA ||
           type == EFI_CONVENTIONAL_MEMORY ||
           type == EFI_ACPI_RECLAIM_MEMORY ||
           type == EFI_ACPI_MEMORY_NVS;
}

EFI_STATUS bootstrap_build_page_tables(
    MEMORY_MAP *Map,
    ELF_HEADER *Header,
    ELF_PROGRAM_HEADER *ProgramHeaders,
    u64 HandoffStackBase,
    u64 HandoffStackSize,
    u64 FramebufferBase,
    u64 FramebufferSize,
    u64 Rsdp,
    u64 BootInfoAddress,
    u64 BootInfoSize,
    u64 MemoryMapAddress,
    u64 MemoryMapSize,
    u64 HandoffCode,
    EFI_PHYSICAL_ADDRESS *Pml4)
{
    EFI_PHYSICAL_ADDRESS root_phys;
    boot_table_t *root;
    EFI_STATUS status = alloc_table(&root_phys, &root);
    if (status != EFI_SUCCESS) return status;

    /* Transitional identity window for the firmware handoff code, stack,
     * BootInfo and device framebuffer.  It is intentionally broad and is
     * replaced by the kernel's normal mappings after entry. */
    /* The handoff routine itself executes in this temporary identity window
     * immediately after CR3 is loaded.  It must therefore remain executable
     * until control reaches the high-half kernel entry. */
    status = map_range(root, 0, 0, 0x40000000ULL, BOOT_W);
    if (status != EFI_SUCCESS) return status;

    /* Keep only the firmware objects and handoff instructions needed across
     * the final CR3 load reachable through the temporary low identity map.
     * All ordinary RAM dereferences use PHYS_MAP_BASE instead. */
    u64 entries = Map->MemoryMapSize / Map->DescriptorSize;
    for (u64 i = 0; i < entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)
            ((u8 *)Map->MemoryMap + i * Map->DescriptorSize);
        if (direct_map_type(desc->Type)) {
            if (desc->PhysicalStart >= PHYS_MAP_LIMIT) return EFI_UNSUPPORTED;
            status = map_range(root, PHYS_MAP_BASE + desc->PhysicalStart,
                               desc->PhysicalStart,
                               desc->NumberOfPages * EFI_PAGE_SIZE,
                               BOOT_W | BOOT_NX);
            if (status != EFI_SUCCESS) return status;
        }
    }

    status = map_range(root, BootInfoAddress & ~(EFI_PAGE_SIZE - 1),
                       BootInfoAddress & ~(EFI_PAGE_SIZE - 1),
                       BootInfoSize + EFI_PAGE_SIZE, BOOT_W | BOOT_NX);
    if (status != EFI_SUCCESS) return status;
    status = map_range(root, MemoryMapAddress & ~(EFI_PAGE_SIZE - 1),
                       MemoryMapAddress & ~(EFI_PAGE_SIZE - 1),
                       MemoryMapSize + EFI_PAGE_SIZE, BOOT_W | BOOT_NX);
    if (status != EFI_SUCCESS) return status;
    status = map_range(root, HandoffCode & ~(EFI_PAGE_SIZE - 1),
                       HandoffCode & ~(EFI_PAGE_SIZE - 1),
                       EFI_PAGE_SIZE, BOOT_W);
    if (status != EFI_SUCCESS) return status;
    status = map_range(root, HandoffStackBase, HandoffStackBase,
                       HandoffStackSize, BOOT_W | BOOT_NX);
    if (status != EFI_SUCCESS) return status;
    status = map_range(root, FramebufferBase, FramebufferBase,
                       FramebufferSize, BOOT_W | BOOT_NX);
    if (status != EFI_SUCCESS) return status;
    if (Rsdp) {
        status = map_range(root, Rsdp & ~(EFI_PAGE_SIZE - 1),
                           Rsdp & ~(EFI_PAGE_SIZE - 1), EFI_PAGE_SIZE,
                           BOOT_W | BOOT_NX);
        if (status != EFI_SUCCESS) return status;
    }

    /* Map each PT_LOAD at 4K granularity so ELF execute/write permissions are
     * preserved exactly.  Kernel mappings remain supervisor-only throughout. */
    for (u16 i = 0; i < Header->ProgramHeaderCount; i++) {
        ELF_PROGRAM_HEADER *ph = &ProgramHeaders[i];
        if (ph->Type != PT_LOAD) continue;
        u64 v = ph->VirtualAddress & ~(EFI_PAGE_SIZE - 1);
        u64 p = ph->PhysicalAddress & ~(EFI_PAGE_SIZE - 1);
        u64 pages = (ph->MemorySize + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
        u64 flags = (ph->Flags & 2) ? BOOT_W : 0;
        if (!(ph->Flags & 1)) flags |= BOOT_NX;
        for (u64 page = 0; page < pages; page++) {
            status = map_page(root, v + page * EFI_PAGE_SIZE,
                              p + page * EFI_PAGE_SIZE, flags);
            if (status != EFI_SUCCESS) return status;
        }
    }
    *Pml4 = root_phys;
    return EFI_SUCCESS;
}
