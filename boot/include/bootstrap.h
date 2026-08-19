#pragma once

#include <kloader.h>
#include <memory.h>

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
    EFI_PHYSICAL_ADDRESS *Pml4
);
