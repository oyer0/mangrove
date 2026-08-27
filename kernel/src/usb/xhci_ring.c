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

static void xhci_prepare_trb(xhci_trb_t *trb, u32 param1, u32 param2,
                             u32 status, u32 control, u32 producer_cycle)
{
    u32 safe_control = control & ~XHCI_TRB_CTRL_CYCLE;
    u32 unpublished_control = safe_control |
        (producer_cycle ? 0 : XHCI_TRB_CTRL_CYCLE);

    /* Retain producer ownership while every other field is replaced.  This
       matters after cycle-state wrap, where a zero Cycle bit would otherwise
       publish the TRB before its parameters are globally visible. */
    trb->control = unpublished_control;
    xhci_memory_barrier();
    trb->param1 = param1;
    trb->param2 = param2;
    trb->status = status;
    trb->control = unpublished_control;
    xhci_memory_barrier();
}

static void xhci_publish_prepared_trb(xhci_trb_t *trb, u32 producer_cycle)
{
    u32 control = trb->control & ~XHCI_TRB_CTRL_CYCLE;

    /* The Cycle bit is the ownership handoff.  All TRB fields and any later
       TRBs in the operation must be visible before this store. */
    xhci_memory_barrier();
    trb->control = control |
        (producer_cycle ? XHCI_TRB_CTRL_CYCLE : 0);
    xhci_memory_barrier();
}

static xhci_status_t xhci_ring_enqueue_internal(
    xhci_ring_t *ring, u32 param1, u32 param2, u32 status, u32 control,
    bool publish, uintptr_t *trb_phys_out, u32 *producer_cycle_out)
{
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

    u32 trb_index = ring->enqueue_idx;
    u32 producer_cycle = ring->cycle_state;
    xhci_trb_t *trb = &ring->trbs[trb_index];

    xhci_prepare_trb(trb, param1, param2, status, control, producer_cycle);
    if (trb_phys_out)
        *trb_phys_out = ring->phys_base +
            (uintptr_t)trb_index * sizeof(xhci_trb_t);
    if (producer_cycle_out)
        *producer_cycle_out = producer_cycle;

    ring->enqueue_idx++;

    /* * If the enqueue pointer hits the final element, we must evaluate the Link TRB.
     * The Link TRB is programmed dynamically here to ensure its Cycle Bit toggles 
     * correctly every time the ring completely wraps.
     */
    if (ring->enqueue_idx == ring->size - 1) {
        xhci_trb_t *link_trb = &ring->trbs[ring->enqueue_idx];
        
        u32 link_ctrl = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_LINK) | XHCI_TRB_CTRL_TC;
        xhci_prepare_trb(link_trb,
                         XHCI_TRB_PARAM1_PTR(ring->phys_base),
                         XHCI_TRB_PARAM2_PTR(ring->phys_base), 0,
                         link_ctrl, producer_cycle);
        xhci_publish_prepared_trb(link_trb, producer_cycle);
        
        /* * Hardware traverses the link, sees TC=1, and toggles its internal Cycle State.
         * Software must now mirror this toggle and wrap back to index 0.
         */
        ring->cycle_state ^= 1;
        ring->enqueue_idx = 0;
    }

    if (publish)
        xhci_publish_prepared_trb(trb, producer_cycle);

    return XHCI_SUCCESS;
}

xhci_status_t xhci_ring_enqueue(xhci_ring_t *ring, u32 param1, u32 param2,
                                u32 status, u32 control)
{
    return xhci_ring_enqueue_internal(ring, param1, param2, status, control,
                                      true, NULL, NULL);
}

xhci_status_t xhci_ring_enqueue_unpublished(
    xhci_ring_t *ring, u32 param1, u32 param2, u32 status, u32 control,
    uintptr_t *trb_phys_out, u32 *producer_cycle_out)
{
    if (!trb_phys_out || !producer_cycle_out)
        return XHCI_ERR_INVALID_PARAM;
    return xhci_ring_enqueue_internal(ring, param1, param2, status, control,
                                      false, trb_phys_out,
                                      producer_cycle_out);
}

xhci_status_t xhci_ring_publish_trb(xhci_ring_t *ring, uintptr_t trb_phys,
                                    u32 producer_cycle)
{
    uintptr_t offset;
    u32 index;

    if (!ring || ring->is_event_ring || !ring->trbs ||
        trb_phys < ring->phys_base || producer_cycle > 1)
        return XHCI_ERR_INVALID_PARAM;
    offset = trb_phys - ring->phys_base;
    if ((offset % sizeof(xhci_trb_t)) != 0 ||
        offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t)))
        return XHCI_ERR_INVALID_PARAM;
    index = (u32)(offset / sizeof(xhci_trb_t));
    if (index >= ring->size - 1)
        return XHCI_ERR_INVALID_PARAM;

    xhci_publish_prepared_trb(&ring->trbs[index], producer_cycle);
    return XHCI_SUCCESS;
}

static bool xhci_ring_phys_to_index(const xhci_ring_t *ring,
                                    uintptr_t trb_phys, u32 *index_out)
{
    uintptr_t offset;

    if (!ring || ring->is_event_ring || !ring->trbs || !index_out ||
        trb_phys < ring->phys_base)
        return false;
    offset = trb_phys - ring->phys_base;
    if ((offset % sizeof(xhci_trb_t)) != 0 ||
        offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t)))
        return false;
    *index_out = (u32)(offset / sizeof(xhci_trb_t));
    return *index_out < ring->size - 1;
}

bool xhci_ring_trb_in_range(const xhci_ring_t *ring, uintptr_t range_start,
                            uintptr_t range_end, uintptr_t candidate)
{
    u32 start_index;
    u32 end_index;
    u32 candidate_index;
    u32 index;

    if (!xhci_ring_phys_to_index(ring, range_start, &start_index) ||
        !xhci_ring_phys_to_index(ring, range_end, &end_index) ||
        !xhci_ring_phys_to_index(ring, candidate, &candidate_index))
        return false;

    index = start_index;
    for (u32 walked = 0; walked < ring->size - 1; walked++) {
        if (index == candidate_index)
            return true;
        if (index == end_index)
            return false;
        index++;
        if (index == ring->size - 1)
            index = 0;
    }
    return false;
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

xhci_status_t xhci_ring_reclaim_td(xhci_ring_t *ring,
                                   uintptr_t td_start_phys,
                                   uintptr_t td_end_phys)
{
    uintptr_t start_offset;
    uintptr_t end_offset;
    u32 start_index;
    u32 end_index;
    u32 index;

    if (!ring || ring->is_event_ring || !ring->trbs ||
        td_start_phys < ring->phys_base || td_end_phys < ring->phys_base) {
        return XHCI_ERR_INVALID_PARAM;
    }

    start_offset = td_start_phys - ring->phys_base;
    end_offset = td_end_phys - ring->phys_base;
    if ((start_offset % sizeof(xhci_trb_t)) != 0 ||
        (end_offset % sizeof(xhci_trb_t)) != 0 ||
        start_offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t)) ||
        end_offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t))) {
        return XHCI_ERR_INVALID_PARAM;
    }

    start_index = (u32)(start_offset / sizeof(xhci_trb_t));
    end_index = (u32)(end_offset / sizeof(xhci_trb_t));
    if (start_index >= ring->size - 1 || end_index >= ring->size - 1 ||
        start_index != ring->dequeue_idx) {
        return XHCI_ERR_INVALID_PARAM;
    }

    /* A TD cannot contain the Link TRB.  The bounded walk also rejects an
       end pointer that is not reachable from the oldest outstanding TRB. */
    index = start_index;
    for (u32 walked = 0; walked < ring->size - 1; walked++) {
        if (index == end_index) {
            index++;
            if (index == ring->size - 1)
                index = 0;
            ring->dequeue_idx = index;
            return XHCI_SUCCESS;
        }

        index++;
        if (index == ring->size - 1)
            index = 0;
    }

    return XHCI_ERR_INVALID_PARAM;
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
