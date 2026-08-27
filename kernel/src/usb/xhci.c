#include <xhci.h>
#include <xhci_regs.h>
#include <xhci_trb.h>
#include <xhci_context.h>
#include <xhci_ring.h>
#include <xhci_storage.h>
#include <xhci_hub.h>
#include <scheduler.h>
#include <address_space.h>
#include <timer.h>
#include <stddef.h>

/* ==============================================================================
 * External Subsystem Dependencies
 * These are implemented across the other xHCI driver modules and Mangrove OS.
 * ============================================================================== */

// xhci_mem.c
extern void* xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void xhci_dma_free(void* virt, usize size);
extern u64* xhci_alloc_dcbaa(u32 max_slots, uintptr_t *phys_out);
extern u64* xhci_alloc_scratchpads(u32 max_scratchpads, u32 page_size, uintptr_t *phys_out);
extern xhci_erst_entry_t* xhci_alloc_erst(u32 num_segments, uintptr_t *phys_out);
extern void* xhci_alloc_device_context(u32 csz, uintptr_t *phys_out);
extern void* xhci_alloc_input_context(u32 csz, uintptr_t *phys_out);

// xhci_event.c
extern void xhci_process_events(xhci_controller_t *xhc);

// xhci_cmd.c
extern xhci_status_t xhci_cmd_enable_slot(xhci_controller_t *xhc, u8 *out_slot_id);
extern xhci_status_t xhci_cmd_disable_slot(xhci_controller_t *xhc, u8 slot_id);
extern xhci_status_t xhci_cmd_address_device(xhci_controller_t *xhc, u8 slot_id, uintptr_t input_ctx_phys, bool block_set_address);
extern xhci_status_t xhci_cmd_evaluate_context(xhci_controller_t *xhc, u8 slot_id, uintptr_t input_ctx_phys);
extern xhci_status_t xhci_cmd_configure_endpoint(xhci_controller_t *xhc, u8 slot_id, uintptr_t input_ctx_phys);

// xhci_descriptor.c
extern xhci_status_t xhci_read_ep0_max_packet_size(xhci_controller_t *xhc, u8 slot_id, u8 *out_max_packet);
extern xhci_status_t xhci_get_keyboard_endpoint_info(xhci_controller_t *xhc, u8 slot_id, u8 *out_ep_addr, u16 *out_max_pkt, u8 *out_interval, u8 *out_config_val, u8 *out_interface_num);
extern xhci_status_t xhci_get_mass_storage_endpoint_info(xhci_controller_t *xhc, u8 slot_id, u8 *out_bulk_in, u16 *out_bulk_in_pkt, u8 *out_bulk_out, u16 *out_bulk_out_pkt, u8 *out_config_val, bool dump_on_miss);

// xhci_control.c
extern xhci_status_t xhci_control_set_configuration(xhci_controller_t *xhc, u8 slot_id, u8 config_value);
extern xhci_status_t xhci_control_set_protocol(xhci_controller_t *xhc, u8 slot_id, u8 interface_index, u8 protocol);

// xhci_hid.c
extern bool xhci_hid_queue_read(xhci_controller_t *xhc, u8 slot_id, u8 dci);

// Mangrove OS Handlers
extern volatile u32 *xhci_get_portsc_ptr(xhci_controller_t *xhc, u8 port_idx);
extern void kprint(const char *fmt, ...);
extern void xhci_process_deferred_port_change(xhci_controller_t *xhc,
                                              u8 port_id);

#define XHCI_DEVICE_CLASS_HID       (1U << 0)
#define XHCI_DEVICE_CLASS_STORAGE   (1U << 1)
#define XHCI_DEVICE_CLASS_HUB       (1U << 2)
#define USB2_RESET_RECOVERY_MS      10U
#define USB_SET_ADDRESS_RECOVERY_MS 2U
#define XHCI_CONTROLLER_WAIT_TIMEOUT_US 1000000ULL

/* ==============================================================================
 * Internal State Structures
 * ============================================================================== */

typedef struct {
    bool pending;
    bool synchronous;
    u64 generation;
    u8 slot_id;
    u8 dci;
    uintptr_t td_start;
    uintptr_t td_end;
    uintptr_t expected_completion_trb;
    u32 completion_code;
} xhci_transfer_record_t;

typedef struct {
    xhci_port_state_t state;
    u32 last_status;
    u32 generation;
    u64 reset_completed_ms;
} xhci_port_record_t;

struct xhci_device {
    u8 slot_id;
    u8 port_id;
    xhci_speed_t speed;
    u32 route_string;
    u8 topology_depth;
    u8 parent_hub_slot;
    u8 parent_port;
    xhci_speed_t parent_speed;
    
    void *out_ctx_virt;
    uintptr_t out_ctx_phys;
    
    void *in_ctx_virt;
    uintptr_t in_ctx_phys;
    
    xhci_ring_t ep_rings[32]; // Rings for Endpoints (DCI 1 to 31)
    
    u8 *ep_buffers_virt[32][XHCI_TRANSFER_RECORD_SLOTS];
    uintptr_t ep_buffers_phys[32][XHCI_TRANSFER_RECORD_SLOTS];

    u8 class_flags;
    u8 hid_dci;
    bool hid_armed;
    bool class_ready;
    xhci_transfer_record_t transfer_records[32][XHCI_TRANSFER_RECORD_SLOTS];
    xhci_device_state_t state;
    bool setup_finished;
    xhci_status_t setup_result;
    xhci_storage_probe_result_t storage_probe;
};

struct xhci_controller {
    uintptr_t mmio_base;
    u8 irq_number;
    bool is_running;
    xhci_controller_state_t state;
    xhci_hid_keyboard_callback_t keyboard_callback;

    xhci_cap_regs_t *cap_regs;
    xhci_op_regs_t *op_regs;
    xhci_run_regs_t *run_regs;
    volatile u32 *db_array;

    u32 max_slots;
    u32 max_ports;
    u32 max_intrs;
    u32 csz;
    u32 page_size;

    u64 *dcbaa;
    uintptr_t dcbaa_phys;
    
    u64 *scratchpads;
    
    xhci_ring_t cmd_ring;
    xhci_ring_t event_ring;
    xhci_erst_entry_t *erst;

    xhci_device_t devices[256];
    xhci_port_record_t ports[256];

    bool in_critical_section;
    volatile u32 pending_port_changes[8];
    volatile u32 port_change_generation;
    volatile u32 pending_port_generation[256];
    volatile bool boot_enumeration_active;
    volatile bool deferred_worker_stop;
    bool last_setup_retry_safe;
    volatile bool event_work_pending;
    volatile bool command_waiting;
    volatile bool command_completion_ready;
    u64 operation_generation;
    u64 command_generation;
    u8 command_expected_type;
    u8 command_expected_slot;
    uintptr_t command_trb_phys;
    u32 command_completion_code;
    xhci_trb_t command_completion;
    volatile bool transfer_waiting;
    volatile bool transfer_completion_ready;
    volatile u8 transfer_wait_slot;
    volatile u8 transfer_wait_dci;
    u64 transfer_wait_generation;
    xhci_trb_t transfer_completion;
    kernel_thread_t *deferred_worker;
};

static xhci_controller_t g_xhc_instance;
static bool g_xhc_init_started;

/* Accessors are implemented below, after the controller-private helpers. */
xhci_ring_t *xhci_get_ep_ring(xhci_controller_t *xhc, u8 slot_id, u8 dci);
bool xhci_is_hid_endpoint(xhci_controller_t *xhc, u8 slot_id, u8 dci);

static const char *xhci_controller_state_name(xhci_controller_state_t state)
{
    switch (state) {
        case XHCI_CONTROLLER_UNINITIALIZED: return "UNINITIALIZED";
        case XHCI_CONTROLLER_CLAIMING_FIRMWARE: return "CLAIMING_FIRMWARE";
        case XHCI_CONTROLLER_RESETTING: return "RESETTING";
        case XHCI_CONTROLLER_RINGS_READY: return "RINGS_READY";
        case XHCI_CONTROLLER_RUNNING: return "RUNNING";
        case XHCI_CONTROLLER_SERVICE_READY: return "SERVICE_READY";
        case XHCI_CONTROLLER_BOOT_ENUMERATING: return "BOOT_ENUMERATING";
        case XHCI_CONTROLLER_BOOT_QUIESCENT: return "BOOT_QUIESCENT";
        case XHCI_CONTROLLER_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

static const char *xhci_device_state_name(xhci_device_state_t state)
{
    switch (state) {
        case XHCI_DEVICE_NO_SLOT: return "NO_SLOT";
        case XHCI_DEVICE_SLOT_ENABLED: return "SLOT_ENABLED";
        case XHCI_DEVICE_ADDRESSING: return "ADDRESSING";
        case XHCI_DEVICE_ADDRESSED: return "ADDRESSED";
        case XHCI_DEVICE_EP0_READY: return "EP0_READY";
        case XHCI_DEVICE_DESCRIPTORS_READY: return "DESCRIPTORS_READY";
        case XHCI_DEVICE_ENDPOINTS_CONFIGURED:
            return "ENDPOINTS_CONFIGURED";
        case XHCI_DEVICE_USB_CONFIGURED: return "USB_CONFIGURED";
        case XHCI_DEVICE_CLASS_ATTACHING: return "CLASS_ATTACHING";
        case XHCI_DEVICE_READY: return "READY";
        case XHCI_DEVICE_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

static const char *xhci_port_state_name(xhci_port_state_t state)
{
    switch (state) {
        case XHCI_PORT_DISCONNECTED: return "DISCONNECTED";
        case XHCI_PORT_CONNECTED_OBSERVED: return "CONNECTED_OBSERVED";
        case XHCI_PORT_DEBOUNCING: return "DEBOUNCING";
        case XHCI_PORT_RESET_REQUESTED: return "RESET_REQUESTED";
        case XHCI_PORT_RESET_IN_PROGRESS: return "RESET_IN_PROGRESS";
        case XHCI_PORT_RESET_COMPLETED: return "RESET_COMPLETED";
        case XHCI_PORT_ENABLED: return "ENABLED";
        case XHCI_PORT_ENUMERATING: return "ENUMERATING";
        case XHCI_PORT_READY: return "READY";
        case XHCI_PORT_FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

static void xhci_set_controller_state(xhci_controller_t *xhc,
                                      xhci_controller_state_t state,
                                      const char *reason)
{
    if (!xhc || xhc->state == state)
        return;
    XHCI_DEBUG_LOG("[xHCI-STATE] controller %s -> %s%s%s\n",
                   xhci_controller_state_name(xhc->state),
                   xhci_controller_state_name(state),
                   reason ? " reason=" : "", reason ? reason : "");
    xhc->state = state;
}

static void xhci_set_device_state(xhci_controller_t *xhc, u8 slot_id,
                                  xhci_device_state_t state,
                                  const char *reason)
{
    xhci_device_t *dev;
    if (!xhc || slot_id == 0)
        return;
    dev = &xhc->devices[slot_id];
    if (dev->state == state)
        return;
    XHCI_DEBUG_LOG("[xHCI-STATE] device s%u %s -> %s%s%s\n", slot_id,
                   xhci_device_state_name(dev->state),
                   xhci_device_state_name(state),
                   reason ? " reason=" : "", reason ? reason : "");
    dev->state = state;
}

static void xhci_wait_until_ms(u64 deadline)
{
    while (timer_uptime_ms() < deadline)
        timer_sleep(1);
}

static bool xhci_wait_register_state(volatile u32 *register_address,
                                     u32 mask, bool set)
{
    timer_monotonic_deadline_t deadline;

    if (!register_address ||
        !timer_monotonic_deadline_start(&deadline,
                                        XHCI_CONTROLLER_WAIT_TIMEOUT_US)) {
        return false;
    }
    for (;;) {
        bool observed = (*register_address & mask) != 0;
        if (observed == set)
            return true;
        if (timer_monotonic_deadline_expired(&deadline))
            return false;
        __asm__ volatile("pause");
    }
}

xhci_controller_state_t xhci_get_controller_state(xhci_controller_t *xhc)
{
    return xhc ? xhc->state : XHCI_CONTROLLER_FAILED;
}

xhci_port_state_t xhci_get_port_state(xhci_controller_t *xhc, u8 port_id)
{
    if (!xhc || port_id == 0 || port_id > xhc->max_ports)
        return XHCI_PORT_FAILED;
    return xhc->ports[port_id].state;
}

void xhci_set_port_state(xhci_controller_t *xhc, u8 port_id,
                         xhci_port_state_t state, u32 status,
                         const char *reason)
{
    xhci_port_record_t *port;
    if (!xhc || port_id == 0 || port_id > xhc->max_ports)
        return;
    port = &xhc->ports[port_id];
    if (port->state != state) {
        XHCI_DEBUG_LOG("[xHCI-STATE] port p%u %s -> %s st=%08x%s%s\n",
                       port_id, xhci_port_state_name(port->state),
                       xhci_port_state_name(state), status,
                       reason ? " reason=" : "", reason ? reason : "");
    }
    port->state = state;
    port->last_status = status;
    if (state == XHCI_PORT_DISCONNECTED ||
        state == XHCI_PORT_RESET_REQUESTED)
        port->reset_completed_ms = 0;
    else if (state == XHCI_PORT_ENABLED)
        port->reset_completed_ms = timer_uptime_ms();
    port->generation = __atomic_load_n(&xhc->port_change_generation,
                                       __ATOMIC_ACQUIRE);
}

#define XHCI_EXT_CAP_ID_LEGACY       1U
#define XHCI_LEGACY_BIOS_OWNED       (1U << 16)
#define XHCI_LEGACY_OS_OWNED         (1U << 24)
#define XHCI_LEGACY_PRESERVE_BITS    ((0x7U << 1) | (0xFFU << 5) | (0x7U << 17))
#define XHCI_LEGACY_SMI_EVENTS       (0x7U << 29)
#define XHCI_MAX_EXT_CAPS            50U

/* Claim the xHC from pre-OS firmware before touching operational registers. */
static void xhci_legacy_handoff(xhci_controller_t *xhc)
{
    u32 offset = XHCI_HCCPARAMS1_XECP(xhc->cap_regs->hccparams1) << 2;

    for (u32 count = 0; offset && count < XHCI_MAX_EXT_CAPS; count++) {
        volatile u32 *cap = (volatile u32 *)(xhc->mmio_base + offset);
        u32 value = *cap;
        u32 next = (value >> 8) & 0xFFU;

        if ((value & 0xFFU) == XHCI_EXT_CAP_ID_LEGACY) {
            bool forced = false;
            u32 initial = value;

            if (value & XHCI_LEGACY_BIOS_OWNED) {
                *cap = value | XHCI_LEGACY_OS_OWNED;
                value = *cap;

                for (u32 waited = 0;
                     (value & XHCI_LEGACY_BIOS_OWNED) && waited < 1000;
                     waited++) {
                    timer_sleep(1);
                    value = *cap;
                }

                if (value & XHCI_LEGACY_BIOS_OWNED) {
                    forced = true;
                    *cap = (value | XHCI_LEGACY_OS_OWNED) &
                           ~XHCI_LEGACY_BIOS_OWNED;
                    value = *cap;
                }
            }

            volatile u32 *control = cap + 1;
            u32 control_value = *control;
            control_value &= XHCI_LEGACY_PRESERVE_BITS;
            control_value |= XHCI_LEGACY_SMI_EVENTS;
            *control = control_value;
            (void)*control;

            if (forced)
                kprint("[xHCI] Warning: forced legacy ownership release\n");
            XHCI_DEBUG_LOG("[xHCI] legacy bios=%u>%u os=%u force=%u\n",
                           (initial & XHCI_LEGACY_BIOS_OWNED) != 0,
                           (value & XHCI_LEGACY_BIOS_OWNED) != 0,
                           (value & XHCI_LEGACY_OS_OWNED) != 0, forced);
            return;
        }

        if (!next)
            return;
        offset += next << 2;
    }
}

void xhci_queue_port_change(xhci_controller_t *xhc, u8 port_id,
                            const char *source)
{
    u32 word;
    u32 bit;
    u32 before;
    u32 generation;

    if (!xhc || port_id == 0)
        return;
    word = port_id >> 5;
    bit = 1U << (port_id & 31U);
    if (word >= 8)
        return;
    before = __atomic_load_n(&xhc->pending_port_changes[word],
                             __ATOMIC_ACQUIRE);
    generation = __atomic_add_fetch(&xhc->port_change_generation, 1,
                                    __ATOMIC_ACQ_REL);
    __atomic_store_n(&xhc->pending_port_generation[port_id], generation,
                     __ATOMIC_RELEASE);
    __atomic_fetch_or(&xhc->pending_port_changes[word], bit,
                      __ATOMIC_RELEASE);
    XHCI_DEBUG_LOG(
        "[xHCI-QUEUE] t=%llu source=%s enqueue port=%u gen=%u "
        "pending-before=%u duplicate=%u boot=%u\n",
        (unsigned long long)timer_uptime_ms(), source ? source : "unknown",
        port_id, generation, (before & bit) != 0, (before & bit) != 0,
        __atomic_load_n(&xhc->boot_enumeration_active, __ATOMIC_ACQUIRE));
    __atomic_store_n(&xhc->event_work_pending, true, __ATOMIC_RELEASE);
    (void)xhci_start_deferred_worker(xhc);
}

bool xhci_take_port_change(xhci_controller_t *xhc, u8 *port_id)
{
    if (!xhc || !port_id)
        return false;

    for (u32 word = 0; word < 8; word++) {
        u32 observed = __atomic_load_n(&xhc->pending_port_changes[word],
                                       __ATOMIC_ACQUIRE);
        while (observed) {
            u32 bit = observed & (0U - observed);
            u32 desired = observed & ~bit;
            if (__atomic_compare_exchange_n(
                    &xhc->pending_port_changes[word], &observed, desired,
                    false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
                u32 bit_index = 0;
                while ((bit >> bit_index) != 1U)
                    bit_index++;
                *port_id = (u8)(word * 32U + bit_index);
                XHCI_DEBUG_LOG(
                    "[xHCI-QUEUE] t=%llu source=deferred-worker dequeue "
                    "port=%u gen=%u\n",
                    (unsigned long long)timer_uptime_ms(), *port_id,
                    __atomic_load_n(&xhc->pending_port_generation[*port_id],
                                    __ATOMIC_ACQUIRE));
                return true;
            }
        }
    }
    return false;
}

bool xhci_arm_command_wait(xhci_controller_t *xhc, u8 expected_cmd_type,
                           uintptr_t command_trb_phys, u8 expected_slot)
{
    if (!xhc || !expected_cmd_type || !command_trb_phys ||
        __atomic_load_n(&xhc->command_waiting, __ATOMIC_ACQUIRE))
        return false;
    xhc->command_generation = ++xhc->operation_generation;
    xhc->command_expected_type = expected_cmd_type;
    xhc->command_expected_slot = expected_slot;
    xhc->command_trb_phys = command_trb_phys;
    xhc->command_completion_code = XHCI_COMP_INVALID;
    __atomic_store_n(&xhc->command_completion_ready, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->command_waiting, true, __ATOMIC_RELEASE);
    return true;
}

void xhci_cancel_command_wait(xhci_controller_t *xhc)
{
    if (!xhc)
        return;
    __atomic_store_n(&xhc->command_waiting, false, __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->command_completion_ready, false,
                     __ATOMIC_RELEASE);
    xhc->command_expected_type = 0;
    xhc->command_expected_slot = 0;
    xhc->command_trb_phys = 0;
}

bool xhci_take_command_completion(xhci_controller_t *xhc,
                                  xhci_trb_t *out_event)
{
    if (!xhc || !out_event ||
        !__atomic_load_n(&xhc->command_completion_ready, __ATOMIC_ACQUIRE))
        return false;
    *out_event = xhc->command_completion;
    __atomic_store_n(&xhc->command_completion_ready, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->command_waiting, false, __ATOMIC_RELEASE);
    xhc->command_expected_type = 0;
    xhc->command_expected_slot = 0;
    xhc->command_trb_phys = 0;
    return true;
}

bool xhci_route_command_completion(xhci_controller_t *xhc,
                                   const xhci_trb_t *event)
{
    uintptr_t event_command;
    uintptr_t ring_end;
    u32 command_index;
    u8 command_type;
    u8 event_slot;
    if (!xhc || !event ||
        !__atomic_load_n(&xhc->command_waiting, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&xhc->command_completion_ready, __ATOMIC_ACQUIRE))
        return false;

    event_command = XHCI_TRB_PTR_GET(event->param1, event->param2);
    ring_end = xhc->cmd_ring.phys_base +
        xhc->cmd_ring.size * sizeof(xhci_trb_t);
    if (event_command != xhc->command_trb_phys ||
        event_command < xhc->cmd_ring.phys_base || event_command >= ring_end ||
        ((event_command - xhc->cmd_ring.phys_base) % sizeof(xhci_trb_t)) != 0)
        return false;
    command_index = (u32)((event_command - xhc->cmd_ring.phys_base) /
                          sizeof(xhci_trb_t));
    command_type = (u8)XHCI_TRB_CTRL_TYPE_GET(
        xhc->cmd_ring.trbs[command_index].control);
    if (command_type != xhc->command_expected_type)
        return false;
    event_slot = XHCI_TRB_CTRL_SLOT_ID_GET(event->control);
    if (xhc->command_expected_slot &&
        event_slot != xhc->command_expected_slot)
        return false;

    xhc->command_completion_code =
        XHCI_TRB_STS_COMP_CODE_GET(event->status);
    xhc->command_completion = *event;
    __atomic_store_n(&xhc->command_completion_ready, true,
                     __ATOMIC_RELEASE);
    return true;
}

static bool xhci_arm_transfer_operation(xhci_controller_t *xhc, u8 slot_id,
                                         u8 dci, uintptr_t td_start,
                                         uintptr_t td_end,
                                         uintptr_t expected_completion_trb,
                                         bool synchronous)
{
    xhci_transfer_record_t *record;
    if (!xhc || !slot_id || !dci ||
        dci >= 32 || !td_start || !td_end || !expected_completion_trb)
        return false;
    if (synchronous &&
        __atomic_load_n(&xhc->transfer_waiting, __ATOMIC_ACQUIRE))
        return false;
    record = NULL;
    for (u32 record_index = 0;
         record_index < XHCI_TRANSFER_RECORD_SLOTS; record_index++) {
        xhci_transfer_record_t *candidate =
            &xhc->devices[slot_id].transfer_records[dci][record_index];
        if (!candidate->pending && (!synchronous || record_index == 0)) {
            record = candidate;
            break;
        }
    }
    if (!record)
        return false;
    record->pending = true;
    record->synchronous = synchronous;
    record->generation = ++xhc->operation_generation;
    record->slot_id = slot_id;
    record->dci = dci;
    record->td_start = td_start;
    record->td_end = td_end;
    record->expected_completion_trb = expected_completion_trb;
    record->completion_code = XHCI_COMP_INVALID;
    if (!synchronous)
        return true;

    xhc->transfer_wait_slot = slot_id;
    xhc->transfer_wait_dci = dci;
    xhc->transfer_wait_generation = record->generation;
    __atomic_store_n(&xhc->transfer_completion_ready, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->transfer_waiting, true, __ATOMIC_RELEASE);
    return true;
}

bool xhci_arm_transfer_wait(xhci_controller_t *xhc, u8 slot_id, u8 dci,
                            uintptr_t td_start, uintptr_t td_end,
                            uintptr_t expected_completion_trb)
{
    return xhci_arm_transfer_operation(xhc, slot_id, dci, td_start, td_end,
                                       expected_completion_trb, true);
}

bool xhci_arm_async_transfer(xhci_controller_t *xhc, u8 slot_id, u8 dci,
                             uintptr_t td_start, uintptr_t td_end,
                             uintptr_t expected_completion_trb)
{
    return xhci_arm_transfer_operation(xhc, slot_id, dci, td_start, td_end,
                                       expected_completion_trb, false);
}

void xhci_cancel_transfer_operation(xhci_controller_t *xhc, u8 slot_id,
                                    u8 dci)
{
    if (!xhc || !slot_id || dci >= 32)
        return;
    for (u32 record_index = 0;
         record_index < XHCI_TRANSFER_RECORD_SLOTS; record_index++)
        xhc->devices[slot_id].transfer_records[dci][record_index].pending = false;
    if (__atomic_load_n(&xhc->transfer_waiting, __ATOMIC_ACQUIRE) &&
        xhc->transfer_wait_slot == slot_id && xhc->transfer_wait_dci == dci)
        xhci_cancel_transfer_wait(xhc);
}

void xhci_cancel_transfer_wait(xhci_controller_t *xhc)
{
    xhci_transfer_record_t *record;
    if (!xhc)
        return;
    __atomic_store_n(&xhc->transfer_waiting, false, __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->transfer_completion_ready, false,
                     __ATOMIC_RELEASE);
    if (xhc->transfer_wait_slot && xhc->transfer_wait_dci < 32) {
        record = &xhc->devices[xhc->transfer_wait_slot]
                      .transfer_records[xhc->transfer_wait_dci][0];
        if (record->generation == xhc->transfer_wait_generation)
            record->pending = false;
    }
    xhc->transfer_wait_slot = 0;
    xhc->transfer_wait_dci = 0;
    xhc->transfer_wait_generation = 0;
}

bool xhci_take_transfer_completion(xhci_controller_t *xhc,
                                   xhci_trb_t *out_event)
{
    if (!xhc || !out_event ||
        !__atomic_load_n(&xhc->transfer_completion_ready, __ATOMIC_ACQUIRE))
        return false;
    *out_event = xhc->transfer_completion;
    __atomic_store_n(&xhc->transfer_completion_ready, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->transfer_waiting, false, __ATOMIC_RELEASE);
    if (xhc->transfer_wait_slot && xhc->transfer_wait_dci < 32) {
        xhci_transfer_record_t *record =
            &xhc->devices[xhc->transfer_wait_slot]
                 .transfer_records[xhc->transfer_wait_dci][0];
        if (record->generation == xhc->transfer_wait_generation)
            record->pending = false;
    }
    xhc->transfer_wait_slot = 0;
    xhc->transfer_wait_dci = 0;
    xhc->transfer_wait_generation = 0;
    return true;
}

xhci_transfer_event_route_t xhci_route_transfer_event(
    xhci_controller_t *xhc, const xhci_trb_t *event)
{
    xhci_transfer_record_t *record;
    xhci_transfer_record_t *related_record;
    xhci_ring_t *ring;
    u8 slot;
    u8 dci;
    uintptr_t completion_trb;
    u32 completion_code;
    if (!xhc || !event)
        return XHCI_TRANSFER_EVENT_STALE;
    slot = XHCI_TRB_CTRL_SLOT_ID_GET(event->control);
    dci = XHCI_TRB_CTRL_EP_ID_GET(event->control);
    if (!slot || slot > xhc->max_slots || dci >= 32)
        return XHCI_TRANSFER_EVENT_STALE;
    completion_trb = XHCI_TRB_PTR_GET(event->param1, event->param2);
    completion_code = XHCI_TRB_STS_COMP_CODE_GET(event->status);
    ring = xhci_get_ep_ring(xhc, slot, dci);
    record = NULL;
    related_record = NULL;
    for (u32 record_index = 0;
         record_index < XHCI_TRANSFER_RECORD_SLOTS; record_index++) {
        xhci_transfer_record_t *candidate =
            &xhc->devices[slot].transfer_records[dci][record_index];
        if (candidate->pending && candidate->slot_id == slot &&
            candidate->dci == dci &&
            xhci_ring_trb_in_range(ring, candidate->td_start,
                                   candidate->td_end, completion_trb)) {
            related_record = candidate;
            /* Success completes the operation only at its declared terminal
               TRB.  A true error belongs to whichever Setup/Data/Status TRB
               detected it, as required for xHCI control transfers. */
            if (completion_trb == candidate->expected_completion_trb ||
                (completion_code != XHCI_COMP_SUCCESS &&
                 completion_code != XHCI_COMP_SHORT_PACKET)) {
                record = candidate;
                break;
            }
        }
    }
    if (!record) {
        /* A Data Stage short-packet event is progress, not completion of the
           control operation; the following Status Stage remains authoritative. */
        if (related_record && completion_code == XHCI_COMP_SHORT_PACKET)
            return XHCI_TRANSFER_EVENT_PROGRESS;
        return XHCI_TRANSFER_EVENT_STALE;
    }

    record->completion_code = completion_code;
    if (record->synchronous) {
        if (!__atomic_load_n(&xhc->transfer_waiting, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&xhc->transfer_completion_ready,
                            __ATOMIC_ACQUIRE) ||
            record->generation != xhc->transfer_wait_generation)
            return XHCI_TRANSFER_EVENT_STALE;
        xhc->transfer_completion = *event;
        __atomic_store_n(&xhc->transfer_completion_ready, true,
                         __ATOMIC_RELEASE);
        return XHCI_TRANSFER_EVENT_WAITING;
    }

    return XHCI_TRANSFER_EVENT_ASYNC;
}

bool xhci_complete_async_transfer(xhci_controller_t *xhc, u8 slot_id,
                                  u8 dci, uintptr_t completion_trb)
{
    xhci_ring_t *ring;
    if (!xhc || !slot_id || dci >= 32 || !completion_trb)
        return false;
    ring = xhci_get_ep_ring(xhc, slot_id, dci);

    for (u32 record_index = 0;
         record_index < XHCI_TRANSFER_RECORD_SLOTS; record_index++) {
        xhci_transfer_record_t *record =
            &xhc->devices[slot_id].transfer_records[dci][record_index];
        if (record->pending && !record->synchronous &&
            record->slot_id == slot_id && record->dci == dci &&
            record->expected_completion_trb == completion_trb &&
            xhci_ring_trb_in_range(ring, record->td_start, record->td_end,
                                   completion_trb)) {
            record->pending = false;
            return true;
        }
    }
    return false;
}

static void xhci_deferred_worker_entry(void *argument)
{
    xhci_controller_t *xhc = (xhci_controller_t *)argument;
    XHCI_DEBUG_LOG("[xHCI-INIT] worker-enter\n");

    while (!__atomic_load_n(&xhc->deferred_worker_stop, __ATOMIC_ACQUIRE)) {
        u8 port_id;
        /* The service thread is the sole Event Ring consumer. */
        __atomic_store_n(&xhc->event_work_pending, false, __ATOMIC_RELEASE);
        xhci_process_events(xhc);

        /* Boot probing and runtime hotplug use the same owner-driven
           orchestration.  The caller that requested boot probing waits on
           the port state; it does not run reset/setup itself. */
        while (xhci_take_port_change(xhc, &port_id)) {
            XHCI_DEBUG_LOG("[xHCI-QUEUE] worker-process port=%u\n", port_id);
            xhci_process_deferred_port_change(xhc, port_id);
        }

        /* Close the wakeup race: no IRQ can set the pending bit between this
           check and scheduler_block().  The original IF state is restored
           after the thread is woken. */
        u64 saved_flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
        bool work_pending = __atomic_load_n(&xhc->event_work_pending,
                                            __ATOMIC_ACQUIRE);
        if (!work_pending) {
            for (u32 word = 0; word < 8 && !work_pending; word++)
                work_pending = __atomic_load_n(&xhc->pending_port_changes[word],
                                               __ATOMIC_ACQUIRE) != 0;
        }
        if (!work_pending) {
            (void)scheduler_block();
        } else {
            (void)scheduler_yield();
        }
        __asm__ volatile("pushq %0; popfq" :: "r"(saved_flags) : "memory");
    }
}

typedef struct {
    u8 port;
    u8 slot;
    xhci_speed_t speed;
    u16 ep0_mps;
    u8 last_cc;
    u64 timeline_start_ms;
    const char *phase;
} xhci_diag_state_t;

static xhci_diag_state_t g_xhci_diag;
static bool g_xhci_diag_control_quiet;

void xhci_diag_set_context(u8 port, u8 slot, xhci_speed_t speed)
{
    g_xhci_diag.port = port;
    g_xhci_diag.slot = slot;
    g_xhci_diag.speed = speed;
}

void xhci_diag_timeline(const char *stage, u32 portsc)
{
    xhci_diag_timeline_at(stage, 0, portsc);
}

void xhci_diag_timeline_at(const char *stage, uintptr_t portsc_addr, u32 portsc)
{
    u64 now = timer_uptime_ms();
    if (stage && stage[0] == 'r' && stage[1] == 'e' && stage[2] == 's' &&
        stage[3] == 'e' && stage[4] == 't' && stage[5] == '-')
        g_xhci_diag.timeline_start_ms = now;
    u64 elapsed = now - g_xhci_diag.timeline_start_ms;
    XHCI_DEBUG_LOG("[xHCI-TIME] %llums +%llums p%u s%u st=%s a=%p ps=%08x ccs=%u ped=%u pr=%u prc=%u pls=%u sp=%u pp=%u ch=%08x\n",
           (unsigned long long)now, (unsigned long long)elapsed,
           g_xhci_diag.port, g_xhci_diag.slot, stage ? stage : "?",
           (void *)portsc_addr, portsc,
           (portsc & XHCI_PORTSC_CCS) != 0, (portsc & XHCI_PORTSC_PED) != 0,
           (portsc & XHCI_PORTSC_PR) != 0, (portsc & XHCI_PORTSC_PRC) != 0,
           (portsc & XHCI_PORTSC_PLS_MASK) >> 5, XHCI_PORTSC_SPEED(portsc),
           (portsc & XHCI_PORTSC_PP) != 0, portsc & XHCI_PORTSC_RW1C_MASK);
}

void xhci_diag_timeline_port(xhci_controller_t *xhc, const char *stage)
{
    volatile u32 *portsc = xhci_get_portsc_ptr(xhc, g_xhci_diag.port);
    xhci_diag_timeline_at(stage, (uintptr_t)portsc, portsc ? *portsc : 0);
}

void xhci_diag_set_phase(const char *phase)
{
    g_xhci_diag.phase = phase;
}

void xhci_diag_set_control_quiet(bool quiet)
{
    g_xhci_diag_control_quiet = quiet;
}

void xhci_diag_address_dw1(xhci_controller_t *xhc, u8 slot, const char *stage)
{
    if (!xhc || slot == 0 || !stage || !xhc->devices[slot].in_ctx_virt) return;
    u32 ctx_sz = xhc->csz ? 64 : 32;
    volatile u32 *slot_dw1 = (volatile u32 *)
        ((u8 *)xhc->devices[slot].in_ctx_virt + ctx_sz + 4);
    XHCI_DEBUG_LOG("[xHCI-ADDR-STEP] %s dw1=%08x\n", stage, *slot_dw1);
}

void xhci_diag_address_context(xhci_controller_t *xhc, u8 slot,
                               uintptr_t input_ctx_phys,
                               const xhci_input_control_context_t *ctrl,
                               const xhci_slot_context_t *slot_ctx,
                               const xhci_ep_context_t *ep0_ctx)
{
    if (!xhc || !ctrl || !slot_ctx || !ep0_ctx) return;
    XHCI_DEBUG_LOG("[xHCI-ADDR] p%u s%u sp%u csz=%u hcc=%08x ip=%p dcba=%p d[%u]=%016llx\n",
           g_xhci_diag.port, slot, g_xhci_diag.speed, xhc->csz,
           xhc->cap_regs->hccparams1, (void *)input_ctx_phys,
           (void *)xhc->dcbaa_phys, slot, (unsigned long long)xhc->dcbaa[slot]);
    XHCI_DEBUG_LOG("[xHCI-ADDR] ic=%08x/%08x slot=%08x,%08x,%08x,%08x ep0=%08x,%08x dq=%08x:%08x i3=%08x\n",
           ctrl->drop_flags, ctrl->add_flags,
           slot_ctx->info1, slot_ctx->info2, slot_ctx->info3, slot_ctx->info4,
           ep0_ctx->info1, ep0_ctx->info2, ep0_ctx->tr_dq_hi,
           ep0_ctx->tr_dq_lo, ep0_ctx->info3);
    XHCI_DEBUG_LOG("[xHCI-ADDR-RAW] 00=%08x 40=%08x/%08x 80=%08x/%08x/%08x/%08x pad20=%08x\n",
           ((const volatile u32 *)ctrl)[0],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x40))[0],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x40))[1],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x80))[0],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x80))[1],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x80))[2],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x80))[3],
           ((const volatile u32 *)((const volatile u8 *)ctrl + 0x20))[0]);
}

void xhci_diag_failure(xhci_status_t result)
{
    kprint("[xHCI-HW-FAIL] port=%u slot=%u phase=%s cc=%u rc=%u\n",
           g_xhci_diag.port, g_xhci_diag.slot,
           g_xhci_diag.phase ? g_xhci_diag.phase : "unknown",
           g_xhci_diag.last_cc, result);
}

void xhci_diag_command_result(u8 trb_type, u8 slot, xhci_status_t result,
                              const xhci_trb_t *event)
{
    u32 cc = event ? XHCI_TRB_STS_COMP_CODE_GET(event->status) : 0;
    u32 residual = event ? XHCI_TRB_STS_XFER_LEN_GET(event->status) : 0;
    uintptr_t ptr = event ? XHCI_TRB_PTR_GET(event->param1, event->param2) : 0;
    g_xhci_diag.last_cc = (u8)cc;
    XHCI_DEBUG_LOG("[xHCI-DIAG] p%u s%u sp%u %s ep0 trb%u len0 cc%u res%u ev%p rc%u\n",
           g_xhci_diag.port, slot, g_xhci_diag.speed,
           g_xhci_diag.phase ? g_xhci_diag.phase : "command", trb_type,
           residual, (void *)ptr, result);
}

void xhci_diag_control_result(xhci_controller_t *xhc, u8 slot,
                              u8 bm_request_type, u8 request, u16 value,
                              u16 index, u16 length, u32 setup_ctrl,
                              u32 status_ctrl, u32 ring_before_enq,
                              u32 ring_before_deq, u32 ring_after_enq,
                              u32 ring_after_deq, xhci_status_t result,
                              const xhci_trb_t *event)
{
    u32 cc = event ? XHCI_TRB_STS_COMP_CODE_GET(event->status) : 0;
    u32 residual = event ? XHCI_TRB_STS_XFER_LEN_GET(event->status) : 0;
    uintptr_t ptr = event ? XHCI_TRB_PTR_GET(event->param1, event->param2) : 0;
    extern u32 xhci_get_ep0_state(xhci_controller_t *, u8);
    g_xhci_diag.last_cc = (u8)cc;
    if (g_xhci_diag_control_quiet)
        return;
    XHCI_DEBUG_LOG("[xHCI-DIAG] p%u s%u sp%u %s ep1 ctrl len%u cc%u res%u ev%p rc%u\n",
           g_xhci_diag.port, slot, g_xhci_diag.speed,
           g_xhci_diag.phase ? g_xhci_diag.phase : "control", length,
           cc, residual, (void *)ptr, result);
    XHCI_DEBUG_LOG("[xHCI-EP0] req=%02x/%02x v=%04x i=%04x l=%u trt=%u data=%s status=%s flags=%02x/%02x mps=%u ring=%u/%u>%u/%u halt=%u\n",
           bm_request_type, request, value, index, length,
           (setup_ctrl >> 16) & 3, (setup_ctrl & XHCI_TRB_CTRL_DIR_IN) ? "IN" : "OUT",
           (status_ctrl & XHCI_TRB_CTRL_DIR_IN) ? "IN" : "OUT",
           setup_ctrl & 0x7e, status_ctrl & 0x7e, g_xhci_diag.ep0_mps,
           ring_before_enq, ring_before_deq, ring_after_enq, ring_after_deq,
           xhci_get_ep0_state(xhc, slot));
}


/* ==============================================================================
 * Subsystem Accessor Implementations
 * These fulfill the externs relied upon by the other modules.
 * ============================================================================== */

volatile u32* xhci_get_doorbell_ptr(xhci_controller_t *xhc, u8 target_idx) {
    if (!xhc || !xhc->db_array) return NULL;
    return &xhc->db_array[target_idx];
}

xhci_ring_t* xhci_get_cmd_ring(xhci_controller_t *xhc) {
    return xhc ? &xhc->cmd_ring : NULL;
}

xhci_ring_t* xhci_get_event_ring(xhci_controller_t *xhc) {
    return xhc ? &xhc->event_ring : NULL;
}

xhci_ring_t* xhci_get_ep_ring(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    if (!xhc || slot_id == 0 || dci >= 32) return NULL;
    return &xhc->devices[slot_id].ep_rings[dci];
}

u8* xhci_get_ep_dma_buffer(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    if (!xhc || slot_id == 0 || dci >= 32) return NULL;
    return xhc->devices[slot_id].ep_buffers_virt[dci][0];
}

uintptr_t xhci_get_ep_dma_phys(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    if (!xhc || slot_id == 0 || dci >= 32) return 0;
    return xhc->devices[slot_id].ep_buffers_phys[dci][0];
}

u8* xhci_get_ep_dma_buffer_for_trb(xhci_controller_t *xhc, u8 slot_id,
                                    u8 dci, uintptr_t trb_phys)
{
    xhci_ring_t *ring;
    uintptr_t offset;
    u32 index;
    if (!xhc || slot_id == 0 || dci >= 32)
        return NULL;
    ring = &xhc->devices[slot_id].ep_rings[dci];
    if (!ring->trbs || trb_phys < ring->phys_base)
        return NULL;
    offset = trb_phys - ring->phys_base;
    if ((offset % sizeof(xhci_trb_t)) != 0 ||
        offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t)))
        return NULL;
    index = (u32)(offset / sizeof(xhci_trb_t));
    if (index >= ring->size - 1)
        return NULL;
    return xhc->devices[slot_id].ep_buffers_virt[dci][
        index % XHCI_TRANSFER_RECORD_SLOTS];
}

uintptr_t xhci_get_ep_dma_phys_for_trb(xhci_controller_t *xhc, u8 slot_id,
                                        u8 dci, uintptr_t trb_phys)
{
    xhci_ring_t *ring;
    uintptr_t offset;
    u32 index;
    if (!xhc || slot_id == 0 || dci >= 32)
        return 0;
    ring = &xhc->devices[slot_id].ep_rings[dci];
    if (!ring->trbs || trb_phys < ring->phys_base)
        return 0;
    offset = trb_phys - ring->phys_base;
    if ((offset % sizeof(xhci_trb_t)) != 0 ||
        offset >= (uintptr_t)(ring->size * sizeof(xhci_trb_t)))
        return 0;
    index = (u32)(offset / sizeof(xhci_trb_t));
    if (index >= ring->size - 1)
        return 0;
    return xhc->devices[slot_id].ep_buffers_phys[dci][
        index % XHCI_TRANSFER_RECORD_SLOTS];
}

bool xhci_is_hid_endpoint(xhci_controller_t *xhc, u8 slot_id, u8 dci)
{
    if (!xhc || !slot_id || !dci || dci >= 32)
        return false;
    xhci_device_t *dev = &xhc->devices[slot_id];
    return dev->slot_id == slot_id &&
           (dev->class_flags & XHCI_DEVICE_CLASS_HID) != 0 &&
           dev->hid_dci == dci;
}

xhci_intr_regs_t* xhci_get_intr_regs(xhci_controller_t *xhc, u8 interrupter_idx) {
    if (!xhc || !xhc->run_regs || interrupter_idx >= xhc->max_intrs) return NULL;
    return (xhci_intr_regs_t *)&xhc->run_regs->irs[interrupter_idx];
}

static volatile xhci_ep_context_t *xhci_output_ep0_context(
    xhci_controller_t *xhc, u8 slot)
{
    if (!xhc || !slot || !xhc->devices[slot].out_ctx_virt)
        return NULL;
    return (volatile xhci_ep_context_t *)
        ((u8 *)xhc->devices[slot].out_ctx_virt + (xhc->csz ? 64U : 32U));
}

xhci_op_regs_t* xhci_get_op_regs(xhci_controller_t *xhc) {
    return xhc ? xhc->op_regs : NULL;
}

u8 xhci_get_max_ports(xhci_controller_t *xhc) {
    return xhc ? (u8)xhc->max_ports : 0;
}

xhci_hid_keyboard_callback_t xhci_get_keyboard_callback(xhci_controller_t *xhc) {
    return xhc ? xhc->keyboard_callback : NULL;
}

u32 xhci_get_ep0_state(xhci_controller_t *xhc, u8 slot_id)
{
    volatile xhci_ep_context_t *ep0 =
        xhci_output_ep0_context(xhc, slot_id);
    if (!ep0)
        return 0;
    return XHCI_EP_CTX_STATE_GET(ep0->info1);
}


/* ==============================================================================
 * Core Controller API (Phase 1)
 * ============================================================================== */

xhci_controller_t* xhci_init(uintptr_t mmio_base, u8 irq_number) {
    xhci_controller_t *xhc = &g_xhc_instance;

    if (g_xhc_init_started)
        return NULL;
    g_xhc_init_started = true;

    __builtin_memset(xhc, 0, sizeof(*xhc));
    xhc->state = XHCI_CONTROLLER_UNINITIALIZED;
    xhci_set_controller_state(xhc, XHCI_CONTROLLER_CLAIMING_FIRMWARE,
                              "ownership-entry");

    xhc->mmio_base = mmio_base;
    xhc->irq_number = irq_number;
    xhc->is_running = false;
    xhc->keyboard_callback = NULL;
    xhc->boot_enumeration_active = true;
    xhc->deferred_worker_stop = false;
    xhc->event_work_pending = false;
    xhc->operation_generation = 0;
    xhc->command_generation = 0;
    xhc->command_expected_type = 0;
    xhc->command_expected_slot = 0;
    xhc->command_trb_phys = 0;
    xhc->command_completion_code = XHCI_COMP_INVALID;
    xhc->command_waiting = false;
    xhc->command_completion_ready = false;
    xhc->transfer_waiting = false;
    xhc->transfer_completion_ready = false;
    xhc->transfer_wait_slot = 0;
    xhc->transfer_wait_dci = 0;
    xhc->transfer_wait_generation = 0;
    xhc->deferred_worker = NULL;
    xhc->port_change_generation = 0;
    for (u32 i = 0; i < 8; i++)
        xhc->pending_port_changes[i] = 0;
    for (u32 i = 0; i < 256; i++)
        xhc->pending_port_generation[i] = 0;

    /* 1. Map Registers */
    xhc->cap_regs = (xhci_cap_regs_t *)mmio_base;
    xhc->op_regs = (xhci_op_regs_t *)(mmio_base + xhc->cap_regs->caplength);
    xhc->db_array = (volatile u32 *)(mmio_base + xhc->cap_regs->dboff);
    xhc->run_regs = (xhci_run_regs_t *)(mmio_base + xhc->cap_regs->rtsoff);

    xhci_legacy_handoff(xhc);
    xhci_set_controller_state(xhc, XHCI_CONTROLLER_RESETTING,
                              "firmware-ownership-claimed");

    XHCI_DEBUG_LOG("[xHCI-INIT] reset-start sts=%08x cmd=%08x\n",
           xhc->op_regs->usbsts, xhc->op_regs->usbcmd);

    /* 2. Wait for Controller Not Ready (CNR) to clear */
    if (!xhci_wait_register_state(&xhc->op_regs->usbsts,
                                  XHCI_USBSTS_CNR, false)) {
        xhci_set_controller_state(xhc, XHCI_CONTROLLER_FAILED,
                                  "initial-cnr-timeout");
        return NULL;
    }

    /* 3. Halt Controller */
    xhc->op_regs->usbcmd &= ~XHCI_USBCMD_RS;
    if (!xhci_wait_register_state(&xhc->op_regs->usbsts,
                                  XHCI_USBSTS_HCH, true)) {
        xhci_set_controller_state(xhc, XHCI_CONTROLLER_FAILED,
                                  "initial-halt-timeout");
        return NULL;
    }

    /* 4. Host Controller Reset (HCRST) */
    xhc->op_regs->usbcmd |= XHCI_USBCMD_HCRST;
    if (!xhci_wait_register_state(&xhc->op_regs->usbcmd,
                                  XHCI_USBCMD_HCRST, false) ||
        !xhci_wait_register_state(&xhc->op_regs->usbsts,
                                  XHCI_USBSTS_CNR, false)) {
        xhci_set_controller_state(xhc, XHCI_CONTROLLER_FAILED,
                                  "reset-ready-timeout");
        return NULL;
    }
    XHCI_DEBUG_LOG("[xHCI-INIT] reset-done sts=%08x cmd=%08x\n",
           xhc->op_regs->usbsts, xhc->op_regs->usbcmd);
    
     /* 5. Parse Capabilities */
    xhc->max_slots = XHCI_HCSPARAMS1_MAX_SLOTS(xhc->cap_regs->hcsparams1);
    xhc->max_ports = XHCI_HCSPARAMS1_MAX_PORTS(xhc->cap_regs->hcsparams1);
    xhc->max_intrs = XHCI_HCSPARAMS1_MAX_INTRS(xhc->cap_regs->hcsparams1);
    u32 max_scratchpads = XHCI_HCSPARAMS2_MAX_SCRATCH(xhc->cap_regs->hcsparams2);
    xhc->csz = (xhc->cap_regs->hccparams1 & XHCI_HCCPARAMS1_CSZ) ? 1 : 0;
    xhc->page_size = 4096;
    XHCI_DEBUG_LOG("[xHCI-INIT] caps slots=%u ports=%u intrs=%u scratch=%u csz=%u\n",
                   xhc->max_slots, xhc->max_ports, xhc->max_intrs,
                   max_scratchpads, xhc->csz);


    /* 6. Program Max Device Slots */
    xhc->op_regs->config = xhc->max_slots & XHCI_CONFIG_MAX_SLOTS_EN_MASK;

    /* 7. Allocate and Set DCBAA */
    xhc->dcbaa = xhci_alloc_dcbaa(xhc->max_slots, &xhc->dcbaa_phys);
    if (!xhc->dcbaa) {
        xhci_set_controller_state(xhc, XHCI_CONTROLLER_FAILED,
                                  "dcbaa-allocation");
        return NULL;
    }
    xhc->op_regs->dcbaap = xhc->dcbaa_phys;

    /* 8. Allocate and Set Scratchpads */
    uintptr_t sp_phys = 0;
    xhc->scratchpads = xhci_alloc_scratchpads(max_scratchpads, xhc->page_size, &sp_phys);
    if (max_scratchpads > 0 && sp_phys != 0) {
        xhc->dcbaa[0] = sp_phys;
    }


    /* 9. Allocate Command Ring */
    xhci_ring_alloc(&xhc->cmd_ring, XHCI_RING_TRBS_PER_PAGE, false);
    xhc->op_regs->crcr = xhc->cmd_ring.phys_base | XHCI_CRCR_RCS;

    /* 10. Allocate Event Ring and ERST */
    xhci_ring_alloc(&xhc->event_ring, XHCI_RING_TRBS_PER_PAGE, true);

    uintptr_t erst_phys = 0;
    xhc->erst = xhci_alloc_erst(1, &erst_phys);

    xhc->erst[0].base_address = xhc->event_ring.phys_base;
    xhc->erst[0].size = XHCI_RING_TRBS_PER_PAGE;
    xhci_intr_regs_t *ir0 = xhci_get_intr_regs(xhc, 0);

    ir0->erstsz = 1;
    ir0->erdp = xhc->event_ring.phys_base;
    ir0->erstba = erst_phys;

    /* 11. Enable Interrupter 0 */
    ir0->iman |= XHCI_IMAN_IE;
    XHCI_DEBUG_LOG("[xHCI-INIT] interrupter-ready\n");
    xhci_set_controller_state(xhc, XHCI_CONTROLLER_RINGS_READY,
                              "rings-and-interrupter-ready");
    /* Reserve the worker's stack before device enumeration consumes the
       early heap.  Keep it suspended until synchronous boot probing is done. */
    xhc->deferred_worker = thread_create_suspended(
        "xhci-service", xhci_deferred_worker_entry, xhc);
    XHCI_DEBUG_LOG("[xHCI-INIT] worker-reserved=%u\n",
                   xhc->deferred_worker != NULL);
    if (!xhc->deferred_worker) {
        xhci_set_controller_state(xhc, XHCI_CONTROLLER_FAILED,
                                  "service-worker-allocation");
        return NULL;
    }
    return xhc;
}

xhci_status_t xhci_start(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;
    if (xhc->state != XHCI_CONTROLLER_RINGS_READY)
        return XHCI_ERR_CONTROLLER_BAD;

    XHCI_DEBUG_LOG("[xHCI-INIT] start-enter sts=%08x cmd=%08x\n",
           xhc->op_regs->usbsts, xhc->op_regs->usbcmd);

    /* Enable global interrupts and set Run/Stop */
    xhc->op_regs->usbcmd |= (XHCI_USBCMD_INTE | XHCI_USBCMD_RS);
    
    if (!xhci_wait_register_state(&xhc->op_regs->usbsts,
                                  XHCI_USBSTS_HCH, false)) {
        return XHCI_ERR_TIMEOUT;
    }
    
    xhc->is_running = true;
    xhci_set_controller_state(xhc, XHCI_CONTROLLER_RUNNING,
                              "run-stop-cleared");
    /* The worker is reserved during xhci_init.  The first submitted event or
       port work starts it after this synchronous start call has returned;
       this avoids scheduling a blocking service thread in the middle of the
       controller-start handoff. */
    xhci_set_controller_state(xhc, XHCI_CONTROLLER_SERVICE_READY,
                              "sole-event-owner-reserved");
    XHCI_DEBUG_LOG("[xHCI-INIT] start-done sts=%08x cmd=%08x\n",
           xhc->op_regs->usbsts, xhc->op_regs->usbcmd);
    return XHCI_SUCCESS;
}

void xhci_acknowledge_boot_interrupts(xhci_controller_t *xhc)
{
    if (!xhc || !xhc->op_regs)
        return;

    xhci_intr_regs_t *ir0 = xhci_get_intr_regs(xhc, 0);
    if (!ir0)
        return;

    /* Boot probing owns enumeration.  This helper only acknowledges
       controller/port change state; the service thread owns Event Ring
       dequeue and dispatch. */
    for (u8 port = 1; port <= xhc->max_ports; port++) {
        volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port);
        if (!portsc)
            continue;
        u32 value = *portsc;
        u32 changes = value & XHCI_PORTSC_RW1C_MASK;
        if (changes) {
            value &= ~XHCI_PORTSC_RW1CS_MASK;
            *portsc = value | changes;
        }
    }

    u32 status = xhc->op_regs->usbsts;
    u32 iman = ir0->iman;
    u64 erdp = ir0->erdp;
    XHCI_DEBUG_LOG("[xHCI-MSI] pre-unmask sts=%08x iman=%08x erdp=%p\n",
           status, iman, (void *)(uintptr_t)erdp);

    u32 status_ack = status & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD);
    if (status_ack)
        xhc->op_regs->usbsts = status_ack;
    if (iman & XHCI_IMAN_IP)
        ir0->iman = (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP;

    (void)xhc->op_regs->usbsts;

    XHCI_DEBUG_LOG("[xHCI-MSI] acked sts=%08x iman=%08x erdp=%p\n",
           xhc->op_regs->usbsts, ir0->iman,
           (void *)(uintptr_t)ir0->erdp);
}

void xhci_begin_boot_enumeration(xhci_controller_t *xhc)
{
    if (!xhc)
        return;
    if (xhc->state == XHCI_CONTROLLER_SERVICE_READY)
        xhci_set_controller_state(xhc,
                                  XHCI_CONTROLLER_BOOT_ENUMERATING,
                                  "root-port-probe-started");
}

void xhci_complete_boot_enumeration(xhci_controller_t *xhc)
{
    if (!xhc)
        return;
    /* Do not discard work that arrived while boot enumeration was active.
       The same service owner drains it after the boot caller releases its
       wait, just like a runtime hotplug event. */
    __atomic_store_n(&xhc->boot_enumeration_active, false,
                     __ATOMIC_RELEASE);
    if (xhc->state == XHCI_CONTROLLER_BOOT_ENUMERATING)
        xhci_set_controller_state(xhc, XHCI_CONTROLLER_BOOT_QUIESCENT,
                                  "boot-enumeration-finished");
}

bool xhci_start_deferred_worker(xhci_controller_t *xhc)
{
    if (!xhc)
        return false;
    if (!xhc->deferred_worker)
        return false;
    XHCI_DEBUG_LOG("[xHCI-INIT] worker-enqueue-enter state=%u queued=%u\n",
                   xhc->deferred_worker->state,
                   xhc->deferred_worker->queued);
    if (xhc->deferred_worker->queued)
        return true;

    if (xhc->deferred_worker->state == THREAD_STATE_BLOCKED)
        return scheduler_unblock(xhc->deferred_worker);
    if (xhc->deferred_worker->state != THREAD_STATE_READY)
        return false;

    if (!scheduler_enqueue(xhc->deferred_worker)) {
        XHCI_DEBUG_LOG("[xHCI-INIT] worker-enqueue-failed\n");
        return false;
    }
    XHCI_DEBUG_LOG("[xHCI-INIT] worker-enqueued\n");
    return true;
}

void xhci_mark_event_work_pending(xhci_controller_t *xhc)
{
    if (xhc)
        __atomic_store_n(&xhc->event_work_pending, true, __ATOMIC_RELEASE);
}

bool xhci_is_service_owner(xhci_controller_t *xhc)
{
    return xhc && xhc->deferred_worker &&
           thread_current() == xhc->deferred_worker;
}

xhci_status_t xhci_stop(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    xhc->op_regs->usbcmd &= ~XHCI_USBCMD_RS;
    if (!xhci_wait_register_state(&xhc->op_regs->usbsts,
                                  XHCI_USBSTS_HCH, true)) {
        return XHCI_ERR_TIMEOUT;
    }

    xhc->is_running = false;
    return XHCI_SUCCESS;
}

xhci_status_t xhci_reset(xhci_controller_t *xhc) {
    xhci_status_t result = xhci_stop(xhc);
    if (result != XHCI_SUCCESS)
        return result;
    xhc->op_regs->usbcmd |= XHCI_USBCMD_HCRST;
    return xhci_wait_register_state(&xhc->op_regs->usbcmd,
                                    XHCI_USBCMD_HCRST, false) ?
        XHCI_SUCCESS : XHCI_ERR_TIMEOUT;
}

void xhci_shutdown(xhci_controller_t *xhc) {
    if (!xhc) return;
    __atomic_store_n(&xhc->deferred_worker_stop, true, __ATOMIC_RELEASE);
    if (xhc->deferred_worker && !xhc->deferred_worker->queued &&
        xhc->deferred_worker->state == THREAD_STATE_READY) {
        (void)thread_destroy(xhc->deferred_worker);
        xhc->deferred_worker = NULL;
    }
    xhci_stop(xhc);
    xhci_ring_free(&xhc->cmd_ring);
    xhci_ring_free(&xhc->event_ring);
    xhc->is_running = false;
    g_xhc_init_started = false;
}


/* ==============================================================================
 * Hardware Interrupt Entry
 * ============================================================================== */

void xhci_interrupt_handler(xhci_controller_t *xhc) {
    if (!xhc || !xhc->op_regs) return;

    u32 status = xhc->op_regs->usbsts;
    if (status & XHCI_USBSTS_EINT) {
        xhci_intr_regs_t *ir0 = xhci_get_intr_regs(xhc, 0);

        /* USBSTS.EINT and IMAN.IP are write-one-to-clear.  Do not write back
           unrelated status bits or use a read/modify/write on IMAN: either
           can acknowledge more state than this interrupt owns. */
        xhc->op_regs->usbsts = XHCI_USBSTS_EINT;

        if (ir0) {
            u32 iman = ir0->iman;
            ir0->iman = (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP;
        }

        /* IRQ context records work and wakes the sole service owner. */
        __atomic_store_n(&xhc->event_work_pending, true, __ATOMIC_RELEASE);
        (void)xhci_start_deferred_worker(xhc);
    }
}


/* ==============================================================================
 * High-Level Device Initialization Pipeline (Phase 3 -> 7)
 * Exposed dynamically for the OS to configure newly attached USB devices.
 * ============================================================================== */

static u8 xhci_interrupt_interval(xhci_speed_t speed, u8 descriptor_interval)
{
    if (!descriptor_interval)
        descriptor_interval = 1;

    if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
        u32 microframes = (u32)descriptor_interval * 8U;
        u8 exponent = 0;

        while (exponent < 10 &&
               (1U << (exponent + 1U)) <= microframes)
            exponent++;
        return exponent < 3 ? 3 : exponent;
    }

    if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER) {
        if (descriptor_interval > 16)
            descriptor_interval = 16;
        return descriptor_interval - 1U;
    }

    return 0;
}

static xhci_status_t xhci_setup_finish(xhci_controller_t *xhc,
                                       xhci_status_t result,
                                       bool owns_lock)
{
    if (owns_lock)
        __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
    return result;
}

static bool xhci_cleanup_failed_device(xhci_controller_t *xhc,
                                       xhci_device_t *dev)
{
    if (!xhc || !dev || !dev->slot_id)
        return false;

    u8 slot_id = dev->slot_id;
    bool disable_ok;

    xhci_diag_set_phase("disable-slot");
    disable_ok = xhci_cmd_disable_slot(xhc, slot_id) == XHCI_SUCCESS;
    if (!disable_ok) {
        kprint("[xHCI] slot %u cleanup incomplete; retry suppressed\n",
               slot_id);
        return false;
    }

    for (u8 dci = 1; dci < 32; dci++) {
        for (u8 record_index = 0;
             record_index < XHCI_TRANSFER_RECORD_SLOTS; record_index++)
            dev->transfer_records[dci][record_index].pending = false;
        for (u8 buffer_index = 0;
             buffer_index < XHCI_TRANSFER_RECORD_SLOTS; buffer_index++) {
            if (dev->ep_buffers_virt[dci][buffer_index]) {
                xhci_dma_free(dev->ep_buffers_virt[dci][buffer_index], 8);
                dev->ep_buffers_virt[dci][buffer_index] = NULL;
                dev->ep_buffers_phys[dci][buffer_index] = 0;
            }
        }
        xhci_ring_free(&dev->ep_rings[dci]);
    }

    if (xhc->dcbaa)
        xhc->dcbaa[slot_id] = 0;
    if (dev->in_ctx_virt) {
        xhci_dma_free(dev->in_ctx_virt,
                      (xhc->csz ? 64U : 32U) * 33U);
        dev->in_ctx_virt = NULL;
    }
    if (dev->out_ctx_virt) {
        xhci_dma_free(dev->out_ctx_virt,
                      (xhc->csz ? 64U : 32U) * 32U);
        dev->out_ctx_virt = NULL;
    }

    __builtin_memset(dev, 0, sizeof(*dev));
    dev->slot_id = slot_id;
    dev->state = XHCI_DEVICE_FAILED;
    dev->setup_finished = true;
    return true;
}

static xhci_status_t xhci_device_setup_finish(xhci_controller_t *xhc,
                                              xhci_device_t *dev,
                                              xhci_status_t result,
                                              bool owns_lock)
{
    bool ready = result == XHCI_SUCCESS && dev &&
        (dev->class_flags == 0 || dev->class_ready);
    if (result == XHCI_SUCCESS && !ready)
        result = XHCI_ERR_TRANSACTION;
    xhc->last_setup_retry_safe = false;
    if (result != XHCI_SUCCESS && dev) {
        bool was_hub = (dev->class_flags & XHCI_DEVICE_CLASS_HUB) != 0;
        bool cleanup_ok = xhci_cleanup_failed_device(xhc, dev);
        xhc->last_setup_retry_safe = cleanup_ok && !was_hub;
    }
    if (dev) {
        dev->setup_result = result;
        dev->setup_finished = true;
        xhci_set_device_state(xhc, dev->slot_id,
                              result == XHCI_SUCCESS ? XHCI_DEVICE_READY :
                                                       XHCI_DEVICE_FAILED,
                              result == XHCI_SUCCESS ? "setup-complete" :
                                                       "setup-failed");
    }
    return xhci_setup_finish(xhc, result, owns_lock);
}

static xhci_status_t xhci_configure_hub_slot(
    xhci_controller_t *xhc, xhci_device_t *dev,
    const xhci_hub_endpoint_info_t *hub_endpoint,
    const xhci_hub_descriptor_info_t *hub_descriptor)
{
    u32 ctx_sz = xhc->csz ? 64 : 32;
    xhci_input_control_context_t *control =
        (xhci_input_control_context_t *)dev->in_ctx_virt;
    xhci_slot_context_t *input_slot = (xhci_slot_context_t *)
        ((u8 *)dev->in_ctx_virt + ctx_sz);
    const volatile u32 *output_slot =
        (const volatile u32 *)dev->out_ctx_virt;
    volatile u32 *input_words = (volatile u32 *)input_slot;

    __builtin_memset(control, 0, ctx_sz);
    __builtin_memset(input_slot, 0, ctx_sz);
    for (u32 i = 0; i < ctx_sz / sizeof(u32); i++)
        input_words[i] = output_slot[i];

    input_slot->info1 |= XHCI_SLOT_CTX_HUB;
    input_slot->info2 &= ~XHCI_SLOT_CTX_NUM_PORTS_SET(0xff);
    input_slot->info2 |= XHCI_SLOT_CTX_NUM_PORTS_SET(
        hub_descriptor->num_ports);

    /* MTT and TTT describe a USB2 high-speed transaction translator. */
    input_slot->info1 &= ~XHCI_SLOT_CTX_MTT;
    input_slot->info3 &= ~XHCI_SLOT_CTX_TTT_SET(3);
    if (dev->speed == XHCI_SPEED_HIGH) {
        if (hub_endpoint->interface_protocol == 2)
            input_slot->info1 |= XHCI_SLOT_CTX_MTT;
        input_slot->info3 |= XHCI_SLOT_CTX_TTT_SET(
            (hub_descriptor->characteristics >> 5) & 3);
    }

    control->drop_flags = 0;
    control->add_flags = XHCI_CTX_FLAG_SLOT;
    xhci_diag_set_phase("hub-slot-context");
    return xhci_cmd_configure_endpoint(xhc, dev->slot_id,
                                       dev->in_ctx_phys);
}

/*
 * Sweeps a root-port device or downstream child through Enable, Addressing,
 * descriptor parsing, and class configuration.
 */
static xhci_status_t xhci_setup_device_topology(
    xhci_controller_t *xhc, u8 port_id, xhci_speed_t speed,
    u32 route_string, u8 topology_depth, u8 parent_hub_slot,
    u8 parent_port, xhci_speed_t parent_speed, bool lock_held)
{
    if (!xhc) return XHCI_ERR_INVALID_PARAM;
    if (xhc->state != XHCI_CONTROLLER_SERVICE_READY &&
        xhc->state != XHCI_CONTROLLER_BOOT_ENUMERATING &&
        xhc->state != XHCI_CONTROLLER_BOOT_QUIESCENT)
        return XHCI_ERR_CONTROLLER_BAD;
    bool owns_lock = !lock_held;

    if (parent_hub_slot == 1 && parent_port == 2) {
        XHCI_DEBUG_LOG(
            "[xHCI-TOPO] t=%llu source=hub-child root=%u parent=s%u/p%u "
            "parent-speed=%u route=%05x depth=%u child-speed=%u lock=%u\n",
            (unsigned long long)timer_uptime_ms(), port_id,
            parent_hub_slot, parent_port, parent_speed, route_string,
            topology_depth, speed, lock_held);
    }

    xhci_diag_set_context(port_id, 0, speed);
    xhci_diag_set_phase("enable-slot");

    /* Spin until we safely acquire the critical section lock, blocking background hotplug interrupts */
    if (owns_lock) {
        while (__atomic_test_and_set(&xhc->in_critical_section,
                                     __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }
    }

    u8 slot_id = 0;
    xhci_status_t err;

    /* Phase 3: Enable Slot */
    err = xhci_cmd_enable_slot(xhc, &slot_id);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        return xhci_setup_finish(xhc, err, owns_lock);
    }


    xhci_device_t *dev = &xhc->devices[slot_id];
    dev->state = XHCI_DEVICE_NO_SLOT;
    dev->slot_id = slot_id;
    dev->port_id = port_id;
    dev->speed = speed;
    dev->route_string = route_string;
    dev->topology_depth = topology_depth;
    dev->parent_hub_slot = parent_hub_slot;
    dev->parent_port = parent_port;
    dev->parent_speed = parent_speed;
    dev->class_flags = 0;
    dev->hid_dci = 0;
    dev->hid_armed = false;
    dev->class_ready = false;
    __builtin_memset(dev->transfer_records, 0,
                     sizeof(dev->transfer_records));
    dev->setup_finished = false;
    dev->setup_result = XHCI_ERR_DEVICE_TIMEOUT;
    __builtin_memset(&dev->storage_probe, 0, sizeof(dev->storage_probe));
    xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_SLOT_ENABLED,
                          "enable-slot-complete");
    xhci_diag_set_context(port_id, slot_id, speed);
    xhci_diag_set_phase("input-context");
    XHCI_DEBUG_LOG("[xHCI-DIAG] p%u s%u sp%u input-context\n",
                   port_id, slot_id, speed);

    /* Allocate Output & Input Contexts */
    dev->out_ctx_virt = xhci_alloc_device_context(xhc->csz, &dev->out_ctx_phys);
    dev->in_ctx_virt = xhci_alloc_input_context(xhc->csz, &dev->in_ctx_phys);
    if (!dev->out_ctx_virt || !dev->in_ctx_virt)
        return xhci_device_setup_finish(xhc, dev, XHCI_ERR_NO_MEMORY,
                                         owns_lock);
    xhc->dcbaa[slot_id] = dev->out_ctx_phys;

    u32 ctx_sz = xhc->csz ? 64 : 32;
    xhci_input_control_context_t *ctrl_ctx = (xhci_input_control_context_t *)dev->in_ctx_virt;
    xhci_slot_context_t *slot_ctx = (xhci_slot_context_t *)((u8*)dev->in_ctx_virt + ctx_sz);
    xhci_ep_context_t *ep0_ctx = (xhci_ep_context_t *)((u8*)dev->in_ctx_virt + (2 * ctx_sz));

    /* 1. Initialize Input Control Context: Enable Slot (Bit 0) and EP0 (Bit 1 / DCI 1) */
    ctrl_ctx->drop_flags = 0;
    ctrl_ctx->add_flags = XHCI_CTX_FLAG_SLOT | XHCI_CTX_FLAG_EP(1);

    /* 2. Initialize Slot Context precisely following specification layout */
    slot_ctx->info1 = XHCI_SLOT_CTX_ROUTE_STRING_SET(route_string) |
                      XHCI_SLOT_CTX_SPEED_SET(speed) | 
                      XHCI_SLOT_CTX_ENTRIES_SET(1);
    slot_ctx->info2 = XHCI_SLOT_CTX_ROOT_HUB_PORT_SET(port_id) | 
                      XHCI_SLOT_CTX_MAX_EXIT_LAT_SET(0);
    slot_ctx->info3 = 0;
    if ((speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) &&
        parent_speed == XHCI_SPEED_HIGH) {
        slot_ctx->info3 = XHCI_SLOT_CTX_PARENT_HUB_ID_SET(parent_hub_slot) |
                          XHCI_SLOT_CTX_PARENT_PORT_SET(parent_port);
    }
    slot_ctx->info4 = 0;
    XHCI_DEBUG_LOG("[xHCI-ADDR-INIT] slot+04=%08x\n", slot_ctx->info2);

    /* 3. Allocate EP0 Transfer Ring */
    err = xhci_ring_alloc(&dev->ep_rings[1], XHCI_RING_TRBS_PER_PAGE, false);
    if (err != XHCI_SUCCESS)
        return xhci_device_setup_finish(xhc, dev, err, owns_lock);
    XHCI_DEBUG_LOG("[xHCI-ADDR-CHK] after-ep0-ring dw1=%08x in=%p/%p ep0ring=%p\n",
                   slot_ctx->info2, (void *)dev->in_ctx_phys,
                   (void *)(dev->in_ctx_phys + 0x1000),
                   (void *)dev->ep_rings[1].phys_base);

    /* 4. Determine default max packet size based on speed */
    u16 initial_max_pkt = 8;
    if (speed == 3) {
        initial_max_pkt = 64;   // High-Speed
    } else if (speed == 4) {
        initial_max_pkt = 512;  // Super-Speed
    } else {
        initial_max_pkt = 8;    // Full/Low-Speed
    }
    g_xhci_diag.ep0_mps = initial_max_pkt;

    /* 5. Initialize Endpoint 0 Context */
    ep0_ctx->info1 = 0; // State = 0 (Disabled / Stopped transition state for Address Device)
    ep0_ctx->info2 = XHCI_EP_CTX_TYPE_SET(XHCI_EP_TYPE_CONTROL) | 
                     XHCI_EP_CTX_MAX_PACKET_SET(initial_max_pkt) | 
                     XHCI_EP_CTX_CERR_SET(3);
    ep0_ctx->tr_dq_lo = (u32)(dev->ep_rings[1].phys_base & 0xFFFFFFFF) | XHCI_EP_CTX_TR_DQ_DCS;
    ep0_ctx->tr_dq_hi = (u32)((dev->ep_rings[1].phys_base >> 32) & 0xFFFFFFFF);
    ep0_ctx->info3 = XHCI_EP_CTX_AVG_TRB_LEN_SET(8); // Control endpoint average TRB length = 8 bytes
    XHCI_DEBUG_LOG("[xHCI-ADDR-CHK] after-ep0-init dw1=%08x ep0=%p\n",
                   slot_ctx->info2,
                   (void *)(dev->in_ctx_phys + (2 * ctx_sz)));

    /* Phase 4: Address Device */
    xhci_diag_set_phase("address-device");
    xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_ADDRESSING,
                          "address-command-submitted");
    volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port_id);

    /* USB2 7.1.7.5 gives a reset device 10 ms before the host may expect it
       to answer another transaction.  Hub-child reset already enforces this
       interval before returning; root-port reset records its exact observed
       completion and waits here, immediately before Address Device can put a
       SET_ADDRESS transaction on the wire. */
    if (!parent_hub_slot && speed != XHCI_SPEED_SUPER) {
        u64 reset_completed_ms = xhc->ports[port_id].reset_completed_ms;
        if (!reset_completed_ms)
            return xhci_device_setup_finish(xhc, dev,
                                            XHCI_ERR_PORT_RESET_FAIL,
                                            owns_lock);
        xhci_wait_until_ms(reset_completed_ms + USB2_RESET_RECOVERY_MS);
        xhci_diag_timeline_at("reset-recovery-complete",
                              (uintptr_t)portsc, portsc ? *portsc : 0);
    }
    xhci_diag_timeline_at("pre-address", (uintptr_t)portsc, portsc ? *portsc : 0);
    xhci_diag_address_context(xhc, slot_id, dev->in_ctx_phys, ctrl_ctx,
                              slot_ctx, ep0_ctx);
    err = xhci_cmd_address_device(xhc, slot_id, dev->in_ctx_phys, false);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        return xhci_device_setup_finish(xhc, dev, err, owns_lock);
    }
    xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_ADDRESSED,
                          "address-command-complete");

    /* xHCI leaves USB SetAddress recovery timing to software.  Completion of
       Address Device proves the request succeeded; only the mandatory 2 ms
       recovery interval remains before the first request to the new address. */
    xhci_wait_until_ms(timer_uptime_ms() + USB_SET_ADDRESS_RECOVERY_MS);
    xhci_diag_timeline_at("set-address-recovery-complete",
                          (uintptr_t)portsc, portsc ? *portsc : 0);
    xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_EP0_READY,
                          "ep0-ring-ready");

    /* Phase 5: Read Descriptors & Evaluate Context */
    u8 descriptor_max_pkt;
    err = xhci_read_ep0_max_packet_size(xhc, slot_id, &descriptor_max_pkt);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        return xhci_device_setup_finish(xhc, dev, err, owns_lock);
    }
    u16 real_max_pkt = descriptor_max_pkt;
    if (speed == 4) {
        if (descriptor_max_pkt >= 16) {
            return xhci_device_setup_finish(xhc, dev,
                                            XHCI_ERR_INVALID_PARAM,
                                            owns_lock);
        }
        real_max_pkt = (u16)(1U << descriptor_max_pkt);
    }
    g_xhci_diag.ep0_mps = real_max_pkt;
    XHCI_DEBUG_LOG("[xHCI-DIAG] p%u s%u sp%u full-device-descriptor not-issued\n",
                   port_id, slot_id, speed);

    if (real_max_pkt != initial_max_pkt) {
        xhci_diag_set_phase("evaluate-context");
        ctrl_ctx->add_flags = XHCI_CTX_FLAG_EP(1);
        ctrl_ctx->drop_flags = 0;
        ep0_ctx->info2 = XHCI_EP_CTX_TYPE_SET(XHCI_EP_TYPE_CONTROL) | 
                         XHCI_EP_CTX_MAX_PACKET_SET(real_max_pkt) | 
                         XHCI_EP_CTX_CERR_SET(3);
        
        err = xhci_cmd_evaluate_context(xhc, slot_id, dev->in_ctx_phys);
        if (err != XHCI_SUCCESS) {
            xhci_diag_failure(err);
            return xhci_device_setup_finish(xhc, dev, err, owns_lock);
        }
    }
    xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_DESCRIPTORS_READY,
                          "descriptor-and-ep0-size-ready");

    /* Phase 6: Configure every endpoint needed by the device. */
    xhci_diag_set_phase("interface-discovery");
    u8 hid_ep = 0, hid_interval = 0, hid_cfg = 0, hid_iface = 0;
    u16 hid_pkt = 0;
    u8 bulk_in_ep = 0, bulk_out_ep = 0, storage_cfg = 0;
    u16 bulk_in_pkt = 0, bulk_out_pkt = 0;
    xhci_hub_endpoint_info_t hub_endpoint;
    __builtin_memset(&hub_endpoint, 0, sizeof(hub_endpoint));
    bool has_hid = xhci_get_keyboard_endpoint_info(xhc, slot_id, &hid_ep, &hid_pkt,
                                                   &hid_interval, &hid_cfg, &hid_iface) == XHCI_SUCCESS;
    bool has_storage = xhci_get_mass_storage_endpoint_info(xhc, slot_id, &bulk_in_ep,
                                                           &bulk_in_pkt, &bulk_out_ep,
                                                           &bulk_out_pkt, &storage_cfg,
                                                           false) == XHCI_SUCCESS;
    bool has_hub = !has_hid && !has_storage &&
        xhci_get_hub_endpoint_info(xhc, slot_id, speed, &hub_endpoint) ==
            XHCI_SUCCESS;
    dev->class_flags = (has_hid ? XHCI_DEVICE_CLASS_HID : 0) |
                       (has_storage ? XHCI_DEVICE_CLASS_STORAGE : 0) |
                       (has_hub ? XHCI_DEVICE_CLASS_HUB : 0);
    dev->class_ready = !(has_hid || has_storage || has_hub);
    XHCI_DEBUG_LOG("[xHCI-DIAG] p%u s%u sp%u interface-discovery hid=%u storage=%u hub=%u\n",
                   port_id, slot_id, speed, has_hid, has_storage, has_hub);
    u8 max_dci = 1;
    u32 endpoint_flags = XHCI_CTX_FLAG_SLOT;

    if (has_hid) {
        u8 ep_num = hid_ep & 0x0F;
        u8 dci = (ep_num * 2) + ((hid_ep & 0x80) ? 1 : 0);
        u8 interval = xhci_interrupt_interval(speed, hid_interval);
        u32 max_esit = hid_pkt;
        xhci_ep_context_t *ep_ctx;
        if (dci > max_dci) max_dci = dci;
        endpoint_flags |= XHCI_CTX_FLAG_EP(dci);
        err = xhci_ring_alloc(&dev->ep_rings[dci],
                              XHCI_RING_TRBS_PER_PAGE, false);
        if (err != XHCI_SUCCESS)
            return xhci_device_setup_finish(xhc, dev, err, owns_lock);
        ep_ctx = (xhci_ep_context_t *)((u8*)dev->in_ctx_virt + ((dci + 1) * ctx_sz));
        ep_ctx->info1 = XHCI_EP_CTX_INTERVAL_SET(interval);
        ep_ctx->info2 = XHCI_EP_CTX_TYPE_SET(XHCI_EP_TYPE_INTR_IN) |
                        XHCI_EP_CTX_MAX_PACKET_SET(hid_pkt) | XHCI_EP_CTX_CERR_SET(3);
        ep_ctx->tr_dq_lo = (u32)dev->ep_rings[dci].phys_base | XHCI_EP_CTX_TR_DQ_DCS;
        ep_ctx->tr_dq_hi = (u32)(dev->ep_rings[dci].phys_base >> 32);
        ep_ctx->info3 = XHCI_EP_CTX_AVG_TRB_LEN_SET(max_esit) |
                        XHCI_EP_CTX_MAX_ESIT_LO_SET(max_esit);
        for (u8 buffer_index = 0;
             buffer_index < XHCI_TRANSFER_RECORD_SLOTS; buffer_index++) {
            dev->ep_buffers_virt[dci][buffer_index] =
                (u8 *)xhci_dma_alloc(8,
                    &dev->ep_buffers_phys[dci][buffer_index]);
            if (!dev->ep_buffers_virt[dci][buffer_index])
                return xhci_device_setup_finish(xhc, dev,
                                                XHCI_ERR_NO_MEMORY,
                                                owns_lock);
        }
    }
    if (has_storage) {
        u8 endpoints[2] = { bulk_out_ep, bulk_in_ep };
        u16 packets[2] = { bulk_out_pkt, bulk_in_pkt };
        u8 types[2] = { XHCI_EP_TYPE_BULK_OUT, XHCI_EP_TYPE_BULK_IN };
        for (u32 i = 0; i < 2; i++) {
            u8 dci = ((endpoints[i] & 0x0F) * 2) + ((endpoints[i] & 0x80) ? 1 : 0);
            xhci_ep_context_t *ep_ctx;
            if (dci > max_dci) max_dci = dci;
            endpoint_flags |= XHCI_CTX_FLAG_EP(dci);
            err = xhci_ring_alloc(&dev->ep_rings[dci],
                                  XHCI_RING_TRBS_PER_PAGE, false);
            if (err != XHCI_SUCCESS)
                return xhci_device_setup_finish(xhc, dev, err, owns_lock);
            ep_ctx = (xhci_ep_context_t *)((u8*)dev->in_ctx_virt + ((dci + 1) * ctx_sz));
            ep_ctx->info1 = 0;
            ep_ctx->info2 = XHCI_EP_CTX_TYPE_SET(types[i]) |
                            XHCI_EP_CTX_MAX_PACKET_SET(packets[i]) | XHCI_EP_CTX_CERR_SET(3);
            ep_ctx->tr_dq_lo = (u32)dev->ep_rings[dci].phys_base | XHCI_EP_CTX_TR_DQ_DCS;
            ep_ctx->tr_dq_hi = (u32)(dev->ep_rings[dci].phys_base >> 32);
        }
    }

    if (has_hub) {
        u8 endpoint = hub_endpoint.endpoint_address;
        u8 dci = ((endpoint & 0x0f) * 2) +
                 ((endpoint & 0x80) ? 1 : 0);
        u32 max_esit = hub_endpoint.bytes_per_interval;
        if (!max_esit) {
            max_esit = (u32)hub_endpoint.max_packet_size *
                       ((u32)hub_endpoint.max_burst + 1U) *
                       ((u32)hub_endpoint.mult + 1U);
        }
        u8 interval = hub_endpoint.interval;
        if (speed == XHCI_SPEED_SUPER && interval)
            interval--;

        if (dci > max_dci) max_dci = dci;
        endpoint_flags |= XHCI_CTX_FLAG_EP(dci);
        err = xhci_ring_alloc(&dev->ep_rings[dci],
                              XHCI_RING_TRBS_PER_PAGE, false);
        if (err != XHCI_SUCCESS)
            return xhci_device_setup_finish(xhc, dev, err, owns_lock);

        xhci_ep_context_t *ep_ctx = (xhci_ep_context_t *)
            ((u8 *)dev->in_ctx_virt + ((dci + 1) * ctx_sz));
        ep_ctx->info1 = XHCI_EP_CTX_INTERVAL_SET(interval) |
                        XHCI_EP_CTX_MULT_SET(hub_endpoint.mult) |
                        XHCI_EP_CTX_MAX_ESIT_HI_SET(max_esit >> 16);
        ep_ctx->info2 = XHCI_EP_CTX_TYPE_SET(XHCI_EP_TYPE_INTR_IN) |
                        XHCI_EP_CTX_MAX_PACKET_SET(
                            hub_endpoint.max_packet_size) |
                        XHCI_EP_CTX_MAX_BURST_SET(
                            hub_endpoint.max_burst) |
                        XHCI_EP_CTX_CERR_SET(3);
        ep_ctx->tr_dq_lo = (u32)dev->ep_rings[dci].phys_base |
                           XHCI_EP_CTX_TR_DQ_DCS;
        ep_ctx->tr_dq_hi = (u32)(dev->ep_rings[dci].phys_base >> 32);
        ep_ctx->info3 = XHCI_EP_CTX_AVG_TRB_LEN_SET(max_esit) |
                        XHCI_EP_CTX_MAX_ESIT_LO_SET(max_esit);
    }

    if (has_hid || has_storage || has_hub) {
        xhci_diag_set_phase("configure-endpoint");
        ctrl_ctx->add_flags = endpoint_flags;
        ctrl_ctx->drop_flags = 0;
        slot_ctx->info1 &= ~XHCI_SLOT_CTX_ENTRIES_SET(0x1F);
        slot_ctx->info1 |= XHCI_SLOT_CTX_ENTRIES_SET(max_dci);
        if (has_hid) {
            u8 dci = ((hid_ep & 0x0F) * 2) +
                     ((hid_ep & 0x80) ? 1 : 0);
            xhci_ep_context_t *ep_ctx = (xhci_ep_context_t *)
                ((u8 *)dev->in_ctx_virt + ((dci + 1) * ctx_sz));
            XHCI_DEBUG_LOG("[xHCI-HID] p%u s%u sp%u ep=%02x dci=%u mps=%u bi=%u xi=%u route=%05x\n",
                           port_id, slot_id, speed, hid_ep, dci, hid_pkt,
                           hid_interval,
                           xhci_interrupt_interval(speed, hid_interval),
                           route_string);
            XHCI_DEBUG_LOG("[xHCI-HID-CTX] add=%08x drop=%08x slot=%08x i1=%08x i2=%08x dq=%08x:%08x i3=%08x\n",
                           ctrl_ctx->add_flags, ctrl_ctx->drop_flags,
                           slot_ctx->info1, ep_ctx->info1, ep_ctx->info2,
                           ep_ctx->tr_dq_hi, ep_ctx->tr_dq_lo, ep_ctx->info3);
        }
        err = xhci_cmd_configure_endpoint(xhc, slot_id, dev->in_ctx_phys);
        if (err != XHCI_SUCCESS) {
            xhci_diag_failure(err);
            return xhci_device_setup_finish(xhc, dev, err, owns_lock);
        }
        xhci_set_device_state(xhc, slot_id,
                              XHCI_DEVICE_ENDPOINTS_CONFIGURED,
                              "configure-endpoint-complete");
        u8 cfg = has_hid ? hid_cfg :
                 (has_storage ? storage_cfg : hub_endpoint.config_value);
        xhci_diag_set_phase("set-configuration");
        if (xhci_control_set_configuration(xhc, slot_id, cfg) != XHCI_SUCCESS) {
            xhci_diag_failure(XHCI_ERR_TRANSACTION);
            return xhci_device_setup_finish(xhc, dev,
                                            XHCI_ERR_TRANSACTION, owns_lock);
        }
        xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_USB_CONFIGURED,
                              "set-configuration-complete");
        xhci_set_device_state(xhc, slot_id, XHCI_DEVICE_CLASS_ATTACHING,
                              "class-attach-started");
        if (has_hid) {
            u8 dci = ((hid_ep & 0x0F) * 2) + ((hid_ep & 0x80) ? 1 : 0);
            xhci_diag_set_phase("set-protocol");
            err = xhci_control_set_protocol(xhc, slot_id, hid_iface, 0);
            if (err != XHCI_SUCCESS) {
                xhci_diag_failure(err);
                return xhci_device_setup_finish(xhc, dev, err, owns_lock);
            }
            dev->hid_dci = dci;
            xhci_diag_set_phase("arm-hid-endpoint");
            for (u32 transfer_index = 0;
                 transfer_index < XHCI_TRANSFER_RECORD_SLOTS;
                 transfer_index++) {
                if (!xhci_hid_queue_read(xhc, slot_id, dci)) {
                    xhci_diag_failure(XHCI_ERR_TRANSACTION);
                    return xhci_device_setup_finish(xhc, dev,
                                                    XHCI_ERR_TRANSACTION,
                                                    owns_lock);
                }
            }
            dev->hid_armed = true;
        }
        if (has_storage) {
            xhci_diag_set_phase("mass-storage-first-operation");
            if (!xhci_storage_init_device(xhc, slot_id, bulk_in_ep,
                                          bulk_out_ep, &dev->storage_probe) ||
                !dev->storage_probe.bot_initialized ||
                !dev->storage_probe.capacity_known ||
                !dev->storage_probe.block_registered) {
                kprint("[xHCI] Mass Storage initialization failed on slot %d\n", slot_id);
                return xhci_device_setup_finish(xhc, dev,
                                                XHCI_ERR_TRANSACTION,
                                                owns_lock);
            }
        }
        if (has_hub) {
            xhci_hub_descriptor_info_t hub_descriptor;
            err = xhci_hub_read_descriptor(xhc, slot_id, speed,
                                           &hub_descriptor);
            if (err != XHCI_SUCCESS) {
                kprint("[xHCI-HUB] s%u descriptor rc=%u\n", slot_id, err);
                return xhci_device_setup_finish(xhc, dev, err, owns_lock);
            }
            err = xhci_configure_hub_slot(xhc, dev, &hub_endpoint,
                                          &hub_descriptor);
            if (err != XHCI_SUCCESS) {
                kprint("[xHCI-HUB] s%u slot-context rc=%u\n", slot_id, err);
                return xhci_device_setup_finish(xhc, dev, err, owns_lock);
            }
            err = xhci_hub_enumerate_children(
                xhc, slot_id, port_id, route_string, topology_depth, speed,
                &hub_descriptor);
            if (err != XHCI_SUCCESS)
                return xhci_device_setup_finish(xhc, dev, err, owns_lock);
        }
        dev->class_ready = true;
    }

    return xhci_device_setup_finish(xhc, dev, XHCI_SUCCESS, owns_lock);
}

xhci_status_t xhci_setup_device(xhci_controller_t *xhc, u8 port_id,
                                xhci_speed_t speed)
{
    return xhci_setup_device_topology(xhc, port_id, speed, 0, 0, 0, 0,
                                      XHCI_SPEED_UNKNOWN, false);
}

bool xhci_setup_retry_allowed(xhci_controller_t *xhc)
{
    return xhc && xhc->last_setup_retry_safe;
}

xhci_status_t xhci_setup_child_device_locked(
    xhci_controller_t *xhc, u8 root_port, xhci_speed_t speed,
    u32 route_string, u8 topology_depth, u8 parent_hub_slot,
    u8 parent_port, xhci_speed_t parent_speed)
{
    if (!route_string || !parent_hub_slot || !parent_port)
        return XHCI_ERR_INVALID_PARAM;
    return xhci_setup_device_topology(
        xhc, root_port, speed, route_string, topology_depth,
        parent_hub_slot, parent_port, parent_speed, true);
}

xhci_status_t xhci_register_keyboard_callback(xhci_controller_t *xhc, xhci_hid_keyboard_callback_t callback) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;
    xhc->keyboard_callback = callback;
    return XHCI_SUCCESS;
}

void xhci_resume_keyboard(xhci_controller_t *xhc)
{
    u32 armed = 0;
    if (!xhc) return;

    for (u32 slot = 1; slot <= xhc->max_slots && slot < 256; slot++) {
        xhci_device_t *dev = &xhc->devices[slot];
        if (dev->slot_id != slot || !dev->setup_finished ||
            dev->setup_result != XHCI_SUCCESS ||
            !(dev->class_flags & XHCI_DEVICE_CLASS_HID) || !dev->hid_dci ||
            dev->hid_armed)
            continue;
        if (xhci_hid_queue_read(xhc, (u8)slot, dev->hid_dci)) armed++;
    }
    XHCI_DEBUG_LOG("[HID-RT] armed=%u\n", armed);
}

static const char *xhci_device_class_name(u8 class_flags)
{
    if (class_flags == XHCI_DEVICE_CLASS_HID) return "hid";
    if (class_flags == XHCI_DEVICE_CLASS_STORAGE) return "storage";
    if (class_flags == XHCI_DEVICE_CLASS_HUB) return "hub";
    if (class_flags) return "multi";
    return "other";
}

static const char *xhci_storage_stage_name(xhci_storage_stage_t stage)
{
    switch (stage) {
        case XHCI_STORAGE_STAGE_DMA:            return "dma";
        case XHCI_STORAGE_STAGE_INQUIRY:        return "inquiry";
        case XHCI_STORAGE_STAGE_CAPACITY:       return "capacity";
        case XHCI_STORAGE_STAGE_GEOMETRY:       return "geometry";
        case XHCI_STORAGE_STAGE_BLOCK_REGISTER:return "register";
        case XHCI_STORAGE_STAGE_READY:          return "ready";
        default:                                return "-";
    }
}

void xhci_print_boot_summary(xhci_controller_t *xhc, bool mgfs_mounted)
{
    if (!xhc) return;

    XHCI_DEBUG_LOG("[USB-SUM] usbblk=%u mgfs=%u\n",
                   xhci_storage_device_count(), mgfs_mounted);
    for (u32 slot = 1; slot <= xhc->max_slots && slot < 256; slot++) {
        xhci_device_t *dev = &xhc->devices[slot];
        if (dev->slot_id != slot) continue;

        XHCI_DEBUG_LOG("[USB-DEV] s%u r=%05x sp=%u cls=%s rc=%u msc=%s blk=%u gpt=%u/%u\n",
                       slot, dev->route_string, dev->speed,
                       xhci_device_class_name(dev->class_flags),
                       dev->setup_finished ? dev->setup_result :
                           XHCI_ERR_DEVICE_TIMEOUT,
                       xhci_storage_stage_name(dev->storage_probe.stage),
                       dev->storage_probe.block_registered,
                       dev->storage_probe.gpt_scan_ran,
                       dev->storage_probe.gpt_found);
    }
}

u16 xhci_get_version(xhci_controller_t *xhc) {
    if (!xhc || !xhc->cap_regs) return 0;
    return xhc->cap_regs->hciversion;
}

bool xhci_is_running(xhci_controller_t *xhc) {
    if (!xhc || !xhc->op_regs) return false;
    return (xhc->op_regs->usbsts & XHCI_USBSTS_HCH) == 0;
}

bool xhci_is_busy(xhci_controller_t *xhc) {
    if (!xhc) return false;
    return xhc->in_critical_section;
}
