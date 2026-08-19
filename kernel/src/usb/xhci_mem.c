#include <xhci.h>
#include <xhci_ring.h>
#include <xhci_context.h>
#include <stddef.h>
#include <pmm.h>
#include <vmm.h>

/* ==============================================================================
 * Mangrove OS Physical/Virtual Memory API Dependencies
 * * As per xHCI strict requirements, DMA structures (Rings, Contexts, DCBAA)
 * MUST be allocated from physically contiguous memory to prevent page-boundary 
 * crossing hardware faults. kmalloc/malloc cannot be used for these allocations.
 * ============================================================================== */

/* ==============================================================================
 * Core DMA Memory Wrappers
 * These back the ring allocation routines in xhci_ring.c
 * ============================================================================== */

void* xhci_dma_alloc(usize size, uintptr_t *phys_out) {
    if (size == 0) {
        return NULL;
    }

    /* * Mangrove OS currently only supports single-page frame allocations.
     * All xHCI structures in this implementation (Rings, DCBAA, Contexts) 
     * are strictly guaranteed to fit within a single 4 KiB page.
     */
    if (size > 4096) {
        return NULL;
    }

    /* Request a single physically contiguous 4 KiB frame from the PMM */
    phys_addr_t phys_addr = pmm_alloc_frame();
    if (!phys_addr) {
        return NULL;
    }

    /* Ordinary RAM uses the permanent direct map; unlike device MMIO this
     * never creates an identity mapping or publishes it to process CR3s. */
    void* virt_addr = phys_to_virt(phys_addr);
    if (!virt_addr) {
        pmm_free_frame(phys_addr);
        return NULL;
    }

    /* Zero the memory completely to prevent uninitialized controller behavior */
    u8* p = (u8*)virt_addr;
    for (usize i = 0; i < 4096; i++) {
        p[i] = 0;
    }

    if (phys_out) {
        *phys_out = (uintptr_t)phys_addr;
    }

    return virt_addr;
}

void xhci_dma_free(void* virt, usize size) {
    if (!virt || size == 0) {
        return;
    }
    
    /* Recover the physical address using the VMM */
    phys_addr_t phys = virt_to_phys(virt);
    
    /* * Mangrove OS currently lacks vmm_unmap_mmio().
     * The physical frame is returned to the PMM pool to prevent physical leaks,
     * but the virtual mapping will safely persist as stale until a VMM teardown. 
     */
    if (phys) {
        pmm_free_frame(phys);
    }
}


/* ==============================================================================
 * xHCI Specific Structural Allocators
 * Built on top of the DMA abstractions to guarantee 64-byte alignment limits.
 * Single page frame allocations naturally satisfy the 64-byte alignment requirement.
 * ============================================================================== */

/*
 * Allocates the Device Context Base Address Array (DCBAA).
 * The DCBAA contains a 64-bit physical pointer for each supported Slot, 
 * plus index 0 reserved for the Scratchpad Array.
 * * @param max_slots Evaluated from HCSPARAMS1
 * @param phys_out  Outputs the physical address to be written to DCBAAP.
 */
u64* xhci_alloc_dcbaa(u32 max_slots, uintptr_t *phys_out) {
    /* DCBAA needs max_slots + 1 entries to account for index 0.
       Max slots <= 255. 256 * 8 = 2048 bytes (Fits in 1 Page). */
    usize size = (max_slots + 1) * sizeof(u64);
    return (u64*)xhci_dma_alloc(size, phys_out);
}

/*
 * Allocates the Scratchpad Buffer Array and individually maps each Scratchpad Buffer.
 * Failure to supply scratchpads will result in a Host System Error (HSE).
 * * @param max_scratchpads Evaluated from HCSPARAMS2.
 * @param page_size       Evaluated from the PAGESIZE operational register (typically 4096).
 * @param phys_out        Outputs the physical address to be stored in DCBAA[0].
 */
u64* xhci_alloc_scratchpads(u32 max_scratchpads, u32 page_size, uintptr_t *phys_out) {
    if (max_scratchpads == 0) {
        if (phys_out) *phys_out = 0;
        return NULL;
    }

    /* Array size: max_scratchpads * 8 bytes. Fits in 1 Page. */
    usize array_size = max_scratchpads * sizeof(u64);
    u64* scratchpad_array = (u64*)xhci_dma_alloc(array_size, phys_out);
    
    if (!scratchpad_array) {
        return NULL;
    }

    /* Allocate each individual scratchpad buffer and populate the array */
    for (u32 i = 0; i < max_scratchpads; i++) {
        uintptr_t sp_phys = 0;
        
        /* Each buffer is typically 4096 bytes. Fits perfectly in 1 Page. */
        void* sp_virt = xhci_dma_alloc(page_size, &sp_phys);
        
        if (!sp_virt) {
            return NULL; 
        }
        
        /* Store physical address for the xHC hardware to consume */
        scratchpad_array[i] = (u64)sp_phys;
    }

    return scratchpad_array;
}

/*
 * Allocates an Event Ring Segment Table (ERST).
 * @param num_segments Number of segments (Entries in the table).
 * @param phys_out     Outputs physical address to be written to ERSTBA.
 */
xhci_erst_entry_t* xhci_alloc_erst(u32 num_segments, uintptr_t *phys_out) {
    if (num_segments == 0) return NULL;
    
    /* ERST segments are 16 bytes each. Size fits in 1 Page. */
    usize size = num_segments * sizeof(xhci_erst_entry_t);
    return (xhci_erst_entry_t*)xhci_dma_alloc(size, phys_out);
}

/*
 * Allocates an Output Device Context.
 * Written by the hardware in response to commands.
 * * @param csz      Context Size bit from HCCPARAMS1 (0 = 32-byte, 1 = 64-byte).
 * @param phys_out Outputs physical address to be written to DCBAA[SlotID].
 */
void* xhci_alloc_device_context(u32 csz, uintptr_t *phys_out) {
    usize ctx_entry_size = csz ? 64 : 32;
    usize total_size = 32 * ctx_entry_size;
    
    void *ptr = xhci_dma_alloc(total_size, phys_out);
    if (ptr) {
        __builtin_memset(ptr, 0, total_size);
    }
    return ptr;
}

/*
 * Allocates an Input Context.
 * Populated by software and passed to commands via the Command Ring.
 * * @param csz      Context Size bit from HCCPARAMS1 (0 = 32-byte, 1 = 64-byte).
 * @param phys_out Outputs physical address to be embedded in Command TRBs.
 */
void* xhci_alloc_input_context(u32 csz, uintptr_t *phys_out) {
    usize ctx_entry_size = csz ? 64 : 32;
    usize total_size = 33 * ctx_entry_size;
    
    void *ptr = xhci_dma_alloc(total_size, phys_out);
    if (ptr) {
        __builtin_memset(ptr, 0, total_size); // Ensure all unused and reserved fields start completely clean
    }
    return ptr;
}
