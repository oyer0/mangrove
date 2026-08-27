#pragma once

#include <xhci.h>
#include <xhci_trb.h>

/* ==============================================================================
 * xHCI Ring Configuration Constants
 * ============================================================================== */

/* * A single 4 KiB page can hold exactly 256 TRBs (16 bytes each).
 * This is the standard size used for Command, Event, and Transfer rings
 * in this driver implementation.
 */
#define XHCI_RING_TRBS_PER_PAGE  256

/*
 * The maximum size of the Event Ring Segment Table (ERST) supported
 * by this driver. Since we allocate 1 page for the Event Ring,
 * we only need 1 segment.
 */
#define XHCI_ERST_MAX_SEGMENTS   1


/* ==============================================================================
 * Event Ring Segment Table (ERST) Entry (xHCI Spec 6.5)
 * * The ERST is an array of these structures used by the hardware to know 
 * where Event Ring segments are physically located in memory.
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u64 base_address; /* Physical address of the Event Ring segment */
    volatile u32 size;         /* Number of TRBs in this segment (e.g., 256) */
    volatile u32 rsvd;         /* Reserved, must be 0 */
} xhci_erst_entry_t;


/* ==============================================================================
 * Software Ring Tracker Structure
 * * Maintains the software state for Command, Event, and Transfer Rings.
 * This structure tracks the producer/consumer indices, the current cycle 
 * bit state (PCS for output rings, CCS for event rings), and maps both 
 * virtual and physical memory addresses.
 * ============================================================================== */

typedef struct {
    xhci_trb_t *trbs;         /* Virtual pointer to the TRB array (DMA mapped) */
    uintptr_t  phys_base;     /* Physical base address of the TRB array */
    u32   size;          /* Total number of TRBs in the ring (including Link TRB) */
    u32   enqueue_idx;   /* Current index where software will write next TRB */
    u32   dequeue_idx;   /* Current index where software will read next TRB */
    u32   cycle_state;   /* Current Cycle Bit state (1 or 0) */
    bool       is_event_ring; /* Event rings do not use Link TRBs; they wrap naturally */
} xhci_ring_t;


/* ==============================================================================
 * Ring Management API
 * ============================================================================== */

/*
 * Allocates contiguous physical memory for a TRB ring and initializes the software tracker.
 * For Command/Transfer rings, this function automatically populates the terminal Link TRB.
 * * @param ring          Pointer to the software ring tracker to initialize.
 * @param num_trbs      Number of TRBs to allocate (typically XHCI_RING_TRBS_PER_PAGE).
 * @param is_event_ring True if this is an Event Ring (alters wrap behavior).
 * @return              XHCI_SUCCESS on success, XHCI_ERR_NO_MEMORY on allocation failure.
 */
xhci_status_t xhci_ring_alloc(xhci_ring_t *ring, u32 num_trbs, bool is_event_ring);

/*
 * Safely deallocates the physical memory associated with a ring and zeroes the tracker.
 * * @param ring          Pointer to the initialized software ring tracker.
 */
void xhci_ring_free(xhci_ring_t *ring);


/* ==============================================================================
 * Ring Operations (Producer)
 * ============================================================================== */

/*
 * Enqueues a single TRB onto a Command or Transfer ring.
 * This function handles writing the TRB parameters, applying a memory barrier, 
 * toggling the Cycle Bit properly, and advancing the enqueue index. 
 * If the Link TRB is reached, it traverses the link and updates the cycle state.
 * * @param ring      Pointer to the command or transfer ring.
 * @param param1    TRB Parameter 1 (Lower 32-bits).
 * @param param2    TRB Parameter 2 (Upper 32-bits).
 * @param status    TRB Status field.
 * @param control   TRB Control field (The Cycle bit will be forcefully overridden by this function).
 * @return          XHCI_SUCCESS on success, XHCI_ERR_RING_FULL if ring is full.
 */
xhci_status_t xhci_ring_enqueue(xhci_ring_t *ring, u32 param1, u32 param2, u32 status, u32 control);

/*
 * Prepare one TRB while retaining software ownership of it.  This is used for
 * the first TRB of a multi-stage operation: later TRBs may be populated, the
 * completion owner armed, and only then may the first TRB be published.
 */
xhci_status_t xhci_ring_enqueue_unpublished(
    xhci_ring_t *ring, u32 param1, u32 param2, u32 status, u32 control,
    uintptr_t *trb_phys_out, u32 *producer_cycle_out);

/* Publish a TRB previously prepared by xhci_ring_enqueue_unpublished(). */
xhci_status_t xhci_ring_publish_trb(xhci_ring_t *ring, uintptr_t trb_phys,
                                    u32 producer_cycle);

/* Test membership in a logical TRB range, including a range that wraps. */
bool xhci_ring_trb_in_range(const xhci_ring_t *ring, uintptr_t range_start,
                            uintptr_t range_end, uintptr_t candidate);

/* Reclaim a completed transfer TRB identified by its physical address. */
xhci_status_t xhci_ring_reclaim_transfer(xhci_ring_t *ring, uintptr_t trb_phys);

/*
 * Reclaim one complete transfer TD, from its first TRB through its terminal
 * TRB.  The first TRB must be the oldest outstanding TRB on the ring.  Link
 * TRBs are skipped while walking the producer ring.
 */
xhci_status_t xhci_ring_reclaim_td(xhci_ring_t *ring,
                                   uintptr_t td_start_phys,
                                   uintptr_t td_end_phys);


/* ==============================================================================
 * Ring Operations (Consumer / Event Ring)
 * ============================================================================== */

/*
 * Checks the Event Ring at the current dequeue index to see if the hardware
 * has written a new event. It does this by comparing the TRB's Cycle bit 
 * against the software's Consumer Cycle State (CCS).
 * * @param ring      Pointer to the event ring.
 * @return          Pointer to the current Event TRB if valid, NULL if no new events.
 */
xhci_trb_t* xhci_event_ring_get_next(xhci_ring_t *ring);

/*
 * Advances the software dequeue pointer for an Event Ring after an event has been processed.
 * Automatically wraps the pointer and toggles the Consumer Cycle State (CCS) when 
 * crossing the segment boundary.
 * * @param ring      Pointer to the event ring.
 */
void xhci_event_ring_advance(xhci_ring_t *ring);
