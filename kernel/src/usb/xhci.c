#include <xhci.h>
#include <xhci_regs.h>
#include <xhci_trb.h>
#include <xhci_context.h>
#include <xhci_ring.h>
#include <xhci_storage.h>
#include <xhci_hub.h>
#include <scheduler.h>
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
extern void timer_sleep(u64 ms);
extern u64 timer_uptime_ms(void);
extern volatile u32 *xhci_get_portsc_ptr(xhci_controller_t *xhc, u8 port_idx);
extern void kprint(const char *fmt, ...);
extern void xhci_process_deferred_port_change(xhci_controller_t *xhc,
                                              u8 port_id);

#define XHCI_DEVICE_CLASS_HID       (1U << 0)
#define XHCI_DEVICE_CLASS_STORAGE   (1U << 1)
#define XHCI_DEVICE_CLASS_HUB       (1U << 2)

/* ==============================================================================
 * Internal State Structures
 * ============================================================================== */

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
    
    u8 *ep_buffers_virt[32];
    uintptr_t ep_buffers_phys[32];

    u8 class_flags;
    u8 hid_dci;
    bool setup_finished;
    xhci_status_t setup_result;
    xhci_storage_probe_result_t storage_probe;
};

struct xhci_controller {
    uintptr_t mmio_base;
    u8 irq_number;
    bool is_running;
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

    bool in_critical_section;
    volatile u32 pending_port_changes[8];
    volatile bool boot_enumeration_active;
    volatile bool deferred_worker_stop;
    kernel_thread_t *deferred_worker;
};

static xhci_controller_t g_xhc_instance;
static bool g_xhc_init_started;

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

void xhci_queue_port_change(xhci_controller_t *xhc, u8 port_id)
{
    u32 word;
    u32 bit;

    if (!xhc || port_id == 0)
        return;
    word = port_id >> 5;
    bit = 1U << (port_id & 31U);
    if (word >= 8)
        return;
    __atomic_fetch_or(&xhc->pending_port_changes[word], bit,
                      __ATOMIC_RELEASE);
    if (!__atomic_load_n(&xhc->boot_enumeration_active, __ATOMIC_ACQUIRE))
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
                return true;
            }
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
        if (!__atomic_load_n(&xhc->boot_enumeration_active,
                             __ATOMIC_ACQUIRE) &&
            xhci_take_port_change(xhc, &port_id)) {
            xhci_process_deferred_port_change(xhc, port_id);
            continue;
        }
        /* Sleep until the IRQ path records a port event and wakes us. */
        (void)scheduler_block();
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
static bool g_hid_runtime_active;
static u8 g_hid_irq_log_count;
static bool g_hid_empty_irq_logged;

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
    return xhc->devices[slot_id].ep_buffers_virt[dci];
}

uintptr_t xhci_get_ep_dma_phys(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    if (!xhc || slot_id == 0 || dci >= 32) return 0;
    return xhc->devices[slot_id].ep_buffers_phys[dci];
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
    if (!xhc || slot_id == 0 || !xhc->devices[slot_id].out_ctx_virt)
        return 0;
    u32 ctx_sz = xhc->csz ? 64 : 32;
    xhci_ep_context_t *ep0 = (xhci_ep_context_t *)
        ((u8 *)xhc->devices[slot_id].out_ctx_virt + (2 * ctx_sz));
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

    xhc->mmio_base = mmio_base;
    xhc->irq_number = irq_number;
    xhc->is_running = false;
    xhc->keyboard_callback = NULL;
    xhc->boot_enumeration_active = true;
    xhc->deferred_worker_stop = false;
    xhc->deferred_worker = NULL;
    for (u32 i = 0; i < 8; i++)
        xhc->pending_port_changes[i] = 0;
    g_hid_runtime_active = false;
    g_hid_irq_log_count = 0;
    g_hid_empty_irq_logged = false;

    /* 1. Map Registers */
    xhc->cap_regs = (xhci_cap_regs_t *)mmio_base;
    xhc->op_regs = (xhci_op_regs_t *)(mmio_base + xhc->cap_regs->caplength);
    xhc->db_array = (volatile u32 *)(mmio_base + xhc->cap_regs->dboff);
    xhc->run_regs = (xhci_run_regs_t *)(mmio_base + xhc->cap_regs->rtsoff);

    xhci_legacy_handoff(xhc);

    XHCI_DEBUG_LOG("[xHCI-INIT] reset-start sts=%08x cmd=%08x\n",
           xhc->op_regs->usbsts, xhc->op_regs->usbcmd);

    /* 2. Wait for Controller Not Ready (CNR) to clear */
    while (xhc->op_regs->usbsts & XHCI_USBSTS_CNR) {
        timer_sleep(1);
    }

    /* 3. Halt Controller */
    xhc->op_regs->usbcmd &= ~XHCI_USBCMD_RS;
    while (!(xhc->op_regs->usbsts & XHCI_USBSTS_HCH)) {
        timer_sleep(1);
    }

    /* 4. Host Controller Reset (HCRST) */
    xhc->op_regs->usbcmd |= XHCI_USBCMD_HCRST;
    while (xhc->op_regs->usbcmd & XHCI_USBCMD_HCRST) {
        timer_sleep(1);
    }
    while (xhc->op_regs->usbsts & XHCI_USBSTS_CNR) {
        timer_sleep(1);
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
    if (!xhc->dcbaa)
        return NULL;
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
    /* Reserve the worker's stack before device enumeration consumes the
       early heap.  Keep it suspended until synchronous boot probing is done. */
    xhc->deferred_worker = thread_create_suspended("xhci-service",
                                                   xhci_deferred_worker_entry,
                                                   xhc);
    XHCI_DEBUG_LOG("[xHCI-INIT] worker-reserved=%u\n",
                   xhc->deferred_worker != NULL);
    if (!xhc->deferred_worker)
        return NULL;
    return xhc;
}

xhci_status_t xhci_start(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    XHCI_DEBUG_LOG("[xHCI-INIT] start-enter sts=%08x cmd=%08x\n",
           xhc->op_regs->usbsts, xhc->op_regs->usbcmd);

    /* Enable global interrupts and set Run/Stop */
    xhc->op_regs->usbcmd |= (XHCI_USBCMD_INTE | XHCI_USBCMD_RS);
    
    while (xhc->op_regs->usbsts & XHCI_USBSTS_HCH) {
        timer_sleep(1);
    }
    
    xhc->is_running = true;
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

    /* Initial enumeration is synchronous.  Consume any initial port events
       while the PCI vector is still masked, without recursively enumerating
       a device from interrupt/event context. */
    bool was_busy = __atomic_test_and_set(&xhc->in_critical_section,
                                          __ATOMIC_ACQUIRE);
    xhci_process_events(xhc);

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
    xhci_process_events(xhc);

    xhci_trb_t *head = xhci_event_ring_get_next(&xhc->event_ring);
    u32 event_type = head ? XHCI_TRB_CTRL_TYPE_GET(head->control) : 0;
    u32 status = xhc->op_regs->usbsts;
    u32 iman = ir0->iman;
    u64 erdp = ir0->erdp;
    XHCI_DEBUG_LOG("[xHCI-MSI] pre-unmask sts=%08x iman=%08x erdp=%p ev=%u\n",
           status, iman, (void *)(uintptr_t)erdp, event_type);

    u32 status_ack = status & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD);
    if (status_ack)
        xhc->op_regs->usbsts = status_ack;
    if (iman & XHCI_IMAN_IP)
        ir0->iman = (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP;

    uintptr_t dequeue = xhc->event_ring.phys_base +
        xhc->event_ring.dequeue_idx * sizeof(xhci_trb_t);
    ir0->erdp = dequeue | XHCI_ERDP_EHB;
    (void)ir0->erdp;
    (void)xhc->op_regs->usbsts;

    if (!was_busy)
        __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);

    XHCI_DEBUG_LOG("[xHCI-MSI] acked sts=%08x iman=%08x erdp=%p\n",
           xhc->op_regs->usbsts, ir0->iman,
           (void *)(uintptr_t)ir0->erdp);
}

void xhci_complete_boot_enumeration(xhci_controller_t *xhc)
{
    if (!xhc)
        return;
    for (u32 i = 0; i < 8; i++)
        __atomic_store_n(&xhc->pending_port_changes[i], 0,
                         __ATOMIC_RELEASE);
    __atomic_store_n(&xhc->boot_enumeration_active, false,
                     __ATOMIC_RELEASE);
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

xhci_status_t xhci_stop(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    xhc->op_regs->usbcmd &= ~XHCI_USBCMD_RS;
    while (!(xhc->op_regs->usbsts & XHCI_USBSTS_HCH)) {
        timer_sleep(1);
    }

    xhc->is_running = false;
    return XHCI_SUCCESS;
}

xhci_status_t xhci_reset(xhci_controller_t *xhc) {
    xhci_stop(xhc);
    xhc->op_regs->usbcmd |= XHCI_USBCMD_HCRST;
    while (xhc->op_regs->usbcmd & XHCI_USBCMD_HCRST) {
        timer_sleep(1);
    }
    return XHCI_SUCCESS;
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
        if (__atomic_load_n(&g_hid_runtime_active, __ATOMIC_ACQUIRE) &&
            g_hid_irq_log_count < 4) {
            xhci_trb_t *head = xhci_event_ring_get_next(&xhc->event_ring);
            u8 type = head ? (u8)XHCI_TRB_CTRL_TYPE_GET(head->control) : 0;
            u8 slot = head ? (u8)XHCI_TRB_CTRL_SLOT_ID_GET(head->control) : 0;
            u8 dci = head ? (u8)XHCI_TRB_CTRL_EP_ID_GET(head->control) : 0;
            bool hid_head = type == XHCI_TRB_TYPE_TRANSFER_EVENT &&
                            xhci_is_hid_endpoint(xhc, slot, dci);
            if (hid_head || !g_hid_empty_irq_logged) {
                XHCI_DEBUG_LOG("[HID-IRQ] st=%08x im=%08x ev=%u s=%u d=%u\n",
                       status, ir0 ? ir0->iman : 0, type, slot, dci);
                if (hid_head)
                    g_hid_irq_log_count++;
                else
                    g_hid_empty_irq_logged = true;
            }
        }

        /* Clear the Event Interrupt flag (RW1C) */
        xhc->op_regs->usbsts = status | XHCI_USBSTS_EINT;

        if (ir0) {
            /* Clear Interrupter Pending flag */
            ir0->iman |= XHCI_IMAN_IP;
        }

        /* Dispatch to the async event processor */
        xhci_process_events(xhc);
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

static xhci_status_t xhci_device_setup_finish(xhci_controller_t *xhc,
                                              xhci_device_t *dev,
                                              xhci_status_t result,
                                              bool owns_lock)
{
    if (dev) {
        dev->setup_result = result;
        dev->setup_finished = true;
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
    bool owns_lock = !lock_held;

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
    dev->setup_finished = false;
    dev->setup_result = XHCI_ERR_DEVICE_TIMEOUT;
    __builtin_memset(&dev->storage_probe, 0, sizeof(dev->storage_probe));
    xhci_diag_set_context(port_id, slot_id, speed);
    xhci_diag_set_phase("input-context");
    XHCI_DEBUG_LOG("[xHCI-DIAG] p%u s%u sp%u input-context\n",
                   port_id, slot_id, speed);

    /* Allocate Output & Input Contexts */
    dev->out_ctx_virt = xhci_alloc_device_context(xhc->csz, &dev->out_ctx_phys);
    dev->in_ctx_virt = xhci_alloc_input_context(xhc->csz, &dev->in_ctx_phys);
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
    xhci_ring_alloc(&dev->ep_rings[1], XHCI_RING_TRBS_PER_PAGE, false);
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
    volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port_id);
    xhci_diag_timeline_at("pre-address", (uintptr_t)portsc, portsc ? *portsc : 0);
    xhci_diag_address_context(xhc, slot_id, dev->in_ctx_phys, ctrl_ctx,
                              slot_ctx, ep0_ctx);
    err = xhci_cmd_address_device(xhc, slot_id, dev->in_ctx_phys, false);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        return xhci_device_setup_finish(xhc, dev, err, owns_lock);
    }

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
        xhci_ring_alloc(&dev->ep_rings[dci], XHCI_RING_TRBS_PER_PAGE, false);
        ep_ctx = (xhci_ep_context_t *)((u8*)dev->in_ctx_virt + ((dci + 1) * ctx_sz));
        ep_ctx->info1 = XHCI_EP_CTX_INTERVAL_SET(interval);
        ep_ctx->info2 = XHCI_EP_CTX_TYPE_SET(XHCI_EP_TYPE_INTR_IN) |
                        XHCI_EP_CTX_MAX_PACKET_SET(hid_pkt) | XHCI_EP_CTX_CERR_SET(3);
        ep_ctx->tr_dq_lo = (u32)dev->ep_rings[dci].phys_base | XHCI_EP_CTX_TR_DQ_DCS;
        ep_ctx->tr_dq_hi = (u32)(dev->ep_rings[dci].phys_base >> 32);
        ep_ctx->info3 = XHCI_EP_CTX_AVG_TRB_LEN_SET(max_esit) |
                        XHCI_EP_CTX_MAX_ESIT_LO_SET(max_esit);
        dev->ep_buffers_virt[dci] = (u8*)xhci_dma_alloc(8, &dev->ep_buffers_phys[dci]);
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
            xhci_ring_alloc(&dev->ep_rings[dci], XHCI_RING_TRBS_PER_PAGE, false);
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
        u8 cfg = has_hid ? hid_cfg :
                 (has_storage ? storage_cfg : hub_endpoint.config_value);
        xhci_diag_set_phase("set-configuration");
        if (xhci_control_set_configuration(xhc, slot_id, cfg) != XHCI_SUCCESS) {
            xhci_diag_failure(XHCI_ERR_TRANSACTION);
            return xhci_device_setup_finish(xhc, dev,
                                            XHCI_ERR_TRANSACTION, owns_lock);
        }
        if (has_hid) {
            u8 dci = ((hid_ep & 0x0F) * 2) + ((hid_ep & 0x80) ? 1 : 0);
            xhci_diag_set_phase("set-protocol");
            err = xhci_control_set_protocol(xhc, slot_id, hid_iface, 0);
            if (err != XHCI_SUCCESS)
                xhci_diag_failure(err);
            dev->hid_dci = dci;
        }
        if (has_storage) {
            xhci_diag_set_phase("mass-storage-first-operation");
            if (!xhci_storage_init_device(xhc, slot_id, bulk_in_ep,
                                          bulk_out_ep, &dev->storage_probe))
                kprint("[xHCI] Mass Storage initialization failed on slot %d\n", slot_id);
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
    }

    return xhci_device_setup_finish(xhc, dev, XHCI_SUCCESS, owns_lock);
}

xhci_status_t xhci_setup_device(xhci_controller_t *xhc, u8 port_id,
                                xhci_speed_t speed)
{
    return xhci_setup_device_topology(xhc, port_id, speed, 0, 0, 0, 0,
                                      XHCI_SPEED_UNKNOWN, false);
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

    __atomic_store_n(&g_hid_runtime_active, true, __ATOMIC_RELEASE);

    for (u32 slot = 1; slot <= xhc->max_slots && slot < 256; slot++) {
        xhci_device_t *dev = &xhc->devices[slot];
        if (dev->slot_id != slot || !dev->setup_finished ||
            dev->setup_result != XHCI_SUCCESS ||
            !(dev->class_flags & XHCI_DEVICE_CLASS_HID) || !dev->hid_dci)
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
