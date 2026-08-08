#include <xhci.h>
#include <xhci_regs.h>
#include <xhci_trb.h>
#include <types.h>
#include <stddef.h>

/* ==============================================================================
 * External Dependencies
 * ============================================================================== */

/* Timekeeping subsystem for required physical link delays (Stage 6) */
extern void timer_sleep(u64 ms);

/* Controller state accessors (implemented in xhci.c) */
extern xhci_op_regs_t* xhci_get_op_regs(xhci_controller_t *xhc);
extern u8              xhci_get_max_ports(xhci_controller_t *xhc);

/* Device setup pipeline hook (implemented in xhci.c) */
extern xhci_status_t   xhci_setup_device(xhci_controller_t *xhc, u8 port_id, xhci_speed_t speed);

/* Logging subsystem for debug output */
extern void kprint(const char *fmt, ...);

extern bool xhci_is_busy(xhci_controller_t *xhc);


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/*
 * Returns a volatile pointer to the PORTSC register for a specific port.
 * Ports are 1-indexed in the xHCI specification.
 * Operational Base + 0x400 + (0x10 * (Port Num - 1))
 */
volatile u32* xhci_get_portsc_ptr(xhci_controller_t *xhc, u8 port_idx) {
    xhci_op_regs_t *op_regs = xhci_get_op_regs(xhc);
    if (!op_regs || port_idx == 0 || port_idx > xhci_get_max_ports(xhc)) {
        return NULL;
    }

    /*
     * The port register set begins at offset 0x400 from the Operational Base.
     * We cast to uintptr_t to bypass the struct layout, then calculate the physical offset.
     */
    uintptr_t op_base = (uintptr_t)op_regs;
    uintptr_t port_base = op_base + 0x400 + (0x10 * (port_idx - 1));
    
    return (volatile u32*)port_base;
}

/*
 * Safely writes to the PORTSC register.
 * CRITICAL: PORTSC contains many RW1C (Read-Write 1 to Clear) bits.
 * If you perform a naive `*portsc |= NEW_BIT`, you will accidentally clear
 * pending interrupt flags (like Connect Status Change). 
 * This function enforces a strict mask to protect state.
 */
static void xhci_write_portsc(volatile u32 *portsc_ptr, u32 new_val) {
    u32 current = *portsc_ptr;
    /* Mask out RW1C bits to preserve them (preventing accidental clearing) */
    current &= ~XHCI_PORTSC_RW1C_MASK;
    /* Apply new values, ensuring we don't accidentally set a RW1C bit from the new payload */
    *portsc_ptr = current | (new_val & ~XHCI_PORTSC_RW1C_MASK);
}

/*
 * Clears a specific RW1C bit in the PORTSC register.
 */

static void xhci_clear_portsc_bit(volatile u32 *portsc_ptr, u32 bit_mask) {
    // For xHCI PORTSC, status change indicators are Write-1-to-Clear.
    // Writing 1 clears them; writing 0 has no effect. 
    // We also need to preserve the current values of writeable control bits 
    // so we don't accidentally toggle speed, power, or reset state.
    u32 val = *portsc_ptr;
    
    // Keep current control bits, but clear out the W1C change bits from the read value 
    // so we don't accidentally re-trigger them, then OR in the specific bit_mask we want to clear.
    // Standard xHCI trick: mask out the RW1C bits from the read value, then write back.
    // (Assuming XHCI_PORTSC_RW1C_MASK covers CSC, PEC, PRC, PLC, CEC, WRC)
    val &= ~XHCI_PORTSC_RW1C_MASK;
    val |= bit_mask;
    
    *portsc_ptr = val;
}

/* ==============================================================================
 * Core Port Enumeration API
 * ============================================================================== */

/*
 * Issues a hardware Port Reset to negotiate link speed and enable the port.
 * This is Phase 2 of the initialization plan.
 * * @param portsc_ptr Pointer to the specific PORTSC register.
 * @return           XHCI_SUCCESS if the port transitions to Enabled, XHCI_ERR_PORT_RESET_FAIL otherwise.
 */
static xhci_status_t xhci_reset_port(volatile u32 *portsc_ptr) {
    u32 status = *portsc_ptr;
    xhci_diag_timeline_at("reset-start", (uintptr_t)portsc_ptr, status);

    /* Read speed ID from PORTSC */
    u8 speed = XHCI_PORTSC_SPEED(status);

    if (speed == 4) {
        /* Issue Warm Port Reset for USB 3.0 (Port 5) */
        xhci_write_portsc(portsc_ptr, XHCI_PORTSC_WPR);
    } else {
        /* Standard Port Reset for USB 2.0 */
        xhci_write_portsc(portsc_ptr, XHCI_PORTSC_PR);
    }

    /* DO NOT USE timer_sleep() HERE! We might be inside an ISR.
       Use a simple busy-wait spin count instead. */
    volatile u32 spin = 2000000;
    bool reset_done = false;

    while (spin > 0) {
        u32 current = *portsc_ptr;
        if ((current & XHCI_PORTSC_PRC) || (current & XHCI_PORTSC_WRC)) {
            reset_done = true;
            xhci_diag_timeline_at("reset-complete", (uintptr_t)portsc_ptr, current);
            break;
        }
        spin--;
    }

    /* Clear ALL pending change flags to prevent infinite ISR loops */
    xhci_clear_portsc_bit(portsc_ptr, XHCI_PORTSC_PRC | XHCI_PORTSC_WRC | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC);
    xhci_diag_timeline_at("post-reset", (uintptr_t)portsc_ptr, *portsc_ptr);

    if (!reset_done) {
        return XHCI_ERR_PORT_RESET_FAIL;
    }

    /* Check if port is Enabled */
    if ((*portsc_ptr & XHCI_PORTSC_PED) == 0) {
        return XHCI_ERR_PORT_RESET_FAIL;
    }

    return XHCI_SUCCESS;
}

xhci_status_t xhci_probe_ports(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    u8 max_ports = xhci_get_max_ports(xhc);
    
    for (u8 port = 1; port <= max_ports; port++) {
        volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port);
        if (!portsc) continue;

        u32 status = *portsc;

        /* Check if a device is physically plugged in (Current Connect Status) */
        if (status & XHCI_PORTSC_CCS) {
            xhci_speed_t reset_speed = XHCI_PORTSC_SPEED(status);
            xhci_diag_set_context(port, 0, reset_speed);
            xhci_diag_set_phase("port-reset");
            xhci_status_t err = xhci_reset_port(portsc);
            if (err != XHCI_SUCCESS) {
                xhci_diag_failure(err);
#ifdef RHIZOME_DEBUG_BOOT_TESTS
                kprint("[xHCI] Port %d reset failed.\n", port);
#endif
                continue;
            }

            /* Read the negotiated link speed */
            status = *portsc;
            u8 speed = XHCI_PORTSC_SPEED(status);
            xhci_diag_timeline_at("post-reset-read", (uintptr_t)portsc, status);

            /* === THE MISSING LINK === */
            /* Push the newly discovered device through the setup pipeline */
            err = xhci_setup_device(xhc, port, speed);
            
            if (err != XHCI_SUCCESS) {
                kprint("[xHCI Error] Failed to setup device on port %d (Code: %d)\n", port, err);
            }
        }
    }

    return XHCI_SUCCESS;
}


/* ==============================================================================
 * Asynchronous Event Handlers
 * Called by xhci_event.c when a Port Status Change TRB arrives on the Event Ring.
 * ============================================================================== */

/*
 * Processes asynchronous hotplug (attach/detach) events.
 * * @param xhc   The controller instance.
 * @param event The parsed Port Status Change Event TRB.
 */
void xhci_handle_port_status_change(xhci_controller_t *xhc, xhci_trb_t *event) {
    if (!xhc || !event) return;

    /* If we are currently in the middle of a device setup sequence, 
     * ignore background hotplug status changes to prevent race conditions. */
    if (xhci_is_busy(xhc)) {
        return;
    }

    /* The target port ID is in param1 [31:24] for Port Status Change Events */
    u8 port_id = (u8)((event->param1 >> 24) & 0xFF);
    
    volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port_id);
    if (!portsc) return;

    u32 status = *portsc;

    /* Handle Connection Change (Hotplug/Unplug) */
    if (status & XHCI_PORTSC_CSC) {
        xhci_clear_portsc_bit(portsc, XHCI_PORTSC_CSC);
        
        if (status & XHCI_PORTSC_CCS) {
            /* Triggers reset and enumeration pipeline */
            if (xhci_reset_port(portsc) == XHCI_SUCCESS) {
                u32 new_status = *portsc;
                u8 speed = XHCI_PORTSC_SPEED(new_status);
                
                xhci_status_t err = xhci_setup_device(xhc, port_id, speed);
                if (err != XHCI_SUCCESS) {
                    kprint("[xHCI Error] Failed to setup device on port %d (Code: %d)\n", port_id, err);
                }
            } else {
                kprint("[xHCI Error] Port %d hotplug reset failed.\n", port_id);
            }
        }
    }
    
    /* Clear other minor status change flags to prevent infinite interrupt loops */
    if (status & XHCI_PORTSC_PEC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_PEC);
    if (status & XHCI_PORTSC_PRC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_PRC);
    if (status & XHCI_PORTSC_PLC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_PLC);
    if (status & XHCI_PORTSC_CEC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_CEC);
    if (status & XHCI_PORTSC_WRC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_WRC); /* Ensure WRC is cleared too */
}
