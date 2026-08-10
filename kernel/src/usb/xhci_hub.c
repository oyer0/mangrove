#include <xhci_hub.h>
#include <stddef.h>

extern void *xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void xhci_dma_free(void *virt, usize size);
extern void timer_sleep(u64 ms);
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
#define USB_PORT_FEAT_C_PORT_LINK_STATE     25
#define USB_PORT_FEAT_C_PORT_CONFIG_ERROR   26
#define USB_PORT_FEAT_C_BH_PORT_RESET       29

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

typedef struct __attribute__((packed)) {
    u16 status;
    u16 change;
} usb_hub_port_status_t;

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
    xhci_status_t result = hub_control(
        xhc, slot_id, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_GET_STATUS, 0, port, sizeof(*status), status_phys);
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

    for (u32 i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        if (change & changes[i].mask)
            (void)hub_clear_port_feature(xhc, slot_id, port,
                                         changes[i].feature);
    }
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
        XHCI_DEBUG_LOG("[xHCI-HUB] s%u p%u st=%04x ch=%04x\n",
                       hub_slot_id, port, status, change);
        hub_clear_port_changes(xhc, hub_slot_id, port, change);
        if (!(status & USB_PORT_STAT_CONNECTION))
            continue;

        result = hub_set_port_feature(xhc, hub_slot_id, port,
                                      USB_PORT_FEAT_RESET);
        if (result != XHCI_SUCCESS) {
            if (first_error == XHCI_SUCCESS) first_error = result;
            continue;
        }

        bool reset_complete = false;
        for (u32 waited = 0; waited < 500; waited += 10) {
            timer_sleep(10);
            result = hub_get_port_status(xhc, hub_slot_id, port,
                                         port_status, status_phys);
            if (result != XHCI_SUCCESS)
                break;
            status = port_status->status;
            change = port_status->change;
            bool link_ready = hub_speed != XHCI_SPEED_SUPER ||
                (status & USB_PORT_STAT_LINK_STATE) == USB_SS_PORT_LS_U0;
            if ((status & USB_PORT_STAT_CONNECTION) &&
                (status & USB_PORT_STAT_ENABLE) &&
                !(status & USB_PORT_STAT_RESET) && link_ready) {
                reset_complete = true;
                break;
            }
        }
        hub_clear_port_changes(xhc, hub_slot_id, port, change);
        if (!reset_complete) {
            kprint("[xHCI-HUB] s%u p%u reset-fail st=%04x ch=%04x rc=%u\n",
                   hub_slot_id, port, status, change, result);
            if (first_error == XHCI_SUCCESS)
                first_error = result == XHCI_SUCCESS ?
                    XHCI_ERR_PORT_RESET_FAIL : result;
            continue;
        }

        timer_sleep(50);
        xhci_speed_t child_speed = hub_child_speed(hub_speed, status);
        u32 child_route = hub_child_route(hub_route_string, hub_depth, port);
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
