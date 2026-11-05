/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pci.h>

typedef struct {
    bool supported;
    u32 usb2_mask;
    u32 usb3_mask;
    u32 usb2_before;
    u32 usb2_after;
    u32 usb3_before;
    u32 usb3_after;
} xhci_pci_routing_t;

/* Boot-only, after xHC firmware handoff/reset and before starting its ports.
 * Unknown controllers receive no configuration accesses. Firmware permission
 * masks remain authoritative; this does not implement an EHCI driver. */
bool xhci_pci_route_ports(const pci_device_t *device,
                          xhci_pci_routing_t *report);
