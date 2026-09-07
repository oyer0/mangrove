/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/device_query.h"
#include "../common/hardware_ids.h"
#include "../common/help.h"
#include "../common/table.h"

#define PCI_SNAPSHOT_MAX (MG_TABLE_MAX_ROWS - 1U)

static mg_device_info_t pci_devices[PCI_SNAPSHOT_MAX];
static mg_table_row_t pci_rows[MG_TABLE_MAX_ROWS];
static mg_hardware_ids_t pci_ids;
static mg_table_t pci_table;

static const mg_table_column_t BDF_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t VENDOR_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DEVICE_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t CLASS_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DRIVER_COLUMN = { MG_TABLE_ALIGN_LEFT };

static const char *pci_class_description(const mg_device_info_t *device)
{
    if (!device) return "PCI device";
    switch (device->class_code) {
        case 0x01: return device->subclass == 0x06
            ? "SATA controller" : "Mass storage controller";
        case 0x02: return "Ethernet controller";
        case 0x03: return "Display controller";
        case 0x04: return "Multimedia controller";
        case 0x06: return "PCI bridge";
        case 0x07: return "Communication controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input device controller";
        case 0x0c: return device->subclass == 0x03
            ? "USB controller" : "Serial bus controller";
        case 0x0d: return "Wireless controller";
        case 0x0e: return "Intelligent controller";
        default: return "PCI device";
    }
}

static void bdf_text(const mg_device_info_t *device, char *out, usize capacity)
{
    if (!out || !capacity || !device) return;
    (void)snprintf(out, capacity, "%02x:%02x.%u", device->bus & 0xffU,
                   device->slot & 0x1fU, device->function & 0x07U);
}

static const mg_hardware_id_match_t *pci_id(
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
    table_row_column(row, &BDF_COLUMN, "BDF");
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
    const mg_hardware_id_match_t *match = pci_id(ids, device);
    char bdf[24];
    char vendor[24];
    char product[24];

    if (!row) return false;
    bdf_text(device, bdf, sizeof(bdf));
    table_row_column(row, &BDF_COLUMN, bdf);
    table_row_column(row, &VENDOR_COLUMN,
                     vendor_text(match, vendor, sizeof(vendor),
                                  device->vendor_id));
    table_row_column(row, &DEVICE_COLUMN,
                     device_text(match, product, sizeof(product),
                                 device->device_id));
    table_row_column(row, &CLASS_COLUMN, pci_class_description(device));
    table_row_column(row, &DRIVER_COLUMN,
                     device->driver[0] ? device->driver : "-");
    return true;
}

static void print_verbose(const mg_device_info_t *device,
                          const mg_hardware_ids_t *ids)
{
    const mg_hardware_id_match_t *match = pci_id(ids, device);
    char bdf[24];

    bdf_text(device, bdf, sizeof(bdf));
    printf("%s %s\n", bdf, pci_class_description(device));
    printf("  vendor: %04x%s\n", device->vendor_id,
           match && match->vendor_found ? " (known)" : "");
    printf("  device: %04x%s\n", device->device_id,
           match && match->device_found ? " (known)" : "");
    printf("  class: %02x/%02x/%02x revision %02x\n",
           device->class_code, device->subclass, device->prog_if,
           device->revision);
    printf("  driver: %s\n", device->driver[0] ? device->driver : "-");
}

int main(int argc, char **argv)
{
    u32 count = 0;
    bool verbose = false;
    mg_result_t result;

    if (command_help_requested(argc, argv)) return command_print_help(argv[0]);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "-v") != 0)) {
        command_usage_error(argv[0], "lspci [-v]", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    verbose = argc == 2;
    result = device_query_category(MG_DEVICE_CATEGORY_PCI, pci_devices,
                                   PCI_SNAPSHOT_MAX, &count);
    if (result != MG_OK) {
        printf("lspci: %s\n", error_string(result));
        return 1;
    }
    hardware_ids_init(&pci_ids);
    for (u32 index = 0; index < count; index++)
        (void)hardware_ids_add(&pci_ids, pci_devices[index].vendor_id,
                               pci_devices[index].device_id);
    (void)hardware_ids_load("/share/hardware/pci.ids", &pci_ids);
    if (verbose) {
        for (u32 index = 0; index < count; index++) {
            if (index) putchar('\n');
            print_verbose(&pci_devices[index], &pci_ids);
        }
        if (!count) puts("No PCI devices.");
        return 0;
    }
    table_init(&pci_table, pci_rows, MG_TABLE_MAX_ROWS);
    if (!append_header(&pci_table)) return 1;
    for (u32 index = 0; index < count; index++)
        if (!append_device(&pci_table, &pci_devices[index], &pci_ids)) {
            printf("lspci: table is too large.\n");
            return 1;
        }
    if (!table_render(&pci_table)) {
        printf("lspci: invalid table data.\n");
        return 1;
    }
    return 0;
}
