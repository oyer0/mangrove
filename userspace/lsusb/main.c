/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/device_query.h"
#include "../common/hardware_ids.h"
#include "../common/help.h"
#include "../common/table.h"

#define USB_SNAPSHOT_MAX (MG_TABLE_MAX_ROWS - 1U)

static mg_device_info_t usb_devices[USB_SNAPSHOT_MAX];
static mg_table_row_t usb_rows[MG_TABLE_MAX_ROWS];
static mg_hardware_ids_t usb_ids;
static mg_table_t usb_table;

static const mg_table_column_t PORT_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t VENDOR_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DEVICE_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t CLASS_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DRIVER_COLUMN = { MG_TABLE_ALIGN_LEFT };

static const char *usb_class_description(const mg_device_info_t *device)
{
    if (!device) return "USB device";
    switch (device->class_code) {
        case 0x03: return "HID device";
        case 0x08: return "Mass storage";
        case 0x09: return "Hub";
        case 0x02: return "Communications";
        case 0x0e: return "Video";
        case 0x01: return "Audio";
        default: return "USB device";
    }
}

static void port_text(const mg_device_info_t *device, char *out,
                      usize capacity)
{
    if (!out || !capacity || !device) return;
    (void)snprintf(out, capacity, "%u-%u", (u32)device->bus,
                   (u32)device->slot);
}

static const mg_hardware_id_match_t *usb_id(
    const mg_hardware_ids_t *ids, const mg_device_info_t *device)
{
    return hardware_ids_find(ids, device->vendor_id, device->device_id);
}

static const char *vendor_text(const mg_hardware_id_match_t *match,
                               char *out, usize capacity, u16 id)
{
    if (match && match->vendor_found) return match->vendor;
    (void)snprintf(out, capacity, "0x%04x", id);
    return out;
}

static const char *device_text(const mg_hardware_id_match_t *match,
                               char *out, usize capacity, u16 id)
{
    if (match && match->device_found) return match->device;
    (void)snprintf(out, capacity, "0x%04x", id);
    return out;
}

static bool append_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &PORT_COLUMN, "BUS/PORT");
    table_row_column(row, &VENDOR_COLUMN, "VENDOR");
    table_row_column(row, &DEVICE_COLUMN, "DEVICE");
    table_row_column(row, &CLASS_COLUMN, "CLASS");
    table_row_column(row, &DRIVER_COLUMN, "DRIVER");
    return true;
}

static bool append_device(mg_table_t *table, const mg_device_info_t *device,
                          const mg_hardware_ids_t *ids)
{
    mg_table_row_t *row = table_row_begin(table);
    const mg_hardware_id_match_t *match = usb_id(ids, device);
    char port[16];
    char vendor[24];
    char product[24];

    if (!row) return false;
    port_text(device, port, sizeof(port));
    table_row_column(row, &PORT_COLUMN, port);
    table_row_column(row, &VENDOR_COLUMN,
                     vendor_text(match, vendor, sizeof(vendor),
                                  device->vendor_id));
    table_row_column(row, &DEVICE_COLUMN,
                     device_text(match, product, sizeof(product),
                                 device->device_id));
    table_row_column(row, &CLASS_COLUMN, usb_class_description(device));
    table_row_column(row, &DRIVER_COLUMN,
                     device->driver[0] ? device->driver : "-");
    return true;
}

static void print_verbose(const mg_device_info_t *device,
                          const mg_hardware_ids_t *ids)
{
    const mg_hardware_id_match_t *match = usb_id(ids, device);
    char port[16];

    port_text(device, port, sizeof(port));
    printf("%s %04x:%04x %s\n", port, device->vendor_id,
           device->device_id, usb_class_description(device));
    if (match && match->vendor_found)
        printf("  vendor: %s\n", match->vendor);
    if (match && match->device_found)
        printf("  device: %s\n", match->device);
    printf("  class: %02x/%02x/%02x\n", device->class_code,
           device->subclass, device->prog_if);
    printf("  speed: %u\n", (u32)device->usb_speed);
    printf("  slot: %u\n", (u32)device->function);
    printf("  driver: %s\n", device->driver[0] ? device->driver : "-");
}

int main(int argc, char **argv)
{
    u32 count = 0;
    bool verbose = false;
    mg_result_t result;

    if (command_help_requested(argc, argv)) return command_print_help(argv[0]);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "-v") != 0)) {
        command_usage_error(argv[0], "lsusb [-v]", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    verbose = argc == 2;
    result = device_query_category(MG_DEVICE_CATEGORY_USB, usb_devices,
                                   USB_SNAPSHOT_MAX, &count);
    if (result != MG_OK) {
        printf("lsusb: %s\n", error_string(result));
        return 1;
    }
    hardware_ids_init(&usb_ids);
    for (u32 index = 0; index < count; index++)
        (void)hardware_ids_add(&usb_ids, usb_devices[index].vendor_id,
                               usb_devices[index].device_id);
    (void)hardware_ids_load("/share/hardware/usb.ids", &usb_ids);
    if (verbose) {
        for (u32 index = 0; index < count; index++) {
            if (index) putchar('\n');
            print_verbose(&usb_devices[index], &usb_ids);
        }
        if (!count) puts("No USB devices.");
        return 0;
    }
    table_init(&usb_table, usb_rows, MG_TABLE_MAX_ROWS);
    if (!append_header(&usb_table)) return 1;
    for (u32 index = 0; index < count; index++)
        if (!append_device(&usb_table, &usb_devices[index], &usb_ids)) {
            printf("lsusb: table is too large.\n");
            return 1;
        }
    if (!table_render(&usb_table)) {
        printf("lsusb: invalid table data.\n");
        return 1;
    }
    return 0;
}
