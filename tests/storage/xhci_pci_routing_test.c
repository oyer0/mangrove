/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <xhci_pci.h>

static u32 config[64];
static u8 writes[4];
static u32 reads, write_count;
static u8 refused;
static pci_device_t controller = {
    /* Deliberately not the X250's BDF: this is a controller contract. */
    .bus = 7, .device = 3, .function = 2,
    .vendor_id = 0x8086, .device_id = 0x9cb1,
    .class_code = 0x0c, .subclass = 3, .prog_if = 0x30
};

u32 pci_read_config32(const pci_device_t *device, u8 offset)
{
    assert(device == &controller);
    assert(offset == 0xd0 || offset == 0xd4 ||
           offset == 0xd8 || offset == 0xdc);
    reads++;
    return config[offset / 4];
}

void pci_write_config32(const pci_device_t *device, u8 offset, u32 value)
{
    assert(device == &controller && write_count < 4);
    /* Masks and all unrelated configuration are never written. */
    assert(offset == 0xd0 || offset == 0xd8);
    u32 allowed = config[(offset + 4) / 4] & (offset == 0xd0 ? 0xff : 0x0f);
    assert(((config[offset / 4] ^ value) & ~allowed) == 0);
    writes[write_count++] = offset;
    if (offset != refused)
        config[offset / 4] = value;
}

static void reset(void)
{
    memset(config, 0, sizeof(config));
    reads = write_count = refused = 0;
}

int main(void)
{
    xhci_pci_routing_t r;
    reset();
    /* A permitted USB2 connector initially assigned to EHCI is invisible to
       xHCI. Changing its selector is necessary before native enumeration. */
    config[0xd4 / 4] = 1U << 5;
    config[0xdc / 4] = 3;
    assert((config[0xd0 / 4] & (1U << 5)) == 0);
    assert(xhci_pci_route_ports(&controller, &r));
    assert(r.supported && r.usb2_before == 0 && r.usb2_after == (1U << 5));
    assert(r.usb3_before == 0 && r.usb3_after == 3);
    assert(write_count == 2 && writes[0] == 0xd8 && writes[1] == 0xd0);
    assert(config[0xd4 / 4] == (1U << 5) && config[0xdc / 4] == 3);
    write_count = 0;
    assert(xhci_pci_route_ports(&controller, &r) && write_count == 0);

    reset(); /* Firmware forbids switching: no invented permissions. */
    assert(xhci_pci_route_ports(&controller, &r));
    assert(r.usb2_mask == 0 && r.usb3_mask == 0 && write_count == 0);

    reset(); /* Partial permissions, existing routes and reserved bits. */
    config[0xd0 / 4] = 0xa5000101;
    config[0xd4 / 4] = 0xffff0024;
    config[0xd8 / 4] = 0x80000002;
    config[0xdc / 4] = 0xffff0005;
    assert(xhci_pci_route_ports(&controller, &r));
    assert(r.usb2_after == 0xa5000125 && r.usb3_after == 0x80000007);
    assert(r.usb2_mask == 0x24 && r.usb3_mask == 5);

    for (u8 offset = 0xd0; offset <= 0xdc; offset += 4) {
        reset();
        config[offset / 4] = 0xffffffffU;
        assert(!xhci_pci_route_ports(&controller, &r));
        assert(write_count == 0);
    }
    for (u8 offset = 0xd0; offset <= 0xd8; offset += 8) {
        reset(); refused = offset;
        config[0xd4 / 4] = config[0xdc / 4] = 1;
        assert(!xhci_pci_route_ports(&controller, &r));
        assert(write_count == (offset == 0xd8 ? 1U : 2U));
        assert(config[offset / 4] == 0); /* no retry or false success */
    }

    reset();
    controller.device_id = 0x9c31; /* Unqualified Intel generation. */
    assert(xhci_pci_route_ports(&controller, &r) && !r.supported);
    controller.device_id = 0x9cb1; controller.vendor_id = 0x1234;
    assert(xhci_pci_route_ports(&controller, &r) && !r.supported);
    controller.vendor_id = 0x8086; controller.prog_if = 0x20;
    assert(xhci_pci_route_ports(&controller, &r) && !r.supported);
    controller.prog_if = 0x30; controller.class_code = 1;
    assert(xhci_pci_route_ports(&controller, &r) && !r.supported);
    assert(xhci_pci_route_ports(NULL, &r) && !r.supported);
    assert(!xhci_pci_route_ports(&controller, NULL));
    assert(reads == 0 && write_count == 0);
    return 0;
}
