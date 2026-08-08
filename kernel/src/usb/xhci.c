#include <xhci.h>
#include <xhci_regs.h>
#include <xhci_trb.h>
#include <xhci_context.h>
#include <xhci_ring.h>
#include <xhci_storage.h>
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
extern xhci_status_t xhci_get_mass_storage_endpoint_info(xhci_controller_t *xhc, u8 slot_id, u8 *out_bulk_in, u16 *out_bulk_in_pkt, u8 *out_bulk_out, u16 *out_bulk_out_pkt, u8 *out_config_val);

// xhci_control.c
extern xhci_status_t xhci_control_set_configuration(xhci_controller_t *xhc, u8 slot_id, u8 config_value);
extern xhci_status_t xhci_control_set_protocol(xhci_controller_t *xhc, u8 slot_id, u8 interface_index, u8 protocol);

// xhci_hid.c
extern void xhci_hid_queue_read(xhci_controller_t *xhc, u8 slot_id, u8 dci);

// Mangrove OS Handlers
extern void timer_sleep(u64 ms);
extern u64 timer_uptime_ms(void);
extern volatile u32 *xhci_get_portsc_ptr(xhci_controller_t *xhc, u8 port_idx);
extern void kprint(const char *fmt, ...);


/* ==============================================================================
 * Internal State Structures
 * ============================================================================== */

struct xhci_device {
    u8 slot_id;
    u8 port_id;
    xhci_speed_t speed;
    
    void *out_ctx_virt;
    uintptr_t out_ctx_phys;
    
    void *in_ctx_virt;
    uintptr_t in_ctx_phys;
    
    xhci_ring_t ep_rings[32]; // Rings for Endpoints (DCI 1 to 31)
    
    u8 *ep_buffers_virt[32];
    uintptr_t ep_buffers_phys[32];
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
    u8 hid_slot_id;
    u8 hid_dci;
};

static xhci_controller_t g_xhc_instance;

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
    kprint("[xHCI-TIME] %llums +%llums p%u s%u st=%s a=%p ps=%08x ccs=%u ped=%u pr=%u prc=%u pls=%u sp=%u pp=%u ch=%08x\n",
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

void xhci_diag_address_dw1(xhci_controller_t *xhc, u8 slot, const char *stage)
{
    if (!xhc || slot == 0 || !stage || !xhc->devices[slot].in_ctx_virt) return;
    u32 ctx_sz = xhc->csz ? 64 : 32;
    volatile u32 *slot_dw1 = (volatile u32 *)
        ((u8 *)xhc->devices[slot].in_ctx_virt + ctx_sz + 4);
    kprint("[xHCI-ADDR-STEP] %s dw1=%08x\n", stage, *slot_dw1);
}

void xhci_diag_address_context(xhci_controller_t *xhc, u8 slot,
                               uintptr_t input_ctx_phys,
                               const xhci_input_control_context_t *ctrl,
                               const xhci_slot_context_t *slot_ctx,
                               const xhci_ep_context_t *ep0_ctx)
{
    if (!xhc || !ctrl || !slot_ctx || !ep0_ctx) return;
    kprint("[xHCI-ADDR] p%u s%u sp%u csz=%u hcc=%08x ip=%p dcba=%p d[%u]=%016llx\n",
           g_xhci_diag.port, slot, g_xhci_diag.speed, xhc->csz,
           xhc->cap_regs->hccparams1, (void *)input_ctx_phys,
           (void *)xhc->dcbaa_phys, slot, (unsigned long long)xhc->dcbaa[slot]);
    kprint("[xHCI-ADDR] ic=%08x/%08x slot=%08x,%08x,%08x,%08x ep0=%08x,%08x dq=%08x:%08x i3=%08x\n",
           ctrl->drop_flags, ctrl->add_flags,
           slot_ctx->info1, slot_ctx->info2, slot_ctx->info3, slot_ctx->info4,
           ep0_ctx->info1, ep0_ctx->info2, ep0_ctx->tr_dq_hi,
           ep0_ctx->tr_dq_lo, ep0_ctx->info3);
    kprint("[xHCI-ADDR-RAW] 00=%08x 40=%08x/%08x 80=%08x/%08x/%08x/%08x pad20=%08x\n",
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
    kprint("[xHCI-DIAG] p%u s%u sp%u %s ep0 trb%u len0 cc%u res%u ev%p rc%u\n",
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
    kprint("[xHCI-DIAG] p%u s%u sp%u %s ep1 ctrl len%u cc%u res%u ev%p rc%u\n",
           g_xhci_diag.port, slot, g_xhci_diag.speed,
           g_xhci_diag.phase ? g_xhci_diag.phase : "control", length,
           cc, residual, (void *)ptr, result);
    kprint("[xHCI-EP0] req=%02x/%02x v=%04x i=%04x l=%u trt=%u data=%s status=%s flags=%02x/%02x mps=%u ring=%u/%u>%u/%u halt=%u\n",
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

    xhc->mmio_base = mmio_base;
    xhc->irq_number = irq_number;
    xhc->is_running = false;
    xhc->keyboard_callback = NULL;

    /* 1. Map Registers */
    xhc->cap_regs = (xhci_cap_regs_t *)mmio_base;
    xhc->op_regs = (xhci_op_regs_t *)(mmio_base + xhc->cap_regs->caplength);
    xhc->db_array = (volatile u32 *)(mmio_base + xhc->cap_regs->dboff);
    xhc->run_regs = (xhci_run_regs_t *)(mmio_base + xhc->cap_regs->rtsoff);

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
    
     /* 5. Parse Capabilities */
    xhc->max_slots = XHCI_HCSPARAMS1_MAX_SLOTS(xhc->cap_regs->hcsparams1);
    xhc->max_ports = XHCI_HCSPARAMS1_MAX_PORTS(xhc->cap_regs->hcsparams1);
    xhc->max_intrs = XHCI_HCSPARAMS1_MAX_INTRS(xhc->cap_regs->hcsparams1);
    u32 max_scratchpads = XHCI_HCSPARAMS2_MAX_SCRATCH(xhc->cap_regs->hcsparams2);
    xhc->csz = (xhc->cap_regs->hccparams1 & XHCI_HCCPARAMS1_CSZ) ? 1 : 0;
    xhc->page_size = 4096;


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
    return xhc;
}

xhci_status_t xhci_start(xhci_controller_t *xhc) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    /* Enable global interrupts and set Run/Stop */
    xhc->op_regs->usbcmd |= (XHCI_USBCMD_INTE | XHCI_USBCMD_RS);
    
    while (xhc->op_regs->usbsts & XHCI_USBSTS_HCH) {
        timer_sleep(1);
    }
    
    xhc->is_running = true;
    return XHCI_SUCCESS;
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
    xhci_stop(xhc);
    xhci_ring_free(&xhc->cmd_ring);
    xhci_ring_free(&xhc->event_ring);
    xhc->is_running = false;
}


/* ==============================================================================
 * Hardware Interrupt Entry
 * ============================================================================== */

void xhci_interrupt_handler(xhci_controller_t *xhc) {
    if (!xhc || !xhc->op_regs) return;

    u32 status = xhc->op_regs->usbsts;

    if (status & XHCI_USBSTS_EINT) {
        /* Clear the Event Interrupt flag (RW1C) */
        xhc->op_regs->usbsts = status | XHCI_USBSTS_EINT;
        
        xhci_intr_regs_t *ir0 = xhci_get_intr_regs(xhc, 0);
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

/*
 * Sweeps a physical port through Enable, Addressing, Descriptor parsing,
 * Configuration, and HID boot protocol execution.
 */
xhci_status_t xhci_setup_device(xhci_controller_t *xhc, u8 port_id, xhci_speed_t speed) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;

    xhci_diag_set_context(port_id, 0, speed);
    xhci_diag_set_phase("enable-slot");

    /* Spin until we safely acquire the critical section lock, blocking background hotplug interrupts */
    while (__atomic_test_and_set(&xhc->in_critical_section, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    u8 slot_id = 0;
    xhci_status_t err;

    /* Phase 3: Enable Slot */
    err = xhci_cmd_enable_slot(xhc, &slot_id);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
        return err;
    }

    xhci_device_t *dev = &xhc->devices[slot_id];
    dev->slot_id = slot_id;
    dev->port_id = port_id;
    dev->speed = speed;
    xhci_diag_set_context(port_id, slot_id, speed);
    xhci_diag_set_phase("input-context");
    kprint("[xHCI-DIAG] p%u s%u sp%u input-context\n", port_id, slot_id, speed);

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
    slot_ctx->info1 = XHCI_SLOT_CTX_ROUTE_STRING_SET(0) | 
                      XHCI_SLOT_CTX_SPEED_SET(speed) | 
                      XHCI_SLOT_CTX_ENTRIES_SET(1);
    slot_ctx->info2 = XHCI_SLOT_CTX_ROOT_HUB_PORT_SET(port_id) | 
                      XHCI_SLOT_CTX_MAX_EXIT_LAT_SET(0);
    slot_ctx->info3 = 0;
    slot_ctx->info4 = 0;
    kprint("[xHCI-ADDR-INIT] slot+04=%08x\n", slot_ctx->info2);

    /* 3. Allocate EP0 Transfer Ring */
    xhci_ring_alloc(&dev->ep_rings[1], XHCI_RING_TRBS_PER_PAGE, false);
    kprint("[xHCI-ADDR-CHK] after-ep0-ring dw1=%08x in=%p/%p ep0ring=%p\n",
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
    kprint("[xHCI-ADDR-CHK] after-ep0-init dw1=%08x ep0=%p\n",
           slot_ctx->info2, (void *)(dev->in_ctx_phys + (2 * ctx_sz)));

    /* Phase 4: Address Device */
    xhci_diag_set_phase("address-device");
    volatile u32 *portsc = xhci_get_portsc_ptr(xhc, port_id);
    xhci_diag_timeline_at("pre-address", (uintptr_t)portsc, portsc ? *portsc : 0);
    xhci_diag_address_context(xhc, slot_id, dev->in_ctx_phys, ctrl_ctx,
                              slot_ctx, ep0_ctx);
    err = xhci_cmd_address_device(xhc, slot_id, dev->in_ctx_phys, false);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
        return err;
    }

    /* Phase 5: Read Descriptors & Evaluate Context */
    u8 real_max_pkt;
    err = xhci_read_ep0_max_packet_size(xhc, slot_id, &real_max_pkt);
    if (err != XHCI_SUCCESS) {
        xhci_diag_failure(err);
        __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
        return err;
    }
    g_xhci_diag.ep0_mps = real_max_pkt;
    kprint("[xHCI-DIAG] p%u s%u sp%u full-device-descriptor not-issued\n",
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
            __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
            return err;
        }
    }

    /* Phase 6: Configure every endpoint needed by the device. */
    xhci_diag_set_phase("interface-discovery");
    u8 hid_ep = 0, hid_interval = 0, hid_cfg = 0, hid_iface = 0;
    u16 hid_pkt = 0;
    u8 bulk_in_ep = 0, bulk_out_ep = 0, storage_cfg = 0;
    u16 bulk_in_pkt = 0, bulk_out_pkt = 0;
    bool has_hid = xhci_get_keyboard_endpoint_info(xhc, slot_id, &hid_ep, &hid_pkt,
                                                   &hid_interval, &hid_cfg, &hid_iface) == XHCI_SUCCESS;
    bool has_storage = xhci_get_mass_storage_endpoint_info(xhc, slot_id, &bulk_in_ep,
                                                           &bulk_in_pkt, &bulk_out_ep,
                                                           &bulk_out_pkt, &storage_cfg) == XHCI_SUCCESS;
    kprint("[xHCI-DIAG] p%u s%u sp%u interface-discovery hid=%u storage=%u\n",
           port_id, slot_id, speed, has_hid, has_storage);
    u8 max_dci = 1;
    u32 endpoint_flags = XHCI_CTX_FLAG_SLOT;

    if (has_hid) {
        u8 ep_num = hid_ep & 0x0F;
        u8 dci = (ep_num * 2) + ((hid_ep & 0x80) ? 1 : 0);
        xhci_ep_context_t *ep_ctx;
        if (dci > max_dci) max_dci = dci;
        endpoint_flags |= XHCI_CTX_FLAG_EP(dci);
        xhci_ring_alloc(&dev->ep_rings[dci], XHCI_RING_TRBS_PER_PAGE, false);
        ep_ctx = (xhci_ep_context_t *)((u8*)dev->in_ctx_virt + ((dci + 1) * ctx_sz));
        ep_ctx->info1 = XHCI_EP_CTX_INTERVAL_SET(hid_interval ? hid_interval : 6);
        ep_ctx->info2 = XHCI_EP_CTX_TYPE_SET(XHCI_EP_TYPE_INTR_IN) |
                        XHCI_EP_CTX_MAX_PACKET_SET(hid_pkt) | XHCI_EP_CTX_CERR_SET(3);
        ep_ctx->tr_dq_lo = (u32)dev->ep_rings[dci].phys_base | XHCI_EP_CTX_TR_DQ_DCS;
        ep_ctx->tr_dq_hi = (u32)(dev->ep_rings[dci].phys_base >> 32);
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

    if (has_hid || has_storage) {
        xhci_diag_set_phase("configure-endpoint");
        ctrl_ctx->add_flags = endpoint_flags;
        ctrl_ctx->drop_flags = 0;
        slot_ctx->info1 &= ~XHCI_SLOT_CTX_ENTRIES_SET(0x1F);
        slot_ctx->info1 |= XHCI_SLOT_CTX_ENTRIES_SET(max_dci);
        err = xhci_cmd_configure_endpoint(xhc, slot_id, dev->in_ctx_phys);
        if (err != XHCI_SUCCESS) {
            xhci_diag_failure(err);
            __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
            return err;
        }
        u8 cfg = has_hid ? hid_cfg : storage_cfg;
        xhci_diag_set_phase("set-configuration");
        if (xhci_control_set_configuration(xhc, slot_id, cfg) != XHCI_SUCCESS) {
            xhci_diag_failure(XHCI_ERR_TRANSACTION);
            __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
            return XHCI_ERR_TRANSACTION;
        }
        if (has_hid) {
            u8 dci = ((hid_ep & 0x0F) * 2) + ((hid_ep & 0x80) ? 1 : 0);
            xhci_diag_set_phase("set-protocol");
            err = xhci_control_set_protocol(xhc, slot_id, hid_iface, 0);
            if (err != XHCI_SUCCESS)
                xhci_diag_failure(err);
            xhc->hid_slot_id = slot_id;
            xhc->hid_dci = dci;
        }
        if (has_storage) {
            xhci_diag_set_phase("mass-storage-first-operation");
            if (!xhci_storage_init_device(xhc, slot_id, bulk_in_ep, bulk_out_ep))
                kprint("[xHCI] Mass Storage initialization failed on slot %d\n", slot_id);
        }
    }

    /* Release the critical section lock before returning successfully */
    __atomic_clear(&xhc->in_critical_section, __ATOMIC_RELEASE);
    return XHCI_SUCCESS;
}

xhci_status_t xhci_register_keyboard_callback(xhci_controller_t *xhc, xhci_hid_keyboard_callback_t callback) {
    if (!xhc) return XHCI_ERR_INVALID_PARAM;
    xhc->keyboard_callback = callback;
    return XHCI_SUCCESS;
}

void xhci_resume_keyboard(xhci_controller_t *xhc)
{
    if (xhc && xhc->hid_slot_id && xhc->hid_dci)
        xhci_hid_queue_read(xhc, xhc->hid_slot_id, xhc->hid_dci);
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
