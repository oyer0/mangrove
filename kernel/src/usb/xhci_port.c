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
extern u64  timer_uptime_ms(void);

/* Controller state accessors (implemented in xhci.c) */
extern xhci_op_regs_t* xhci_get_op_regs(xhci_controller_t *xhc);
extern u8              xhci_get_max_ports(xhci_controller_t *xhc);

/* Device setup pipeline hook (implemented in xhci.c) */
extern xhci_status_t   xhci_setup_device(xhci_controller_t *xhc, u8 port_id, xhci_speed_t speed);

/* Logging subsystem for debug output */
extern void kprint(const char *fmt, ...);

extern bool xhci_is_busy(xhci_controller_t *xhc);
extern void xhci_queue_port_change(xhci_controller_t *xhc, u8 port_id,
                                   const char *source);


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
    /* Never write back RW1CS state, including PED (which requests port disable). */
    current &= ~XHCI_PORTSC_RW1CS_MASK;
    *portsc_ptr = current | (new_val & ~XHCI_PORTSC_RW1CS_MASK);
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
    val &= ~XHCI_PORTSC_RW1CS_MASK;
    val |= bit_mask;
    
    *portsc_ptr = val;
}

#define XHCI_PORT_DEBOUNCE_MS       100U
#define XHCI_PORT_RESET_TIMEOUT_MS  500U
#define XHCI_PORT_POLL_MS           1U
#define XHCI_BOOT_PORT_TIMEOUT_MS   10000U

static const char *xhci_port_reset_state_name(xhci_port_state_t state)
{
    switch (state) {
        case XHCI_PORT_CONNECTED_OBSERVED: return "CONNECTED_OBSERVED";
        case XHCI_PORT_DEBOUNCING: return "DEBOUNCING";
        case XHCI_PORT_RESET_REQUESTED: return "RESET_REQUESTED";
        case XHCI_PORT_RESET_IN_PROGRESS: return "RESET_IN_PROGRESS";
        case XHCI_PORT_RESET_COMPLETED: return "RESET_COMPLETED";
        case XHCI_PORT_ENABLED: return "ENABLED";
        default: return "UNKNOWN";
    }
}

static void xhci_port_reset_log(u8 port_id,
                                xhci_port_state_t state,
                                u32 status,
                                const char *reason)
{
    XHCI_DEBUG_LOG(
        "[xHCI-PORT] p%u reset state=%s st=%08x ccs=%u ped=%u pr=%u "
        "wpr=%u prc=%u wrc=%u pls=%u sp=%u%s%s\n",
        port_id, xhci_port_reset_state_name(state), status,
        (status & XHCI_PORTSC_CCS) != 0,
        (status & XHCI_PORTSC_PED) != 0,
        (status & XHCI_PORTSC_PR) != 0,
        (status & XHCI_PORTSC_WPR) != 0,
        (status & XHCI_PORTSC_PRC) != 0,
        (status & XHCI_PORTSC_WRC) != 0,
        (status & XHCI_PORTSC_PLS_MASK) >> 5,
        XHCI_PORTSC_SPEED(status),
        reason ? " reason=" : "", reason ? reason : "");
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
static xhci_status_t xhci_reset_port(xhci_controller_t *xhc, u8 port_id,
                                     volatile u32 *portsc_ptr) {
    xhci_port_state_t state = XHCI_PORT_CONNECTED_OBSERVED;
    u32 status = *portsc_ptr;
    u32 stale_changes = status & XHCI_PORTSC_RW1C_MASK;
    bool superspeed = XHCI_PORTSC_SPEED(status) == XHCI_SPEED_SUPER;
    u32 reset_bit = superspeed ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR;
    u32 completion_bit = superspeed ? XHCI_PORTSC_WRC : XHCI_PORTSC_PRC;
    u64 stable_since;
    u64 debounce_deadline;
    u64 reset_deadline;
    bool completion_observed = false;
    bool reset_asserted_logged = false;
    const char *failure_reason = "reset-timeout";

    xhci_diag_timeline_at("reset-start", (uintptr_t)portsc_ptr, status);
    xhci_port_reset_log(port_id, state, status, NULL);
    xhci_set_port_state(xhc, port_id, state, status, "reset-start");

    if (!(status & XHCI_PORTSC_CCS)) {
        failure_reason = "not-connected-at-reset-start";
        goto failed;
    }

    /* Establish a clean W1C baseline before debounce and reset. */
    if (stale_changes)
        xhci_clear_portsc_bit(portsc_ptr, stale_changes);

    state = XHCI_PORT_DEBOUNCING;
    stable_since = timer_uptime_ms();
    debounce_deadline = stable_since + XHCI_PORT_RESET_TIMEOUT_MS;
    xhci_port_reset_log(port_id, state, status, "connection-stability");
    xhci_set_port_state(xhc, port_id, state, status,
                        "connection-stability");
    while (timer_uptime_ms() - stable_since < XHCI_PORT_DEBOUNCE_MS) {
        if (timer_uptime_ms() >= debounce_deadline) {
            failure_reason = "connection-not-stable-before-reset";
            goto failed;
        }
        timer_sleep(XHCI_PORT_POLL_MS);
        status = *portsc_ptr;
        if (!(status & XHCI_PORTSC_CCS)) {
            if (status & XHCI_PORTSC_CSC)
                xhci_clear_portsc_bit(portsc_ptr, XHCI_PORTSC_CSC);
            stable_since = timer_uptime_ms();
            continue;
        }
        if (status & XHCI_PORTSC_CSC) {
            xhci_clear_portsc_bit(portsc_ptr, XHCI_PORTSC_CSC);
            stable_since = timer_uptime_ms();
        }
    }

    state = XHCI_PORT_RESET_REQUESTED;
    xhci_port_reset_log(port_id, state, status,
                        superspeed ? "assert-warm-reset" : "assert-reset");
    xhci_set_port_state(xhc, port_id, state, status,
                        superspeed ? "assert-warm-reset" : "assert-reset");
    if (superspeed)
        xhci_write_portsc(portsc_ptr, XHCI_PORTSC_WPR);
    else
        xhci_write_portsc(portsc_ptr, XHCI_PORTSC_PR);

    state = XHCI_PORT_RESET_IN_PROGRESS;
    xhci_port_reset_log(port_id, state, *portsc_ptr, NULL);
    xhci_set_port_state(xhc, port_id, state, *portsc_ptr,
                        "reset-asserted");
    reset_deadline = timer_uptime_ms() + XHCI_PORT_RESET_TIMEOUT_MS;
    while (timer_uptime_ms() < reset_deadline) {
        status = *portsc_ptr;
        if (!(status & XHCI_PORTSC_CCS)) {
            failure_reason = "connection-lost-during-reset";
            goto failed;
        }
        if ((status & reset_bit) && !reset_asserted_logged) {
            reset_asserted_logged = true;
            xhci_port_reset_log(port_id, state, status, "reset-asserted");
        }
        if (status & completion_bit) {
            if (!completion_observed) {
                completion_observed = true;
                state = XHCI_PORT_RESET_COMPLETED;
                xhci_port_reset_log(port_id, state, status,
                                    superspeed ? "WRC" : "PRC");
                xhci_set_port_state(xhc, port_id, state, status,
                                    superspeed ? "WRC" : "PRC");
            }
        }
        if (completion_observed && !(status & reset_bit)) {
            if (!(status & XHCI_PORTSC_PED)) {
                failure_reason = "reset-completed-port-disabled";
            } else if ((status & XHCI_PORTSC_PLS_MASK) != 0) {
                failure_reason = "reset-completed-link-not-u0";
            } else if (XHCI_PORTSC_SPEED(status) == XHCI_SPEED_UNKNOWN) {
                failure_reason = "reset-completed-speed-unknown";
            } else {
                state = XHCI_PORT_ENABLED;
                xhci_port_reset_log(port_id, state, status, NULL);
                xhci_set_port_state(xhc, port_id, state, status,
                                    "ready-for-address");
                xhci_diag_timeline_at("reset-complete",
                                      (uintptr_t)portsc_ptr, status);
                xhci_clear_portsc_bit(portsc_ptr,
                                      status & XHCI_PORTSC_RW1C_MASK);
                xhci_diag_timeline_at("post-reset", (uintptr_t)portsc_ptr,
                                      *portsc_ptr);
                return XHCI_SUCCESS;
            }
        }
        timer_sleep(XHCI_PORT_POLL_MS);
    }

    if (!completion_observed)
        failure_reason = "reset-completion-change-missing";
    else if (status & reset_bit)
        failure_reason = superspeed ? "warm-reset-bit-still-set" :
                                      "reset-bit-still-set";
    else if (!(status & XHCI_PORTSC_PED))
        failure_reason = "reset-completed-port-disabled";
    else if ((status & XHCI_PORTSC_PLS_MASK) != 0)
        failure_reason = "reset-completed-link-not-u0";

failed:
    status = *portsc_ptr;
    xhci_port_reset_log(port_id, state, status, failure_reason);
    xhci_set_port_state(xhc, port_id, XHCI_PORT_FAILED, status,
                        failure_reason);
    xhci_diag_timeline_at("post-reset", (uintptr_t)portsc_ptr, status);
    xhci_clear_portsc_bit(portsc_ptr, status & XHCI_PORTSC_RW1C_MASK);
    return XHCI_ERR_PORT_RESET_FAIL;
}

xhci_status_t xhci_probe_ports(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    xhci_begin_boot_enumeration(xhc);

    u8 max_ports = xhci_get_max_ports(xhc);
    
    for (u8 port = 1; port <= max_ports; port++) {
        volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port);
        if (!portsc) continue;

        u32 status = *portsc;

        /* Check if a device is physically plugged in (Current Connect Status) */
        if (status & XHCI_PORTSC_CCS) {
            xhci_set_port_state(xhc, port, XHCI_PORT_CONNECTED_OBSERVED,
                                status, "boot-probe");
            xhci_queue_port_change(xhc, port, "boot-probe");

            u64 deadline = timer_uptime_ms() + XHCI_BOOT_PORT_TIMEOUT_MS;
            while (timer_uptime_ms() < deadline) {
                xhci_port_state_t state = xhci_get_port_state(xhc, port);
                if (state == XHCI_PORT_READY ||
                    state == XHCI_PORT_FAILED ||
                    state == XHCI_PORT_DISCONNECTED)
                    break;
                (void)xhci_start_deferred_worker(xhc);
                timer_sleep(XHCI_PORT_POLL_MS);
            }

            xhci_port_state_t final_state = xhci_get_port_state(xhc, port);
            if (final_state != XHCI_PORT_READY &&
                final_state != XHCI_PORT_DISCONNECTED) {
                xhci_set_port_state(xhc, port, XHCI_PORT_FAILED, *portsc,
                                    "boot-owner-timeout");
                kprint("[xHCI Error] Port %d owner enumeration timeout\n",
                       port);
            }
        } else {
            xhci_set_port_state(xhc, port, XHCI_PORT_DISCONNECTED,
                                status, "boot-probe-disconnected");
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
    u8 port_id;
    volatile u32 *portsc;
    u32 status;
    if (!xhc || !event) return;

    /* The Port Status Change Event identifies the port; PORTSC is the source
       of truth for the connection state. */
    port_id = (u8)((event->param1 >> 24) & 0xFF);
    portsc = xhci_get_portsc_ptr(xhc, port_id);
    if (!portsc)
        return;
    status = *portsc;
    if (status & XHCI_PORTSC_CCS)
        xhci_set_port_state(xhc, port_id, XHCI_PORT_CONNECTED_OBSERVED,
                            status, "port-status-change-event");
    else
        xhci_set_port_state(xhc, port_id, XHCI_PORT_DISCONNECTED,
                            status, "port-status-change-disconnected");

    XHCI_DEBUG_LOG(
        "[xHCI-QUEUE] t=%llu source=port-status-change-event event=%p "
        "port=%u param1=%08x control=%08x st=%08x\n",
        (unsigned long long)timer_uptime_ms(), (void *)event, port_id,
        event->param1, event->control, status);

    /* Handle Connection Change (Hotplug/Unplug) */
    if (status & XHCI_PORTSC_CSC) {
        xhci_clear_portsc_bit(portsc, XHCI_PORTSC_CSC);
        
        if (status & XHCI_PORTSC_CCS)
            xhci_queue_port_change(xhc, port_id,
                                    xhci_is_busy(xhc) ?
                                        "port-status-change-busy" :
                                        "port-status-change-event");
    }
    
    /* Clear other minor status change flags to prevent infinite interrupt loops */
    if (status & XHCI_PORTSC_PEC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_PEC);
    if (status & XHCI_PORTSC_PRC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_PRC);
    if (status & XHCI_PORTSC_PLC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_PLC);
    if (status & XHCI_PORTSC_CEC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_CEC);
    if (status & XHCI_PORTSC_WRC) xhci_clear_portsc_bit(portsc, XHCI_PORTSC_WRC); /* Ensure WRC is cleared too */
}

/* Runs in the xHCI driver's normal kernel context, never in the IRQ path. */
void xhci_process_deferred_port_change(xhci_controller_t *xhc, u8 port_id)
{
    volatile u32 *portsc;
    u32 status;

    if (!xhc)
        return;
    portsc = xhci_get_portsc_ptr(xhc, port_id);
    if (!portsc)
        return;

    status = *portsc;
    if (!(status & XHCI_PORTSC_CCS)) {
        xhci_set_port_state(xhc, port_id, XHCI_PORT_DISCONNECTED,
                            status, "deferred-port-change-disconnected");
        return;
    }

    xhci_set_port_state(xhc, port_id, XHCI_PORT_CONNECTED_OBSERVED,
                        status, "deferred-port-change");
    if (xhci_reset_port(xhc, port_id, portsc) == XHCI_SUCCESS) {
        u32 new_status = *portsc;
        u8 speed = XHCI_PORTSC_SPEED(new_status);
        xhci_set_port_state(xhc, port_id, XHCI_PORT_ENUMERATING,
                            new_status, "runtime-device-setup");
        xhci_status_t err = xhci_setup_device(xhc, port_id, speed);
        if (err != XHCI_SUCCESS && xhci_setup_retry_allowed(xhc)) {
            status = *portsc;
            if ((status & XHCI_PORTSC_CCS) &&
                !(status & (XHCI_PORTSC_PR | XHCI_PORTSC_WPR))) {
                XHCI_DEBUG_LOG("[xHCI-RETRY] p%u attempt=2 previous=%u "
                               "st=%08x\n", port_id, err, status);
                xhci_set_port_state(xhc, port_id,
                                    XHCI_PORT_CONNECTED_OBSERVED, status,
                                    "setup-retry-after-cleanup");
                err = xhci_reset_port(xhc, port_id, portsc) == XHCI_SUCCESS ?
                    xhci_setup_device(xhc, port_id,
                                      XHCI_PORTSC_SPEED(*portsc)) :
                    XHCI_ERR_PORT_RESET_FAIL;
            }
        }
        if (err != XHCI_SUCCESS) {
            xhci_set_port_state(xhc, port_id, XHCI_PORT_FAILED,
                                *portsc, "runtime-device-setup-failed");
            kprint("[xHCI Error] Failed to setup device on port %d (Code: %d)\n",
                   port_id, err);
        } else {
            xhci_set_port_state(xhc, port_id, XHCI_PORT_READY,
                                *portsc, "runtime-device-ready");
        }
    } else {
        kprint("[xHCI Error] Port %d hotplug reset failed.\n", port_id);
    }
}
