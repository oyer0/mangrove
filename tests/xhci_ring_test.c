#include <stdio.h>
#include <stdlib.h>
#include <xhci_ring.h>

void *xhci_dma_alloc(usize size, uintptr_t *phys) { void *p=calloc(1,(size_t)size); *phys=(uintptr_t)p; return p; }
void xhci_dma_free(void *p, usize size) { (void)size; free(p); }

int main(void) {
    xhci_ring_t r;
    if (xhci_ring_alloc(&r, 4, false) != XHCI_SUCCESS) return 1;
    for (u32 i=0; i<30; i++) {
        uintptr_t trb = r.phys_base + (uintptr_t)r.enqueue_idx * sizeof(xhci_trb_t);
        if (xhci_ring_enqueue(&r, 0, 0, 0, XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_NORMAL)) != XHCI_SUCCESS) return 2;
        if (xhci_ring_reclaim_transfer(&r, trb) != XHCI_SUCCESS) return 3;
    }
    {
        uintptr_t first = r.phys_base + (uintptr_t)r.enqueue_idx * sizeof(xhci_trb_t);
        uintptr_t second = r.phys_base + (uintptr_t)((r.enqueue_idx + 1) % 3) * sizeof(xhci_trb_t);
        uintptr_t third = 0;
        if (xhci_ring_enqueue(&r,0,0,0,0) != XHCI_SUCCESS ||
            xhci_ring_enqueue(&r,0,0,0,0) != XHCI_SUCCESS ||
            xhci_ring_reclaim_transfer(&r, first) != XHCI_SUCCESS ||
            xhci_ring_reclaim_transfer(&r, second) != XHCI_SUCCESS) return 5;
    }
    if (r.enqueue_idx != 2 || r.dequeue_idx != 2 || r.cycle_state != 1) return 4;
    xhci_ring_free(&r);
    puts("xHCI transfer-ring wrap/reclaim test passed");
    return 0;
}
