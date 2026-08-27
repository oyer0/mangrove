#pragma once

#include <types.h>
#include <xhci_trb.h>
#include <xhci_context.h>
#include <kprint.h>

#ifndef XHCI_DEBUG
#define XHCI_DEBUG 0
#endif

#if XHCI_DEBUG
#define XHCI_DEBUG_LOG(...) kprint_debug_screen(__VA_ARGS__)
#else
#define XHCI_DEBUG_LOG(...) kprint_debug(__VA_ARGS__)
#endif

/* Number of independently owned asynchronous transfer records/buffers per
   endpoint. Synchronous EP0 operations continue to use record zero. */
#define XHCI_TRANSFER_RECORD_SLOTS 1U

/* * Opaque handle to an xHCI Host Controller instance.
 * The internal structure (containing DCBAA, Rings, Scratchpads, etc.) 
 * is defined privately within the driver implementation to maintain encapsulation.
 */
typedef struct xhci_controller xhci_controller_t;

/*
 * Opaque handle to a connected USB device managed by the xHCI controller.
 */
typedef struct xhci_device xhci_device_t;

/*
 * Standard xHCI driver operation status codes.
 */
typedef enum {
    XHCI_SUCCESS             = 0,
    XHCI_ERR_NO_MEMORY       = 1,
    XHCI_ERR_TIMEOUT         = 2,
    XHCI_ERR_INVALID_PARAM   = 3,
    XHCI_ERR_NOT_SUPPORTED   = 4,
    XHCI_ERR_HW_HALTED       = 5,
    XHCI_ERR_CONTROLLER_BAD  = 6,
    XHCI_ERR_PORT_RESET_FAIL = 7,
    XHCI_ERR_DEVICE_TIMEOUT  = 8,
    XHCI_ERR_TRANSACTION     = 9
} xhci_status_t;

/* xHCI-owned lifecycle states, separate from the kernel init graph. */
typedef enum {
    XHCI_CONTROLLER_UNINITIALIZED = 0,
    XHCI_CONTROLLER_CLAIMING_FIRMWARE,
    XHCI_CONTROLLER_RESETTING,
    XHCI_CONTROLLER_RINGS_READY,
    XHCI_CONTROLLER_RUNNING,
    XHCI_CONTROLLER_SERVICE_READY,
    XHCI_CONTROLLER_BOOT_ENUMERATING,
    XHCI_CONTROLLER_BOOT_QUIESCENT,
    XHCI_CONTROLLER_FAILED
} xhci_controller_state_t;

typedef enum {
    XHCI_PORT_DISCONNECTED = 0,
    XHCI_PORT_CONNECTED_OBSERVED,
    XHCI_PORT_DEBOUNCING,
    XHCI_PORT_RESET_REQUESTED,
    XHCI_PORT_RESET_IN_PROGRESS,
    XHCI_PORT_RESET_COMPLETED,
    XHCI_PORT_ENABLED,
    XHCI_PORT_ENUMERATING,
    XHCI_PORT_READY,
    XHCI_PORT_FAILED
} xhci_port_state_t;

typedef enum {
    XHCI_DEVICE_NO_SLOT = 0,
    XHCI_DEVICE_SLOT_ENABLED,
    XHCI_DEVICE_ADDRESSING,
    XHCI_DEVICE_ADDRESSED,
    XHCI_DEVICE_EP0_READY,
    XHCI_DEVICE_DESCRIPTORS_READY,
    XHCI_DEVICE_ENDPOINTS_CONFIGURED,
    XHCI_DEVICE_USB_CONFIGURED,
    XHCI_DEVICE_CLASS_ATTACHING,
    XHCI_DEVICE_READY,
    XHCI_DEVICE_FAILED
} xhci_device_state_t;

/*
 * USB Device Speeds as defined by the xHCI specification.
 * Mapped from the PORTSC Port Speed (PS) field and used in Slot Contexts.
 */
typedef enum {
    XHCI_SPEED_UNKNOWN       = 0,
    XHCI_SPEED_FULL          = 1, /* 12 MB/s */
    XHCI_SPEED_LOW           = 2, /* 1.5 MB/s (Standard for standard HID Keyboards) */
    XHCI_SPEED_HIGH          = 3, /* 480 MB/s */
    XHCI_SPEED_SUPER         = 4  /* 5 GB/s */
} xhci_speed_t;

/*
 * Callback function signature for HID Keyboard reports.
 * * @param modifier_mask  Standard USB HID modifier byte (Bitmask of Ctrl, Shift, Alt, GUI).
 * @param key_codes      Array of active USB HID usage codes (scancodes).
 * @param count          Number of active keys in the key_codes array.
 */
typedef void (*xhci_hid_keyboard_callback_t)(u8 modifier_mask, const u8 *key_codes, u8 count);

typedef enum {
    XHCI_TRANSFER_EVENT_STALE = 0,
    XHCI_TRANSFER_EVENT_WAITING,
    XHCI_TRANSFER_EVENT_ASYNC,
    XHCI_TRANSFER_EVENT_PROGRESS
} xhci_transfer_event_route_t;


/* ==============================================================================
 * Core Controller API
 * ============================================================================== */

/* * Initializes a new xHCI controller instance.
 * Allocates primary memory structures (DCBAA, Command Ring, Event Ring, Scratchpads)
 * but leaves the controller in a Halted state.
 * * @param mmio_base     The 64-bit virtual memory address mapped to the controller's PCI BAR.
 * @param irq_number    The hardware IRQ (or GSI) assigned to this controller.
 * @return              Pointer to the initialized controller instance, or NULL on failure.
 */
xhci_controller_t* xhci_init(uintptr_t mmio_base, u8 irq_number);

/*
 * Transitions the controller into the Running state (USBCMD.RS = 1) 
 * and enables Interrupter 0.
 * * @param xhc           The controller instance.
 * @return              XHCI_SUCCESS if successfully started.
 */
xhci_status_t xhci_start(xhci_controller_t *xhc);
void xhci_acknowledge_boot_interrupts(xhci_controller_t *xhc);
void xhci_begin_boot_enumeration(xhci_controller_t *xhc);
void xhci_complete_boot_enumeration(xhci_controller_t *xhc);
bool xhci_start_deferred_worker(xhci_controller_t *xhc);
bool xhci_is_service_owner(xhci_controller_t *xhc);
bool xhci_setup_retry_allowed(xhci_controller_t *xhc);
xhci_controller_state_t xhci_get_controller_state(xhci_controller_t *xhc);
xhci_port_state_t xhci_get_port_state(xhci_controller_t *xhc, u8 port_id);
void xhci_set_port_state(xhci_controller_t *xhc, u8 port_id,
                         xhci_port_state_t state, u32 status,
                         const char *reason);

/*
 * Halts the controller (USBCMD.RS = 0) and waits for the HCH (HCHalted) status bit.
 * * @param xhc           The controller instance.
 * @return              XHCI_SUCCESS if safely halted.
 */
xhci_status_t xhci_stop(xhci_controller_t *xhc);

/*
 * Issues a Host Controller Reset (USBCMD.HCRST) and completely resets the driver state.
 * * @param xhc           The controller instance.
 * @return              XHCI_SUCCESS if reset succeeds and controller halts.
 */
xhci_status_t xhci_reset(xhci_controller_t *xhc);

/*
 * Safely deallocates all VMM/PMM memory associated with this controller 
 * and unregisters its interrupts.
 */
void xhci_shutdown(xhci_controller_t *xhc);


/* ==============================================================================
 * Subsystem Integration API
 * ============================================================================== */

/*
 * Main hardware interrupt entry point for the xHCI controller.
 * The Mangrove OS IRQ dispatcher must route the controller's mapped vector to this function.
 * This routine clears the EINT/IP flags and processes the Event Ring.
 */
void xhci_interrupt_handler(xhci_controller_t *xhc);

/*
 * Completion handoff between the xHCI service owner and synchronous driver
 * operations.  The service owner is the only code allowed to consume the
 * Event Ring; callers arm a mailbox before ringing a doorbell and wait for
 * the owner to publish the captured event.
 */
bool xhci_arm_command_wait(xhci_controller_t *xhc, u8 expected_cmd_type,
                           uintptr_t command_trb_phys, u8 expected_slot);
void xhci_cancel_command_wait(xhci_controller_t *xhc);
bool xhci_take_command_completion(xhci_controller_t *xhc,
                                  xhci_trb_t *out_event);
bool xhci_route_command_completion(xhci_controller_t *xhc,
                                   const xhci_trb_t *event);
bool xhci_arm_transfer_wait(xhci_controller_t *xhc, u8 slot_id, u8 dci,
                            uintptr_t td_start, uintptr_t td_end,
                            uintptr_t expected_completion_trb);
bool xhci_arm_async_transfer(xhci_controller_t *xhc, u8 slot_id, u8 dci,
                             uintptr_t td_start, uintptr_t td_end,
                             uintptr_t expected_completion_trb);
void xhci_cancel_transfer_operation(xhci_controller_t *xhc, u8 slot_id,
                                    u8 dci);
void xhci_cancel_transfer_wait(xhci_controller_t *xhc);
bool xhci_take_transfer_completion(xhci_controller_t *xhc,
                                   xhci_trb_t *out_event);
xhci_transfer_event_route_t xhci_route_transfer_event(
    xhci_controller_t *xhc, const xhci_trb_t *event);
bool xhci_complete_async_transfer(xhci_controller_t *xhc, u8 slot_id,
                                  u8 dci, uintptr_t completion_trb);

/*
 * Initiates the port enumeration process. Iterates over all Root Hub ports,
 * detects connections, issues port resets, assigns Slot IDs, and evaluates contexts.
 * * @param xhc           The controller instance.
 * @return              XHCI_SUCCESS if enumeration completes normally.
 */
xhci_status_t xhci_probe_ports(xhci_controller_t *xhc);

/*
 * Registers an OS-level callback for processing incoming USB HID Keyboard reports.
 * For this driver scope, this acts as the bridge between the asynchronous USB Interrupt IN 
 * endpoint and the Mangrove OS terminal/input subsystem.
 */
xhci_status_t xhci_register_keyboard_callback(xhci_controller_t *xhc, xhci_hid_keyboard_callback_t callback);
void xhci_resume_keyboard(xhci_controller_t *xhc);
void xhci_print_boot_summary(xhci_controller_t *xhc, bool mgfs_mounted);

void xhci_diag_set_context(u8 port, u8 slot, xhci_speed_t speed);
void xhci_diag_set_phase(const char *phase);
void xhci_diag_set_control_quiet(bool quiet);
void xhci_diag_timeline(const char *stage, u32 portsc);
void xhci_diag_timeline_at(const char *stage, uintptr_t portsc_addr, u32 portsc);
void xhci_diag_timeline_port(xhci_controller_t *xhc, const char *stage);
volatile u32 *xhci_get_portsc_ptr(xhci_controller_t *xhc, u8 port_idx);
void xhci_diag_address_dw1(xhci_controller_t *xhc, u8 slot, const char *stage);
void xhci_diag_address_context(xhci_controller_t *xhc, u8 slot,
                               uintptr_t input_ctx_phys,
                               const xhci_input_control_context_t *ctrl,
                               const xhci_slot_context_t *slot_ctx,
                               const xhci_ep_context_t *ep0_ctx);
void xhci_diag_failure(xhci_status_t result);
void xhci_diag_command_result(u8 trb_type, u8 slot, xhci_status_t result,
                              const xhci_trb_t *event);
void xhci_diag_control_result(xhci_controller_t *xhc, u8 slot,
                              u8 bm_request_type, u8 request, u16 value,
                              u16 index, u16 length, u32 setup_ctrl,
                              u32 status_ctrl, u32 ring_before_enq,
                              u32 ring_before_deq, u32 ring_after_enq,
                              u32 ring_after_deq, xhci_status_t result,
                              const xhci_trb_t *event);


/* ==============================================================================
 * State Queries
 * ============================================================================== */

/*
 * Queries the controller version (HCIVERSION from CAPLENGTH).
 * * @return              BCD encoded 16-bit value (e.g., 0x0100 for xHCI 1.0, 0x0110 for 1.1).
 */
u16 xhci_get_version(xhci_controller_t *xhc);

/*
 * Evaluates the USBSTS.HCH register bit.
 * * @return              True if the controller is actively executing (HCH == 0).
 */
bool xhci_is_running(xhci_controller_t *xhc);
