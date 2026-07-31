#include <xhci_ring.h>
#include <xhci_trb.h>
#include <stddef.h>

/* ==============================================================================
 * External DMA Memory Interfaces
 * These will be implemented in xhci_mem.c. They must guarantee physically 
 * contiguous, non-pageable memory.
 * ============================================================================== */
extern void* xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void  xhci_dma_free(void *virt, usize size);


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/* * Hardware write barrier.
 * Strictly guarantees that all TRB parameters are committed to physical RAM 
 * before the Cycle Bit is flipped. Without this, the xHC might fetch a TRB 
 * with a valid cycle bit but garbage parameters due to CPU out-of-order execution.
 */
static inline void xhci_memory_barrier(void) {
    __asm__ volatile("sfence" ::: "memory");
}

/* Freestanding zero-fill */
static void xhci_zero_mem(void *ptr, usize bytes) {
    u8 *p = (u8 *)ptr;
    for (usize i = 0; i < bytes; i++) {
        p[i] = 0;
    }
}


/* ==============================================================================
 * Public Ring API
 * ============================================================================== */

xhci_status_t xhci_ring_alloc(xhci_ring_t *ring, u32 num_trbs, bool is_event_ring) {
    if (!ring || num_trbs == 0) {
        return XHCI_ERR_INVALID_PARAM;
    }

    uintptr_t phys = 0;
    void *virt = xhci_dma_alloc(num_trbs * sizeof(xhci_trb_t), &phys);
    
    if (!virt) {
        return XHCI_ERR_NO_MEMORY;
    }

    xhci_zero_mem(virt, num_trbs * sizeof(xhci_trb_t));

    ring->trbs = (xhci_trb_t *)virt;
    ring->phys_base = phys;
    ring->size = num_trbs;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_state = 1; /* xHC expects Initial Producer/Consumer Cycle State to be 1 */
    ring->is_event_ring = is_event_ring;

    return XHCI_SUCCESS;
}

void xhci_ring_free(xhci_ring_t *ring) {
    if (ring && ring->trbs) {
        xhci_dma_free(ring->trbs, ring->size * sizeof(xhci_trb_t));
        xhci_zero_mem(ring, sizeof(xhci_ring_t));
    }
}

xhci_status_t xhci_ring_enqueue(xhci_ring_t *ring, u32 param1, u32 param2, u32 status, u32 control) {
    if (!ring || ring->is_event_ring) {
        return XHCI_ERR_INVALID_PARAM;
    }

    /* * Calculate the next index to ensure the ring isn't full.
     * We account for the Link TRB boundary when wrapping.
     */
    u32 next_idx = ring->enqueue_idx + 1;
    if (next_idx == ring->size - 1) {
        next_idx = 0;
    }

    if (next_idx == ring->dequeue_idx) {
        return XHCI_ERR_NO_MEMORY; /* Ring Overrun */
    }

    xhci_trb_t *trb = &ring->trbs[ring->enqueue_idx];

    /* Write standard fields */
    trb->param1 = param1;
    trb->param2 = param2;
    trb->status = status;

    /* Write control field, forcefully masking out the incoming cycle bit */
    u32 safe_control = control & ~XHCI_TRB_CTRL_CYCLE;
    trb->control = safe_control;

    xhci_memory_barrier();

    /* Apply software cycle bit to hand ownership to hardware */
    if (ring->cycle_state) {
        trb->control |= XHCI_TRB_CTRL_CYCLE;
    }

    ring->enqueue_idx++;

    /* * If the enqueue pointer hits the final element, we must evaluate the Link TRB.
     * The Link TRB is programmed dynamically here to ensure its Cycle Bit toggles 
     * correctly every time the ring completely wraps.
     */
    if (ring->enqueue_idx == ring->size - 1) {
        xhci_trb_t *link_trb = &ring->trbs[ring->enqueue_idx];
        
        link_trb->param1 = XHCI_TRB_PARAM1_PTR(ring->phys_base);
        link_trb->param2 = XHCI_TRB_PARAM2_PTR(ring->phys_base);
        link_trb->status = 0;
        
        u32 link_ctrl = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_LINK) | XHCI_TRB_CTRL_TC;
        link_trb->control = link_ctrl;
        
        xhci_memory_barrier();
        
        if (ring->cycle_state) {
            link_trb->control |= XHCI_TRB_CTRL_CYCLE;
        }
        
        /* * Hardware traverses the link, sees TC=1, and toggles its internal Cycle State.
         * Software must now mirror this toggle and wrap back to index 0.
         */
        ring->cycle_state ^= 1;
        ring->enqueue_idx = 0;
    }

    return XHCI_SUCCESS;
}

xhci_status_t xhci_ring_reclaim_transfer(xhci_ring_t *ring, uintptr_t trb_phys)
{
    uintptr_t offset;
    u32 index;

    if (!ring || ring->is_event_ring || !ring->trbs ||
        trb_phys < ring->phys_base) {
        return XHCI_ERR_INVALID_PARAM;
    }
    offset = trb_phys - ring->phys_base;
    if ((offset % sizeof(xhci_trb_t)) != 0 ||
        offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t))) {
        return XHCI_ERR_INVALID_PARAM;
    }
    index = (u32)(offset / sizeof(xhci_trb_t));
    if (index >= ring->size - 1) {
        return XHCI_ERR_INVALID_PARAM;
    }

    /* HID queues one TRB at a time; the completion pointer must identify
       the oldest outstanding TRB.  Reclaiming by pointer also remains
       correct when the producer has crossed the Link TRB. */
    if (index != ring->dequeue_idx) {
        return XHCI_ERR_INVALID_PARAM;
    }
    ring->dequeue_idx++;
    if (ring->dequeue_idx == ring->size - 1) {
        ring->dequeue_idx = 0;
    }
    return XHCI_SUCCESS;
}

xhci_trb_t* xhci_event_ring_get_next(xhci_ring_t *ring) {
    if (!ring || !ring->is_event_ring) {
        return NULL;
    }

    xhci_trb_t *trb = &ring->trbs[ring->dequeue_idx];

    /* * Hardware writes events using the current Consumer Cycle State (CCS).
     * If the cycle bit matches our software cycle state, the event is valid.
     */
    u32 trb_cycle = trb->control & XHCI_TRB_CTRL_CYCLE;
    u32 expected_cycle = ring->cycle_state ? XHCI_TRB_CTRL_CYCLE : 0;

    if (trb_cycle == expected_cycle) {
        return trb;
    }

    return NULL; /* No new events */
}

void xhci_event_ring_advance(xhci_ring_t *ring) {
    if (!ring || !ring->is_event_ring) {
        return;
    }

    ring->dequeue_idx++;

    /* * Event rings are structurally different from Command/Transfer rings.
     * They do not utilize a Link TRB. They wrap strictly based on the size 
     * defined in the ERST. When they wrap, the CCS naturally toggles.
     */
    if (ring->dequeue_idx == ring->size) {
        ring->dequeue_idx = 0;
        ring->cycle_state ^= 1;
    }
}
