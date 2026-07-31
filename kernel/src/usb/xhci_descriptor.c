#include <xhci.h>
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

/* USB HID Class Definitions */
#define XHCI_USB_CLASS_HID                0x03
#define XHCI_USB_SUBCLASS_BOOT            0x01
#define XHCI_USB_PROTOCOL_KEYBOARD        0x01

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
    err = xhci_control_get_descriptor(xhc, slot_id, XHCI_USB_DESC_TYPE_CONFIGURATION, 0, total_length, buffer_phys);
    if (err != XHCI_SUCCESS) {
        xhci_dma_free(dma_buffer, alloc_size);
        return err;
    }

    /* Parse the contiguous descriptor blob */
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

    kprint("[xHCI] Error: Device does not contain a HID Boot Keyboard Interface.\n");
    xhci_dma_free(dma_buffer, alloc_size);
    return XHCI_ERR_NOT_SUPPORTED;
}
