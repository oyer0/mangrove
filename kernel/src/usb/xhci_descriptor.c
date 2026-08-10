#include <xhci.h>
#include <xhci_hub.h>
#include <stddef.h>

/* ==============================================================================
 * External Dependencies
 * ============================================================================== */

/* Memory Subsystem (implemented in xhci_mem.c) */
extern void* xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void  xhci_dma_free(void* virt, usize size);

/* Control Transfers (implemented in xhci_control.c) */
extern xhci_status_t xhci_control_get_descriptor(xhci_controller_t *xhc, u8 slot_id, 
                                                 u8 desc_type, u8 desc_index, 
                                                 u16 length, uintptr_t buffer_phys);

/* Mangrove OS Logging */
extern void kprint(const char *fmt, ...);


/* ==============================================================================
 * Standard USB 2.0 / 3.0 Descriptor Constants
 * ============================================================================== */

#define XHCI_USB_DESC_TYPE_DEVICE         0x01
#define XHCI_USB_DESC_TYPE_CONFIGURATION  0x02
#define XHCI_USB_DESC_TYPE_STRING         0x03
#define XHCI_USB_DESC_TYPE_INTERFACE      0x04
#define XHCI_USB_DESC_TYPE_ENDPOINT       0x05
#define XHCI_USB_DESC_TYPE_HID            0x21
#define XHCI_USB_DESC_TYPE_SS_EP_COMPANION 0x30

/* USB HID Class Definitions */
#define XHCI_USB_CLASS_HID                0x03
#define XHCI_USB_SUBCLASS_BOOT            0x01
#define XHCI_USB_PROTOCOL_KEYBOARD        0x01
#define XHCI_USB_CLASS_HUB                0x09

/* Endpoint Attributes */
#define XHCI_USB_EP_ATTR_TYPE_MASK        0x03
#define XHCI_USB_EP_ATTR_INTR             0x03
#define XHCI_USB_EP_DIR_IN                0x80


/* ==============================================================================
 * Standard USB Descriptor Structures (Packed)
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bInterfaceNumber;
    u8  bAlternateSetting;
    u8  bNumEndpoints;
    u8  bInterfaceClass;
    u8  bInterfaceSubClass;
    u8  bInterfaceProtocol;
    u8  iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} usb_endpoint_descriptor_t;

static void xhci_dump_configuration(u8 slot_id, const u8 *buffer, u16 total_length)
{
    XHCI_DEBUG_LOG("[xHCI-CFG] s%u total=%u\n", slot_id, total_length);
    for (u16 row = 0; row < total_length; row += 12) {
        XHCI_DEBUG_LOG("[xHCI-CFG-RAW] +%u", row);
        u16 end = row + 12;
        if (end > total_length) end = total_length;
        for (u16 i = row; i < end; i++)
            XHCI_DEBUG_LOG(" %02x", buffer[i]);
        XHCI_DEBUG_LOG("\n");
    }

    for (u16 offset = 0; offset + 2 <= total_length;) {
        u8 length = buffer[offset];
        u8 type = buffer[offset + 1];
        if (length < 2 || offset + length > total_length) {
            XHCI_DEBUG_LOG("[xHCI-CFG-D] +%u invalid l=%u t=%02x\n",
                           offset, length, type);
            break;
        }

        if (type == XHCI_USB_DESC_TYPE_CONFIGURATION && length >= 9) {
            const usb_config_descriptor_t *cfg =
                (const usb_config_descriptor_t *)(buffer + offset);
            XHCI_DEBUG_LOG("[xHCI-CFG-D] +%u CFG l=%u total=%u ifs=%u val=%u attr=%02x pwr=%u\n",
                           offset, length, cfg->wTotalLength,
                           cfg->bNumInterfaces, cfg->bConfigurationValue,
                           cfg->bmAttributes, cfg->bMaxPower);
        } else if (type == XHCI_USB_DESC_TYPE_INTERFACE && length >= 9) {
            const usb_interface_descriptor_t *iface =
                (const usb_interface_descriptor_t *)(buffer + offset);
            XHCI_DEBUG_LOG("[xHCI-CFG-D] +%u IF l=%u n=%u alt=%u eps=%u cls=%02x/%02x/%02x\n",
                           offset, length, iface->bInterfaceNumber,
                           iface->bAlternateSetting, iface->bNumEndpoints,
                           iface->bInterfaceClass, iface->bInterfaceSubClass,
                           iface->bInterfaceProtocol);
        } else if (type == XHCI_USB_DESC_TYPE_ENDPOINT && length >= 7) {
            const usb_endpoint_descriptor_t *ep =
                (const usb_endpoint_descriptor_t *)(buffer + offset);
            XHCI_DEBUG_LOG("[xHCI-CFG-D] +%u EP l=%u a=%02x attr=%02x mps=%u int=%u\n",
                           offset, length, ep->bEndpointAddress,
                           ep->bmAttributes, ep->wMaxPacketSize,
                           ep->bInterval);
        } else if (type == XHCI_USB_DESC_TYPE_SS_EP_COMPANION && length >= 6) {
            u16 bytes_per_interval = (u16)buffer[offset + 4] |
                                     ((u16)buffer[offset + 5] << 8);
            XHCI_DEBUG_LOG("[xHCI-CFG-D] +%u SSC l=%u burst=%u attr=%02x bytes=%u\n",
                           offset, length, buffer[offset + 2],
                           buffer[offset + 3], bytes_per_interval);
        } else {
            XHCI_DEBUG_LOG("[xHCI-CFG-D] +%u OTHER l=%u t=%02x\n",
                           offset, length, type);
        }
        offset += length;
    }
}


/* ==============================================================================
 * Initial Descriptor Retrieval (Phase 5)
 * ============================================================================== */

/*
 * Reads the first 8 bytes of the Device Descriptor strictly to determine the 
 * true Maximum Packet Size of the Control Endpoint (EP0).
 * * @param xhc             The controller instance.
 * @param slot_id         The target Slot ID.
 * @param out_max_packet  Pointer to store the extracted bMaxPacketSize0.
 * @return                XHCI_SUCCESS or standard error code.
 */
xhci_status_t xhci_read_ep0_max_packet_size(xhci_controller_t *xhc, u8 slot_id, u8 *out_max_packet) {
    if (!xhc || !out_max_packet) return XHCI_ERR_INVALID_PARAM;

    uintptr_t buffer_phys = 0;
    
    /* 8 bytes is the minimum size guaranteed to be supported before Context Evaluation */
    u8 *dma_buffer = (u8 *)xhci_dma_alloc(8, &buffer_phys);
    if (!dma_buffer) return XHCI_ERR_NO_MEMORY;

    xhci_diag_set_phase("EP0 max-packet read");
    xhci_status_t err = xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_DEVICE, 0, 8, buffer_phys);

    if (err == XHCI_SUCCESS) {
        /* Byte 7 of the standard Device Descriptor is bMaxPacketSize0 */
        *out_max_packet = dma_buffer[7];
    }

    xhci_dma_free(dma_buffer, 8);
    return err;
}


/* ==============================================================================
 * Complex Descriptor Parsing (Phase 5 & 6)
 * ============================================================================== */

/*
 * Retrieves the full Configuration Descriptor hierarchy and recursively parses 
 * the variable-length blob to locate the USB HID Keyboard Interface and its 
 * associated Interrupt IN Endpoint.
 *
 * Extracts all parameters necessary to configure the endpoint context and 
 * perform the SET_CONFIGURATION and SET_PROTOCOL transfers.
 *
 * @param xhc               The controller instance.
 * @param slot_id           The target Slot ID.
 * @param out_ep_addr       Outputs the raw Endpoint Address (e.g., 0x81).
 * @param out_max_pkt       Outputs the endpoint's wMaxPacketSize.
 * @param out_interval      Outputs the endpoint's polling interval.
 * @param out_config_val    Outputs the target bConfigurationValue for Phase 6.
 * @param out_interface_num Outputs the target bInterfaceNumber for Phase 6.
 * @return                  XHCI_SUCCESS if successfully parsed and a keyboard was found.
 */
xhci_status_t xhci_get_keyboard_endpoint_info(xhci_controller_t *xhc, u8 slot_id, 
                                              u8 *out_ep_addr, u16 *out_max_pkt, 
                                              u8 *out_interval, u8 *out_config_val, 
                                              u8 *out_interface_num) 
{
    if (!xhc || !out_ep_addr || !out_max_pkt || !out_interval || !out_config_val || !out_interface_num) {
        return XHCI_ERR_INVALID_PARAM;
    }

    uintptr_t buffer_phys = 0;
    
    /* Allocate 4096 bytes (1 Page) to safely buffer any massive composite descriptor */
    usize alloc_size = 4096;
    u8 *dma_buffer = (u8 *)xhci_dma_alloc(alloc_size, &buffer_phys);
    if (!dma_buffer) return XHCI_ERR_NO_MEMORY;

    /* Read the first 9 bytes to retrieve the Configuration Header and total length */
    xhci_diag_set_phase("config-header read");
    xhci_status_t err = xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_CONFIGURATION, 0, 9, buffer_phys);
    if (err != XHCI_SUCCESS) {
        xhci_dma_free(dma_buffer, alloc_size);
        return err;
    }

    usb_config_descriptor_t *config_header = (usb_config_descriptor_t *)dma_buffer;
    u16 total_length = config_header->wTotalLength;

    if (total_length > alloc_size) {
        kprint("[xHCI] Error: Configuration descriptor exceeds 4KB buffer.\n");
        xhci_dma_free(dma_buffer, alloc_size);
        return XHCI_ERR_NO_MEMORY;
    }

    /* Perform the full read now that the required byte span is known */
    xhci_diag_set_phase("config-full read");
    err = xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_CONFIGURATION, 0, total_length, buffer_phys);
    if (err != XHCI_SUCCESS) {
        xhci_dma_free(dma_buffer, alloc_size);
        return err;
    }

    /* Parse the contiguous descriptor blob */
    xhci_diag_set_phase("interface discovery");
    u16 offset = 0;
    bool in_keyboard_interface = false;
    u8 active_config_value = config_header->bConfigurationValue;

    while (offset < total_length) {
        u8 desc_len = dma_buffer[offset];
        if (desc_len == 0) {
            break; /* Prevent infinite loops on corrupted device data */
        }
        
        u8 desc_type = dma_buffer[offset + 1];

        if (desc_type == XHCI_USB_DESC_TYPE_INTERFACE) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)(dma_buffer + offset);
            
            /* Identify standard Boot Protocol Keyboard */
            if (iface->bInterfaceClass == XHCI_USB_CLASS_HID && 
                iface->bInterfaceSubClass == XHCI_USB_SUBCLASS_BOOT && 
                iface->bInterfaceProtocol == XHCI_USB_PROTOCOL_KEYBOARD) 
            {
                in_keyboard_interface = true;
                *out_config_val = active_config_value;
                *out_interface_num = iface->bInterfaceNumber;
            } else {
                in_keyboard_interface = false;
            }
        } 
        else if (desc_type == XHCI_USB_DESC_TYPE_ENDPOINT && in_keyboard_interface) {
            usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)(dma_buffer + offset);
            
            /* Verify it is an Interrupt Endpoint and Direction is IN */
            if ((ep->bmAttributes & XHCI_USB_EP_ATTR_TYPE_MASK) == XHCI_USB_EP_ATTR_INTR && 
                (ep->bEndpointAddress & XHCI_USB_EP_DIR_IN)) 
            {
                *out_ep_addr = ep->bEndpointAddress;
                /* Mask out transaction opportunities (bits 11:12) to isolate base packet size */
                *out_max_pkt = ep->wMaxPacketSize & 0x07FF; 
                *out_interval = ep->bInterval;

                xhci_dma_free(dma_buffer, alloc_size);
                return XHCI_SUCCESS;
            }
        }

        offset += desc_len;
    }

    XHCI_DEBUG_LOG("[xHCI-DIAG] Device is not a HID Boot Keyboard\n");
    xhci_dma_free(dma_buffer, alloc_size);
    return XHCI_ERR_NOT_SUPPORTED;
}

/* Locate the two bulk endpoints used by a USB Mass Storage BOT interface. */
xhci_status_t xhci_get_mass_storage_endpoint_info(xhci_controller_t *xhc, u8 slot_id,
                                                  u8 *out_bulk_in, u16 *out_bulk_in_pkt,
                                                  u8 *out_bulk_out, u16 *out_bulk_out_pkt,
                                                  u8 *out_config_val, bool dump_on_miss)
{
    uintptr_t buffer_phys = 0;
    usize alloc_size = 4096;
    u8 *buffer;
    u16 total_length;
    u16 offset;
    bool mass_storage_interface = false;

    if (!xhc || !out_bulk_in || !out_bulk_in_pkt || !out_bulk_out ||
        !out_bulk_out_pkt || !out_config_val) return XHCI_ERR_INVALID_PARAM;

    buffer = (u8 *)xhci_dma_alloc(alloc_size, &buffer_phys);
    if (!buffer) return XHCI_ERR_NO_MEMORY;
    xhci_diag_set_phase("config-header read");
    if (xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_CONFIGURATION,
                                    0, 9, buffer_phys) != XHCI_SUCCESS) {
        xhci_dma_free(buffer, alloc_size);
        return XHCI_ERR_NOT_SUPPORTED;
    }

    total_length = ((usb_config_descriptor_t *)buffer)->wTotalLength;
    *out_config_val = ((usb_config_descriptor_t *)buffer)->bConfigurationValue;
    xhci_diag_set_phase("config-full read");
    if (total_length < 9 || total_length > alloc_size ||
        xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_CONFIGURATION,
                                    0, total_length, buffer_phys) != XHCI_SUCCESS) {
        xhci_dma_free(buffer, alloc_size);
        return XHCI_ERR_NOT_SUPPORTED;
    }

    xhci_diag_set_phase("interface discovery");
    *out_bulk_in = 0;
    *out_bulk_out = 0;
    for (offset = 0; offset + 2 <= total_length;) {
        u8 length = buffer[offset];
        u8 type = buffer[offset + 1];
        if (length < 2 || offset + length > total_length) break;
        if (type == XHCI_USB_DESC_TYPE_INTERFACE && length >= 9) {
            usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)(buffer + offset);
            mass_storage_interface = iface->bInterfaceClass == 0x08 &&
                                     iface->bInterfaceSubClass == 0x06 &&
                                     iface->bInterfaceProtocol == 0x50;
        } else if (type == XHCI_USB_DESC_TYPE_ENDPOINT && mass_storage_interface && length >= 7) {
            usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t *)(buffer + offset);
            if ((ep->bmAttributes & XHCI_USB_EP_ATTR_TYPE_MASK) == 2) {
                if (ep->bEndpointAddress & XHCI_USB_EP_DIR_IN) {
                    *out_bulk_in = ep->bEndpointAddress;
                    *out_bulk_in_pkt = ep->wMaxPacketSize & 0x07ff;
                } else {
                    *out_bulk_out = ep->bEndpointAddress;
                    *out_bulk_out_pkt = ep->wMaxPacketSize & 0x07ff;
                }
            }
        }
        offset += length;
    }

    bool found = *out_bulk_in && *out_bulk_out;
    if (!found && dump_on_miss) {
        xhci_dump_configuration(slot_id, buffer, total_length);
        xhci_diag_set_phase("device-descriptor diagnostic");
        if (xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_DEVICE,
                                        0, sizeof(usb_device_descriptor_t),
                                        buffer_phys) == XHCI_SUCCESS) {
            const usb_device_descriptor_t *device =
                (const usb_device_descriptor_t *)buffer;
            XHCI_DEBUG_LOG("[xHCI-DEV] s%u cls=%02x/%02x/%02x cfgs=%u vid=%04x pid=%04x\n",
                           slot_id, device->bDeviceClass,
                           device->bDeviceSubClass, device->bDeviceProtocol,
                           device->bNumConfigurations, device->idVendor,
                           device->idProduct);
        }
    }

    xhci_dma_free(buffer, alloc_size);
    return found ? XHCI_SUCCESS : XHCI_ERR_NOT_SUPPORTED;
}

/* Locate a hub interface and its interrupt-IN status endpoint. */
xhci_status_t xhci_get_hub_endpoint_info(
    xhci_controller_t *xhc, u8 slot_id, xhci_speed_t speed,
    xhci_hub_endpoint_info_t *out_info)
{
    uintptr_t buffer_phys = 0;
    const usize alloc_size = 4096;
    u8 *buffer;
    u16 total_length;
    bool in_hub_interface = false;
    bool found_endpoint = false;

    if (!xhc || !slot_id || !out_info)
        return XHCI_ERR_INVALID_PARAM;

    __builtin_memset(out_info, 0, sizeof(*out_info));
    buffer = (u8 *)xhci_dma_alloc(alloc_size, &buffer_phys);
    if (!buffer)
        return XHCI_ERR_NO_MEMORY;

    xhci_diag_set_phase("hub config-header");
    if (xhci_control_get_descriptor(xhc, slot_id,
                                    XHCI_USB_DESC_TYPE_CONFIGURATION, 0, 9,
                                    buffer_phys) != XHCI_SUCCESS) {
        xhci_dma_free(buffer, alloc_size);
        return XHCI_ERR_NOT_SUPPORTED;
    }

    const usb_config_descriptor_t *config =
        (const usb_config_descriptor_t *)buffer;
    total_length = config->wTotalLength;
    if (config->bLength < 9 ||
        config->bDescriptorType != XHCI_USB_DESC_TYPE_CONFIGURATION ||
        total_length < 9 || total_length > alloc_size) {
        xhci_dma_free(buffer, alloc_size);
        return XHCI_ERR_NOT_SUPPORTED;
    }
    out_info->config_value = config->bConfigurationValue;

    xhci_diag_set_phase("hub config-full");
    if (xhci_control_get_descriptor(xhc, slot_id,
                                    XHCI_USB_DESC_TYPE_CONFIGURATION, 0,
                                    total_length, buffer_phys) != XHCI_SUCCESS) {
        xhci_dma_free(buffer, alloc_size);
        return XHCI_ERR_NOT_SUPPORTED;
    }

    for (u16 offset = 0; offset + 2 <= total_length;) {
        u8 length = buffer[offset];
        u8 type = buffer[offset + 1];
        if (length < 2 || offset + length > total_length)
            break;

        if (type == XHCI_USB_DESC_TYPE_INTERFACE && length >= 9) {
            const usb_interface_descriptor_t *interface =
                (const usb_interface_descriptor_t *)(buffer + offset);
            in_hub_interface = interface->bAlternateSetting == 0 &&
                interface->bInterfaceClass == XHCI_USB_CLASS_HUB;
            if (in_hub_interface) {
                out_info->interface_number = interface->bInterfaceNumber;
                out_info->interface_protocol = interface->bInterfaceProtocol;
            }
        } else if (type == XHCI_USB_DESC_TYPE_ENDPOINT &&
                   in_hub_interface && length >= 7) {
            const usb_endpoint_descriptor_t *endpoint =
                (const usb_endpoint_descriptor_t *)(buffer + offset);
            if ((endpoint->bmAttributes & XHCI_USB_EP_ATTR_TYPE_MASK) ==
                    XHCI_USB_EP_ATTR_INTR &&
                (endpoint->bEndpointAddress & XHCI_USB_EP_DIR_IN)) {
                out_info->endpoint_address = endpoint->bEndpointAddress;
                out_info->max_packet_size =
                    endpoint->wMaxPacketSize & 0x07ff;
                out_info->interval = endpoint->bInterval;
                found_endpoint = true;
            }
        } else if (type == XHCI_USB_DESC_TYPE_SS_EP_COMPANION &&
                   found_endpoint && in_hub_interface && length >= 6) {
            out_info->max_burst = buffer[offset + 2];
            out_info->mult = buffer[offset + 3] & 0x03;
            out_info->bytes_per_interval =
                (u16)buffer[offset + 4] | ((u16)buffer[offset + 5] << 8);
            out_info->has_ss_companion = true;
        }
        offset += length;
    }

    xhci_dma_free(buffer, alloc_size);
    if (!found_endpoint ||
        (speed == XHCI_SPEED_SUPER && !out_info->has_ss_companion))
        return XHCI_ERR_NOT_SUPPORTED;
    return XHCI_SUCCESS;
}
