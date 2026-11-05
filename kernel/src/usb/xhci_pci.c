/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <xhci_pci.h>

/* Intel 330837-002, table 1-4 and sections 13.1.33--13.1.36.
 * 8086:9cb1 is the Broadwell/Wildcat Point-LP xHC. The documented firmware
 * permission fields cover eight switchable USB2 ports and four SS ports.
 * Additional/internal ports and all other configuration bits are preserved.
 * Never infer this register contract from the Intel vendor ID alone. */
#define INTEL_USB2_ROUTE        0xd0U
#define INTEL_USB2_PERMISSION   0xd4U
#define INTEL_USB3_ENABLE       0xd8U
#define INTEL_USB3_PERMISSION   0xdcU
#define INTEL_USB2_PORTS        0xffU
#define INTEL_USB3_PORTS        0x0fU

bool xhci_pci_route_ports(const pci_device_t *device,
                          xhci_pci_routing_t *report)
{
    if (!report)
        return false;
    *report = (xhci_pci_routing_t){0};
    if (!device || device->vendor_id != 0x8086U ||
        device->device_id != 0x9cb1U || device->class_code != 0x0cU ||
        device->subclass != 0x03U || device->prog_if != 0x30U)
        return true;

    report->supported = true;
    u32 usb2_permission = pci_read_config32(device, INTEL_USB2_PERMISSION);
    u32 usb3_permission = pci_read_config32(device, INTEL_USB3_PERMISSION);
    report->usb2_before = pci_read_config32(device, INTEL_USB2_ROUTE);
    report->usb3_before = pci_read_config32(device, INTEL_USB3_ENABLE);
    report->usb2_after = report->usb2_before;
    report->usb3_after = report->usb3_before;

    /* An inaccessible function must not become permission to write. */
    if (usb2_permission == 0xffffffffU || usb3_permission == 0xffffffffU ||
        report->usb2_before == 0xffffffffU ||
        report->usb3_before == 0xffffffffU)
        return false;

    report->usb2_mask = usb2_permission & INTEL_USB2_PORTS;
    report->usb3_mask = usb3_permission & INTEL_USB3_PORTS;
    u32 usb3_wanted = report->usb3_before | report->usb3_mask;
    u32 usb2_wanted = report->usb2_before | report->usb2_mask;

    /* Enable permitted SS visibility before moving USB2 pins. Do not rewrite
     * a register already configured correctly, or touch permission masks. */
    if (usb3_wanted != report->usb3_before) {
        pci_write_config32(device, INTEL_USB3_ENABLE, usb3_wanted);
        report->usb3_after = pci_read_config32(device, INTEL_USB3_ENABLE);
        if (report->usb3_after != usb3_wanted)
            return false;
    }
    if (usb2_wanted != report->usb2_before) {
        pci_write_config32(device, INTEL_USB2_ROUTE, usb2_wanted);
        report->usb2_after = pci_read_config32(device, INTEL_USB2_ROUTE);
        if (report->usb2_after != usb2_wanted)
            return false;
    }
    return true;
}
