#include <xhci_hub.h>
#include <stddef.h>

extern void *xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void xhci_dma_free(void *virt, usize size);
extern void timer_sleep(u64 ms);
extern u64 timer_uptime_ms(void);
extern void kprint(const char *fmt, ...);
extern xhci_status_t xhci_control_transfer(
    xhci_controller_t *xhc, u8 slot_id, u8 bm_request_type, u8 request,
    u16 value, u16 index, u16 length, uintptr_t data_buffer_phys);

#define USB_DIR_OUT                         0x00
#define USB_DIR_IN                          0x80
#define USB_TYPE_CLASS                      0x20
#define USB_RECIP_DEVICE                    0x00
#define USB_RECIP_OTHER                     0x03

#define USB_REQ_GET_STATUS                  0
#define USB_REQ_CLEAR_FEATURE               1
#define USB_REQ_SET_FEATURE                 3
#define USB_REQ_GET_DESCRIPTOR              6
#define USB_REQ_SET_HUB_DEPTH               12

#define USB_DESC_TYPE_HUB                   0x29
#define USB_DESC_TYPE_SS_HUB                0x2a
#define USB_SS_HUB_DESCRIPTOR_SIZE          12

#define USB_PORT_FEAT_RESET                 4
#define USB_PORT_FEAT_POWER                 8
#define USB_PORT_FEAT_C_CONNECTION          16
#define USB_PORT_FEAT_C_ENABLE              17
#define USB_PORT_FEAT_C_SUSPEND             18
#define USB_PORT_FEAT_C_OVER_CURRENT        19
#define USB_PORT_FEAT_C_RESET               20
#define USB_PORT_FEAT_BH_PORT_RESET         29
#define USB_PORT_FEAT_C_BH_PORT_RESET       30
#define USB_PORT_FEAT_C_PORT_LINK_STATE     25
#define USB_PORT_FEAT_C_PORT_CONFIG_ERROR   26

#define USB_PORT_STAT_CONNECTION            0x0001
#define USB_PORT_STAT_ENABLE                0x0002
#define USB_PORT_STAT_RESET                 0x0010
#define USB_PORT_STAT_LINK_STATE            0x01e0
#define USB_SS_PORT_LS_U0                    0x0000
#define USB_PORT_STAT_LOW_SPEED             0x0200
#define USB_PORT_STAT_HIGH_SPEED            0x0400

#define USB_PORT_STAT_C_CONNECTION          0x0001
#define USB_PORT_STAT_C_ENABLE              0x0002
#define USB_PORT_STAT_C_SUSPEND             0x0004
#define USB_PORT_STAT_C_OVERCURRENT         0x0008
#define USB_PORT_STAT_C_RESET               0x0010
#define USB_PORT_STAT_C_BH_RESET            0x0020
#define USB_PORT_STAT_C_LINK_STATE          0x0040
#define USB_PORT_STAT_C_CONFIG_ERROR        0x0080

#define XHCI_HUB_PORT_DEBOUNCE_MS           100U
#define XHCI_HUB_PORT_DEBOUNCE_TIMEOUT_MS   500U
#define XHCI_HUB_PORT_RESET_TIMEOUT_MS      500U
#define XHCI_HUB_PORT_POLL_MS               10U
#define XHCI_HUB_RESET_RECOVERY_MS          10U

typedef struct __attribute__((packed)) {
    u16 status;
    u16 change;
} usb_hub_port_status_t;

#if XHCI_DEBUG
static u32 g_hub_probe_generation;
#endif

typedef enum {
    XHCI_HUB_RESET_CONNECTED,
    XHCI_HUB_RESET_DEBOUNCING,
    XHCI_HUB_RESET_REQUESTED,
    XHCI_HUB_RESET_IN_PROGRESS,
    XHCI_HUB_RESET_COMPLETION_OBSERVED,
    XHCI_HUB_RESET_ENABLED,
    XHCI_HUB_RESET_READY
} xhci_hub_reset_state_t;

static const char *xhci_hub_reset_state_name(xhci_hub_reset_state_t state)
{
    switch (state) {
        case XHCI_HUB_RESET_CONNECTED: return "CONNECTED";
        case XHCI_HUB_RESET_DEBOUNCING: return "DEBOUNCING";
        case XHCI_HUB_RESET_REQUESTED: return "RESET_REQUESTED";
        case XHCI_HUB_RESET_IN_PROGRESS: return "RESET_IN_PROGRESS";
        case XHCI_HUB_RESET_COMPLETION_OBSERVED:
            return "RESET_COMPLETION_OBSERVED";
        case XHCI_HUB_RESET_ENABLED: return "ENABLED";
        case XHCI_HUB_RESET_READY: return "READY_FOR_ADDRESS";
        default: return "UNKNOWN";
    }
}

static void xhci_hub_reset_log(u8 hub_slot_id, u8 port,
                               xhci_hub_reset_state_t state,
                               u16 status, u16 change,
                               const char *reason)
{
    XHCI_DEBUG_LOG(
        "[xHCI-HUB] s%u p%u reset state=%s st=%04x ch=%04x ccs=%u "
        "en=%u rst=%u ls=%u creset=%u cbh=%u%s%s\n",
        hub_slot_id, port, xhci_hub_reset_state_name(state), status, change,
        (status & USB_PORT_STAT_CONNECTION) != 0,
        (status & USB_PORT_STAT_ENABLE) != 0,
        (status & USB_PORT_STAT_RESET) != 0,
        (status & USB_PORT_STAT_LINK_STATE) >> 5,
        (change & USB_PORT_STAT_C_RESET) != 0,
        (change & USB_PORT_STAT_C_BH_RESET) != 0,
        reason ? " reason=" : "", reason ? reason : "");
}

static xhci_status_t hub_control(xhci_controller_t *xhc, u8 slot_id,
                                 u8 request_type, u8 request, u16 value,
                                 u16 index, u16 length, uintptr_t buffer_phys)
{
    return xhci_control_transfer(xhc, slot_id, request_type, request, value,
                                 index, length, buffer_phys);
}

xhci_status_t xhci_hub_read_descriptor(
    xhci_controller_t *xhc, u8 slot_id, xhci_speed_t speed,
    xhci_hub_descriptor_info_t *out_descriptor)
{
    uintptr_t phys = 0;
    u8 *buffer;
    u8 descriptor_type;
    u16 request_length;
    xhci_status_t result;

    if (!xhc || !slot_id || !out_descriptor)
        return XHCI_ERR_INVALID_PARAM;

    descriptor_type = speed == XHCI_SPEED_SUPER ?
        USB_DESC_TYPE_SS_HUB : USB_DESC_TYPE_HUB;
    request_length = speed == XHCI_SPEED_SUPER ?
        USB_SS_HUB_DESCRIPTOR_SIZE : 12;
    buffer = (u8 *)xhci_dma_alloc(request_length, &phys);
    if (!buffer)
        return XHCI_ERR_NO_MEMORY;

    xhci_diag_set_control_quiet(true);
    result = hub_control(xhc, slot_id,
                         USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                         USB_REQ_GET_DESCRIPTOR,
                         (u16)descriptor_type << 8, 0, request_length, phys);
    xhci_diag_set_control_quiet(false);
    if (result != XHCI_SUCCESS || buffer[0] < 7 ||
        buffer[1] != descriptor_type ||
        (speed == XHCI_SPEED_SUPER && buffer[0] < USB_SS_HUB_DESCRIPTOR_SIZE)) {
        xhci_dma_free(buffer, request_length);
        return result == XHCI_SUCCESS ? XHCI_ERR_NOT_SUPPORTED : result;
    }

    __builtin_memset(out_descriptor, 0, sizeof(*out_descriptor));
    out_descriptor->descriptor_type = buffer[1];
    out_descriptor->num_ports = buffer[2];
    out_descriptor->characteristics = (u16)buffer[3] | ((u16)buffer[4] << 8);
    out_descriptor->power_on_delay_2ms = buffer[5];
    out_descriptor->controller_current = buffer[6];
    if (speed == XHCI_SPEED_SUPER) {
        out_descriptor->header_decode_latency = buffer[7];
        out_descriptor->hub_delay = (u16)buffer[8] | ((u16)buffer[9] << 8);
    }
    xhci_dma_free(buffer, request_length);

    XHCI_DEBUG_LOG("[xHCI-HUB] s%u desc=%02x ports=%u chars=%04x pgood=%ums\n",
                   slot_id, out_descriptor->descriptor_type,
                   out_descriptor->num_ports,
                   out_descriptor->characteristics,
                   (u32)out_descriptor->power_on_delay_2ms * 2U);
    return out_descriptor->num_ports ? XHCI_SUCCESS : XHCI_ERR_NOT_SUPPORTED;
}

static xhci_status_t hub_set_depth(xhci_controller_t *xhc, u8 slot_id,
                                   u8 depth)
{
    return hub_control(xhc, slot_id,
                       USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                       USB_REQ_SET_HUB_DEPTH, depth, 0, 0, 0);
}

static xhci_status_t hub_set_port_feature(xhci_controller_t *xhc, u8 slot_id,
                                          u8 port, u16 feature)
{
    return hub_control(xhc, slot_id,
                       USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                       USB_REQ_SET_FEATURE, feature, port, 0, 0);
}

static xhci_status_t hub_clear_port_feature(xhci_controller_t *xhc, u8 slot_id,
                                            u8 port, u16 feature)
{
    return hub_control(xhc, slot_id,
                       USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                       USB_REQ_CLEAR_FEATURE, feature, port, 0, 0);
}

static xhci_status_t hub_get_port_status(xhci_controller_t *xhc, u8 slot_id,
                                         u8 port, usb_hub_port_status_t *status,
                                         uintptr_t status_phys)
{
#if XHCI_DEBUG
    bool trace_target = slot_id == 1 && port == 2;
    if (trace_target) {
        /* A failed or stale data stage must not look like a valid zeroed
           response in the trace.  The sentinel is diagnostic-only. */
        status->status = 0xA5A5;
        status->change = 0x5A5A;
        XHCI_DEBUG_LOG(
            "[xHCI-HUB] t=%llu s%u p%u GET_STATUS buffer=%p phys=%p "
            "sentinel=a5a5/5a5a\n",
            (unsigned long long)timer_uptime_ms(), slot_id, port,
            (void *)status, (void *)status_phys);
    }
#else
    const bool trace_target = false;
#endif
    xhci_status_t result = hub_control(
        xhc, slot_id, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_GET_STATUS, 0, port, sizeof(*status), status_phys);
    if (trace_target) {
        XHCI_DEBUG_LOG(
            "[xHCI-HUB] t=%llu s%u p%u GET_STATUS result rc=%u "
            "st=%04x ch=%04x sentinel=%u\n",
            (unsigned long long)timer_uptime_ms(), slot_id, port, result,
            status->status, status->change,
            status->status == 0xA5A5 && status->change == 0x5A5A);
    }
    return result;
}

static void hub_clear_port_changes(xhci_controller_t *xhc, u8 slot_id, u8 port,
                                   u16 change)
{
    static const struct {
        u16 mask;
        u16 feature;
    } changes[] = {
        { USB_PORT_STAT_C_CONNECTION,  USB_PORT_FEAT_C_CONNECTION },
        { USB_PORT_STAT_C_ENABLE,      USB_PORT_FEAT_C_ENABLE },
        { USB_PORT_STAT_C_SUSPEND,     USB_PORT_FEAT_C_SUSPEND },
        { USB_PORT_STAT_C_OVERCURRENT, USB_PORT_FEAT_C_OVER_CURRENT },
        { USB_PORT_STAT_C_RESET,       USB_PORT_FEAT_C_RESET },
        { USB_PORT_STAT_C_BH_RESET,    USB_PORT_FEAT_C_BH_PORT_RESET },
        { USB_PORT_STAT_C_LINK_STATE,  USB_PORT_FEAT_C_PORT_LINK_STATE },
        { USB_PORT_STAT_C_CONFIG_ERROR, USB_PORT_FEAT_C_PORT_CONFIG_ERROR }
    };

    if (slot_id == 1 && port == 2 && change) {
        XHCI_DEBUG_LOG(
            "[xHCI-HUB] t=%llu s%u p%u clear-change mask=%04x\n",
            (unsigned long long)timer_uptime_ms(), slot_id, port, change);
    }

    for (u32 i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        if (change & changes[i].mask)
            (void)hub_clear_port_feature(xhc, slot_id, port,
                                         changes[i].feature);
    }
}

static xhci_status_t xhci_hub_reset_port(
    xhci_controller_t *xhc, u8 hub_slot_id, u8 port,
    xhci_speed_t hub_speed, usb_hub_port_status_t *port_status,
    uintptr_t status_phys, u16 initial_status, u16 initial_change,
    u16 *out_status, u16 *out_change,
    const char **out_reason)
{
    xhci_hub_reset_state_t state = XHCI_HUB_RESET_CONNECTED;
    xhci_status_t result = XHCI_SUCCESS;
    u16 status = initial_status;
    u16 change = initial_change;
    u16 completion_change = hub_speed == XHCI_SPEED_SUPER ?
        USB_PORT_STAT_C_BH_RESET : USB_PORT_STAT_C_RESET;
    u16 reset_feature = hub_speed == XHCI_SPEED_SUPER ?
        USB_PORT_FEAT_BH_PORT_RESET : USB_PORT_FEAT_RESET;
    u64 stable_since;
    u64 debounce_deadline;
    u64 reset_deadline;
    bool completion_observed = false;
    const char *failure_reason = "reset-timeout";

    if (out_status) *out_status = status;
    if (out_change) *out_change = change;
    if (out_reason) *out_reason = "unknown";

    xhci_hub_reset_log(hub_slot_id, port, state, status, change, NULL);

    /* Clear only the pre-existing change indications.  They establish the
       baseline; reset completion must be observed after the request below. */
    if (change)
        hub_clear_port_changes(xhc, hub_slot_id, port, change);

    state = XHCI_HUB_RESET_DEBOUNCING;
    stable_since = timer_uptime_ms();
    debounce_deadline = stable_since + XHCI_HUB_PORT_DEBOUNCE_TIMEOUT_MS;
    xhci_hub_reset_log(hub_slot_id, port, state, status, change,
                       "connection-stability");
    while (timer_uptime_ms() - stable_since < XHCI_HUB_PORT_DEBOUNCE_MS) {
        if (timer_uptime_ms() >= debounce_deadline) {
            failure_reason = "connection-not-stable-before-reset";
            goto failed;
        }

        timer_sleep(XHCI_HUB_PORT_POLL_MS);
        result = hub_get_port_status(xhc, hub_slot_id, port,
                                     port_status, status_phys);
        if (result != XHCI_SUCCESS) {
            failure_reason = "status-read-failed-during-debounce";
            goto failed;
        }
        status = port_status->status;
        change = port_status->change;
        if (hub_slot_id == 1 && port == 2) {
            XHCI_DEBUG_LOG(
                "[xHCI-HUB] t=%llu s%u p%u debounce sample st=%04x "
                "ch=%04x\n",
                (unsigned long long)timer_uptime_ms(), hub_slot_id, port,
                status, change);
        }
        if (!(status & USB_PORT_STAT_CONNECTION)) {
            if (change & USB_PORT_STAT_C_CONNECTION)
                hub_clear_port_changes(xhc, hub_slot_id, port,
                                       USB_PORT_STAT_C_CONNECTION);
            stable_since = timer_uptime_ms();
            continue;
        }
        if (change & USB_PORT_STAT_C_CONNECTION) {
            hub_clear_port_changes(xhc, hub_slot_id, port,
                                   USB_PORT_STAT_C_CONNECTION);
            stable_since = timer_uptime_ms();
        }
    }

    /* Clear any status changes accumulated while debouncing, then request
       the reset appropriate to the hub's bus speed. */
    if (change)
        hub_clear_port_changes(xhc, hub_slot_id, port, change);
    state = XHCI_HUB_RESET_REQUESTED;
    if (hub_slot_id == 1 && port == 2) {
        XHCI_DEBUG_LOG(
            "[xHCI-HUB] t=%llu s%u p%u pre-reset feature=%u st=%04x "
            "ch=%04x\n",
            (unsigned long long)timer_uptime_ms(), hub_slot_id, port,
            reset_feature, status, change);
    }
    xhci_hub_reset_log(hub_slot_id, port, state, status, change,
                       hub_speed == XHCI_SPEED_SUPER ?
                           "set-bh-port-reset" : "set-port-reset");
    result = hub_set_port_feature(xhc, hub_slot_id, port, reset_feature);
    if (result != XHCI_SUCCESS) {
        failure_reason = "reset-request-failed";
        goto failed;
    }

    state = XHCI_HUB_RESET_IN_PROGRESS;
    xhci_hub_reset_log(hub_slot_id, port, state, status, change, NULL);
    reset_deadline = timer_uptime_ms() + XHCI_HUB_PORT_RESET_TIMEOUT_MS;
    while (timer_uptime_ms() < reset_deadline) {
        timer_sleep(XHCI_HUB_PORT_POLL_MS);
        result = hub_get_port_status(xhc, hub_slot_id, port,
                                     port_status, status_phys);
        if (result != XHCI_SUCCESS) {
            failure_reason = "status-read-failed-during-reset";
            goto failed;
        }
        status = port_status->status;
        change = port_status->change;

        if (!(status & USB_PORT_STAT_CONNECTION)) {
            failure_reason = "connection-lost-during-reset";
            goto failed;
        }
        if (change & USB_PORT_STAT_C_CONNECTION) {
            failure_reason = "connection-change-during-reset";
            goto failed;
        }
        if (change & completion_change) {
            if (!completion_observed) {
                completion_observed = true;
                state = XHCI_HUB_RESET_COMPLETION_OBSERVED;
                xhci_hub_reset_log(hub_slot_id, port, state, status, change,
                                   hub_speed == XHCI_SPEED_SUPER ?
                                       "C_BH_RESET" : "C_RESET");
            }
        }
        if (completion_observed && !(status & USB_PORT_STAT_RESET)) {
            if (!(status & USB_PORT_STAT_ENABLE)) {
                failure_reason = "reset-completed-port-disabled";
            } else if (hub_speed == XHCI_SPEED_SUPER &&
                       (status & USB_PORT_STAT_LINK_STATE) != USB_SS_PORT_LS_U0) {
                failure_reason = "reset-completed-link-not-u0";
            } else {
                state = XHCI_HUB_RESET_ENABLED;
                xhci_hub_reset_log(hub_slot_id, port, state, status, change,
                                   NULL);
                state = XHCI_HUB_RESET_READY;
                xhci_hub_reset_log(hub_slot_id, port, state, status, change,
                                   "ready-for-address");
                timer_sleep(XHCI_HUB_RESET_RECOVERY_MS);
                result = hub_get_port_status(xhc, hub_slot_id, port,
                                             port_status, status_phys);
                if (result != XHCI_SUCCESS) {
                    failure_reason = "status-read-failed-after-reset";
                    goto failed;
                }
                status = port_status->status;
                change = port_status->change;
                if (!(status & USB_PORT_STAT_CONNECTION)) {
                    failure_reason = "connection-lost-after-reset";
                    goto failed;
                }
                if (!(status & USB_PORT_STAT_ENABLE)) {
                    failure_reason = "port-disabled-after-reset";
                    goto failed;
                }
                if (hub_speed == XHCI_SPEED_SUPER &&
                    (status & USB_PORT_STAT_LINK_STATE) != USB_SS_PORT_LS_U0) {
                    failure_reason = "link-left-u0-after-reset";
                    goto failed;
                }
                if (change & USB_PORT_STAT_C_CONNECTION) {
                    failure_reason = "connection-change-after-reset";
                    goto failed;
                }
                if (out_status) *out_status = status;
                if (out_change) *out_change = change;
                if (out_reason) *out_reason = "ready-for-address";
                hub_clear_port_changes(xhc, hub_slot_id, port, change);
                return XHCI_SUCCESS;
            }
        }
    }

    if (!completion_observed)
        failure_reason = "reset-completion-change-missing";
    else if (status & USB_PORT_STAT_RESET)
        failure_reason = "reset-bit-still-set";
    else if (!(status & USB_PORT_STAT_ENABLE))
        failure_reason = "reset-completed-port-disabled";
    else if (hub_speed == XHCI_SPEED_SUPER &&
             (status & USB_PORT_STAT_LINK_STATE) != USB_SS_PORT_LS_U0)
        failure_reason = "reset-completed-link-not-u0";

failed:
    if (out_status) *out_status = status;
    if (out_change) *out_change = change;
    if (out_reason) *out_reason = failure_reason;
    xhci_hub_reset_log(hub_slot_id, port, state, status, change,
                       failure_reason);
    hub_clear_port_changes(xhc, hub_slot_id, port, change);
    return result == XHCI_SUCCESS ? XHCI_ERR_PORT_RESET_FAIL : result;
}

static xhci_speed_t hub_child_speed(xhci_speed_t hub_speed, u16 port_status)
{
    if (hub_speed == XHCI_SPEED_SUPER)
        return XHCI_SPEED_SUPER;
    if (port_status & USB_PORT_STAT_LOW_SPEED)
        return XHCI_SPEED_LOW;
    if (port_status & USB_PORT_STAT_HIGH_SPEED)
        return XHCI_SPEED_HIGH;
    return XHCI_SPEED_FULL;
}

static u32 hub_child_route(u32 parent_route, u8 parent_depth, u8 port)
{
    u32 shift = (u32)parent_depth * 4U;
    u32 route_port = port > 15 ? 15 : port;
    if (shift >= 20)
        return parent_route;
    return parent_route | (route_port << shift);
}

xhci_status_t xhci_hub_enumerate_children(
    xhci_controller_t *xhc, u8 hub_slot_id, u8 root_port,
    u32 hub_route_string, u8 hub_depth, xhci_speed_t hub_speed,
    const xhci_hub_descriptor_info_t *descriptor)
{
    uintptr_t status_phys = 0;
    usb_hub_port_status_t *port_status;
    xhci_status_t first_error = XHCI_SUCCESS;

    if (!xhc || !hub_slot_id || !descriptor || !descriptor->num_ports ||
        hub_depth >= 5)
        return XHCI_ERR_INVALID_PARAM;

    port_status = (usb_hub_port_status_t *)
        xhci_dma_alloc(sizeof(*port_status), &status_phys);
    if (!port_status)
        return XHCI_ERR_NO_MEMORY;

#if XHCI_DEBUG
    u32 probe_generation = ++g_hub_probe_generation;
#else
    u32 probe_generation = 0;
#endif
    if (hub_slot_id == 1) {
        XHCI_DEBUG_LOG(
            "[xHCI-HUB] s%u discovery source=boot-hub-probe gen=%u "
            "root=%u route=%05x depth=%u speed=%u ports=%u\n",
            hub_slot_id, probe_generation, root_port, hub_route_string,
            hub_depth, hub_speed, descriptor->num_ports);
    }

    xhci_diag_set_control_quiet(true);
    if (hub_speed == XHCI_SPEED_SUPER) {
        first_error = hub_set_depth(xhc, hub_slot_id, hub_depth);
        if (first_error != XHCI_SUCCESS)
            goto done;
    }

    for (u8 port = 1; port <= descriptor->num_ports; port++) {
        xhci_status_t result = hub_set_port_feature(
            xhc, hub_slot_id, port, USB_PORT_FEAT_POWER);
        if (result != XHCI_SUCCESS && first_error == XHCI_SUCCESS)
            first_error = result;
    }

    if (descriptor->power_on_delay_2ms)
        timer_sleep((u64)descriptor->power_on_delay_2ms * 2U);

    for (u8 port = 1; port <= descriptor->num_ports; port++) {
        xhci_status_t result = hub_get_port_status(
            xhc, hub_slot_id, port, port_status, status_phys);
        if (result != XHCI_SUCCESS) {
            kprint("[xHCI-HUB] s%u p%u status rc=%u\n",
                   hub_slot_id, port, result);
            if (first_error == XHCI_SUCCESS) first_error = result;
            continue;
        }

        u16 status = port_status->status;
        u16 change = port_status->change;
        if (hub_slot_id == 1 && port == 2) {
            XHCI_DEBUG_LOG(
                "[xHCI-HUB] t=%llu s%u p%u discovery-result gen=%u "
                "source=boot-hub-probe st=%04x ch=%04x\n",
                (unsigned long long)timer_uptime_ms(), hub_slot_id, port,
                probe_generation, status, change);
        }
        XHCI_DEBUG_LOG("[xHCI-HUB] s%u p%u st=%04x ch=%04x\n",
                       hub_slot_id, port, status, change);
        if (!(status & USB_PORT_STAT_CONNECTION))
        {
            hub_clear_port_changes(xhc, hub_slot_id, port, change);
            continue;
        }

        const char *reset_reason = NULL;
        result = xhci_hub_reset_port(
            xhc, hub_slot_id, port, hub_speed, port_status, status_phys,
            status, change, &status, &change, &reset_reason);
        if (result != XHCI_SUCCESS) {
            kprint("[xHCI-HUB] s%u p%u reset-fail st=%04x ch=%04x rc=%u reason=%s\n",
                   hub_slot_id, port, status, change, result,
                   reset_reason ? reset_reason : "unknown");
            if (first_error == XHCI_SUCCESS)
                first_error = result;
            continue;
        }

        xhci_speed_t child_speed = hub_child_speed(hub_speed, status);
        u32 child_route = hub_child_route(hub_route_string, hub_depth, port);
        if (hub_slot_id == 1 && port == 2) {
            XHCI_DEBUG_LOG(
                "[xHCI-HUB] t=%llu s%u p%u ready gen=%u root=%u "
                "parent-route=%05x child-route=%05x depth=%u speed=%u\n",
                (unsigned long long)timer_uptime_ms(), hub_slot_id, port,
                probe_generation, root_port, hub_route_string, child_route,
                hub_depth + 1, child_speed);
        }
        XHCI_DEBUG_LOG("[xHCI-HUB] s%u p%u reset st=%04x sp=%u route=%05x\n",
                       hub_slot_id, port, status, child_speed, child_route);

        /* Child descriptor traffic is useful in the existing phase trace. */
        xhci_diag_set_control_quiet(false);
        result = xhci_setup_child_device_locked(
            xhc, root_port, child_speed, child_route, hub_depth + 1,
            hub_slot_id, port, hub_speed);
        xhci_diag_set_control_quiet(true);
        if (result != XHCI_SUCCESS) {
            kprint("[xHCI-HUB] s%u p%u child rc=%u\n",
                   hub_slot_id, port, result);
            if (first_error == XHCI_SUCCESS) first_error = result;
        }
    }

done:
    xhci_diag_set_control_quiet(false);
    xhci_dma_free(port_status, sizeof(*port_status));
    return first_error;
}
