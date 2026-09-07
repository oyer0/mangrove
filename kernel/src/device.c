/* SPDX-License-Identifier: GPL-3.0-only */
#include <device.h>

#include <block.h>
#include <net/net.h>
#include <mg/net.h>
#include <pci.h>
#include <storage/gpt.h>
#include <string.h>
#include <xhci.h>
#include <vfs.h>

#define DEVICE_SNAPSHOT_MAX 384U
#define DEVICE_DISK_DISPLAY_MAX 128U
#define PCI_ID_BASE         0x1000000000000000ULL
#define NETWORK_ID_BASE     0x3000000000000000ULL
#define XHCI_USB_CLASS_HID       (1U << 0)
#define XHCI_USB_CLASS_STORAGE   (1U << 1)
#define XHCI_USB_CLASS_HUB       (1U << 2)

static mg_device_info_t snapshot[DEVICE_SNAPSHOT_MAX];
static u32 snapshot_count;
static u64 snapshot_generation;

typedef struct {
    u64 internal_id;
    u32 display_number;
} disk_display_entry_t;

/* Display numbers are assigned once per internal block-device instance and
 * retained for the boot.  Removed devices therefore never donate a name to a
 * later device. */
static disk_display_entry_t disk_displays[DEVICE_DISK_DISPLAY_MAX];
static u32 disk_display_count;
static u32 next_disk_display_number = 1U;

static void copy_text(char *out, usize capacity, const char *text)
{
    if (!out || !capacity) return;
    out[0] = '\0';
    if (!text) return;
    strncpy(out, text, capacity - 1U);
    out[capacity - 1U] = '\0';
}

static u64 block_id(const block_device_t *device)
{
    return device ? BLOCK_DEVICE_ID_BASE | (device->id + 1ULL) : 0;
}

static bool block_is_partition(const block_device_t *device)
{
    return device && device->type == BLOCK_DEVICE_PARTITION;
}

static bool block_has_partition(const block_device_t *device)
{
    if (!device) return false;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *child = block_get_device(index);
        gpt_partition_info_t partition;

        if (child && block_is_partition(child) &&
            gpt_get_partition_info(child, &partition) &&
            partition.parent && partition.parent->id == device->id) return true;
    }
    return false;
}

static bool block_has_system_partition(const block_device_t *device)
{
    if (!device) return false;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *child = block_get_device(index);
        gpt_partition_info_t partition;

        if (!child || !block_is_partition(child) ||
            !gpt_get_partition_info(child, &partition) || !partition.parent ||
            partition.parent->id != device->id) continue;
        if (!strcmp(partition.name, GPT_MANGROVE_ROOT_NAME) ||
            !strcmp(partition.name, GPT_MANGROVE_BOOT_NAME)) return true;
    }
    return false;
}

static u32 disk_display_number(u64 internal_id)
{
    if (!internal_id) return 0;
    for (u32 index = 0; index < disk_display_count; index++)
        if (disk_displays[index].internal_id == internal_id)
            return disk_displays[index].display_number;
    if (disk_display_count >= DEVICE_DISK_DISPLAY_MAX ||
        next_disk_display_number == 0U)
        return 0;
    disk_displays[disk_display_count].internal_id = internal_id;
    disk_displays[disk_display_count].display_number =
        next_disk_display_number++;
    disk_display_count++;
    return disk_displays[disk_display_count - 1U].display_number;
}

static bool filter_matches(u32 filter, u32 category)
{
    if (filter == MG_DEVICE_CATEGORY_ALL) return true;
    if (filter == MG_DEVICE_FILTER_BLOCKS)
        return category == MG_DEVICE_CATEGORY_BLOCK ||
               category == MG_DEVICE_CATEGORY_PARTITION;
    return filter == category;
}

static char hex_digit(u8 value)
{
    return value < 10U ? (char)('0' + value) : (char)('a' + value - 10U);
}

static void format_pci_name(char *out, usize capacity, const pci_device_t *device)
{
    if (!out || capacity < 12U || !device) return;
    out[0] = 'P'; out[1] = 'C'; out[2] = 'I'; out[3] = ' ';
    out[4] = hex_digit((u8)(device->bus >> 4));
    out[5] = hex_digit(device->bus & 0xfU);
    out[6] = ':';
    out[7] = hex_digit((u8)(device->device >> 4));
    out[8] = hex_digit(device->device & 0xfU);
    out[9] = '.';
    out[10] = (char)('0' + device->function);
    out[11] = '\0';
}

static void add_snapshot(const mg_device_info_t *info)
{
    if (!info || snapshot_count >= DEVICE_SNAPSHOT_MAX) return;
    snapshot[snapshot_count++] = *info;
}

static void add_pci_devices(void)
{
    u32 count = pci_get_device_count();

    for (u32 index = 0; index < count; index++) {
        const pci_device_t *device = pci_get_device(index);
        mg_device_info_t info = {0};
        if (!device) continue;
        /* The PCI slot index is only an array position and can be reused by
         * a hot-added function.  Use the boot-local PCI generation so a
         * stale inspection ID cannot silently select a replacement device. */
        info.id = PCI_ID_BASE | device->generation;
        info.category = MG_DEVICE_CATEGORY_PCI;
        info.state = MG_DEVICE_STATE_PRESENT;
        info.bus = device->bus;
        info.slot = device->device;
        info.function = device->function;
        info.vendor_id = device->vendor_id;
        info.device_id = device->device_id;
        info.class_code = device->class_code;
        info.subclass = device->subclass;
        info.prog_if = device->prog_if;
        info.revision = device->revision;
        format_pci_name(info.name, sizeof(info.name), device);
        if (device->vendor_id == 0x8086U &&
            (device->device_id == 0x100eU || device->device_id == 0x100fU))
            copy_text(info.driver, sizeof(info.driver), "e1000");
        else if (device->vendor_id == 0x10ecU && device->device_id == 0x8168U)
            copy_text(info.driver, sizeof(info.driver), "rtl8168");
        else if (device->class_code == 0x0cU && device->subclass == 0x03U &&
                 device->prog_if == 0x30U)
            copy_text(info.driver, sizeof(info.driver), "xhci");
        else if (device->class_code == 0x01U && device->subclass == 0x06U)
            copy_text(info.driver, sizeof(info.driver), "ahci");
        else
            copy_text(info.driver, sizeof(info.driver), "-");
        add_snapshot(&info);
    }
}

static void fill_block_common(mg_device_info_t *info,
                              const block_device_t *device)
{
    u64 size = 0;
    if (!info || !device) return;
    info->id = block_id(device);
    info->state = MG_DEVICE_STATE_PRESENT;
    info->block_size = device->sector_size;
    if (device->read_only) info->flags |= MG_DEVICE_FLAG_READ_ONLY;
    copy_text(info->connection, sizeof(info->connection), device->connection);
    copy_text(info->model, sizeof(info->model), device->model);
    if (!info->connection[0]) {
        const char *connection = NULL;
        switch (device->type) {
            case BLOCK_DEVICE_USB: connection = "usb"; break;
            case BLOCK_DEVICE_SATA: connection = "sata"; break;
            case BLOCK_DEVICE_NVME: connection = "nvme"; break;
            case BLOCK_DEVICE_RAM: connection = "virt"; break;
            default: break;
        }
        copy_text(info->connection, sizeof(info->connection), connection);
    }
    if (device->sector_size && device->sector_count <=
        (~(u64)0 / device->sector_size))
        size = device->sector_count * device->sector_size;
    info->size_bytes = size;
}

static void add_block_devices(void)
{
    u32 count = block_device_count();

    for (u32 index = 0; index < count; index++) {
        block_device_t *device = block_get_device(index);
        mg_device_info_t info = {0};
        gpt_partition_info_t partition;
        const char *type_name;
        const char *filesystem;

        if (!device) continue;
        fill_block_common(&info, device);
        if (block_is_partition(device)) {
            info.category = MG_DEVICE_CATEGORY_PARTITION;
            if (gpt_get_partition_info(device, &partition)) {
                info.parent_id = block_id(partition.parent);
                if (partition.parent &&
                    partition.parent->type == BLOCK_DEVICE_USB)
                    info.flags |= MG_DEVICE_FLAG_REMOVABLE;
                info.disk_number = (u16)disk_display_number(info.parent_id);
                info.partition_number = (u16)(partition.number <= 0xffffU
                    ? partition.number : 0U);
                info.first_lba = partition.first_lba;
                info.last_lba = partition.last_lba;
                copy_text(info.name, sizeof(info.name), partition.name);
                copy_text(info.role, sizeof(info.role), partition.name);
                if (!strcmp(partition.name, GPT_MANGROVE_ROOT_NAME)) {
                    info.flags |= MG_DEVICE_FLAG_ROOT;
                    info.flags |= MG_DEVICE_FLAG_SYSTEM_MANAGED;
                    copy_text(info.filesystem, sizeof(info.filesystem), "MGFS");
                } else if (!strcmp(partition.name, GPT_MANGROVE_BOOT_NAME) &&
                           gpt_partition_is_esp(&partition)) {
                    info.flags |= MG_DEVICE_FLAG_BOOT;
                    info.flags |= MG_DEVICE_FLAG_SYSTEM_MANAGED;
                    copy_text(info.filesystem, sizeof(info.filesystem), "FAT32");
                }
                {
                    const char *filesystem = vfs_probe_filesystem(device);
                    if (filesystem) {
                        copy_text(info.filesystem, sizeof(info.filesystem),
                                  filesystem);
                        (void)vfs_filesystem_label(
                            device, filesystem, info.label,
                            sizeof(info.label));
                    }
                }
            }
            if (!info.name[0]) copy_text(info.name, sizeof(info.name),
                                         "partition");
            copy_text(info.driver, sizeof(info.driver), "partition");
        } else {
            info.category = MG_DEVICE_CATEGORY_BLOCK;
            info.disk_number = (u16)disk_display_number(info.id);
            if (device->type == BLOCK_DEVICE_USB)
                info.flags |= MG_DEVICE_FLAG_REMOVABLE;
            if (block_has_system_partition(device))
                info.flags |= MG_DEVICE_FLAG_SYSTEM_MANAGED;
            type_name = block_type_name(device->type);
            copy_text(info.name, sizeof(info.name), type_name);
            copy_text(info.driver, sizeof(info.driver),
                      device->type == BLOCK_DEVICE_USB ? "usb-storage" :
                      "block");
            /* A removable device may contain a filesystem directly on the
             * whole disk.  Probe it only when no registered partition child
             * exists, so a partitioned disk cannot be mounted twice. */
            if (!block_has_partition(device)) {
                filesystem = vfs_probe_filesystem(device);
                if (filesystem) {
                    copy_text(info.filesystem, sizeof(info.filesystem),
                              filesystem);
                    (void)vfs_filesystem_label(
                        device, filesystem, info.label, sizeof(info.label));
                }
            }
        }
        if (block_device_automount_suppressed(device->id))
            info.flags |= MG_DEVICE_FLAG_AUTOMOUNT_SUPPRESSED;
        /* Mount metadata belongs to the exact block-device instance, not to
         * the partition-discovery branch.  In particular, removable media
         * may contain a filesystem directly on the whole disk. */
        if (vfs_mount_point_for_device(device, info.mount_point,
                                       sizeof(info.mount_point), NULL))
            info.flags |= MG_DEVICE_FLAG_MOUNTED;
        add_snapshot(&info);
    }
}

static const char *usb_class_name(u8 class_flags)
{
    if (class_flags == (1U << 0)) return "USB HID";
    if (class_flags == (1U << 1)) return "USB storage";
    if (class_flags == (1U << 2)) return "USB hub";
    if (class_flags) return "USB composite";
    return "USB device";
}

static const char *usb_driver_name(u8 class_flags)
{
    if ((class_flags & XHCI_USB_CLASS_HID) != 0) return "hid-keyboard";
    if ((class_flags & XHCI_USB_CLASS_STORAGE) != 0) return "usb-storage";
    if ((class_flags & XHCI_USB_CLASS_HUB) != 0) return "usb-hub";
    return "-";
}

static void usb_class_identity(const xhci_usb_device_info_t *device,
                               u8 *class_code, u8 *subclass, u8 *protocol)
{
    if (!device || !class_code || !subclass || !protocol) return;
    *class_code = device->class_code;
    *subclass = device->subclass;
    *protocol = device->protocol;
    /* USB class 0 means the class is declared by an interface descriptor.
     * The generic snapshot retains the class-driver classification so lsusb
     * can still provide a useful class fallback without inspecting xHCI. */
    if (*class_code != 0) return;
    if ((device->class_flags & XHCI_USB_CLASS_HID) != 0) {
        *class_code = 0x03;
        *subclass = 0x01;
        *protocol = 0x01;
    } else if ((device->class_flags & XHCI_USB_CLASS_STORAGE) != 0) {
        *class_code = 0x08;
        *subclass = 0x06;
        *protocol = 0x50;
    } else if ((device->class_flags & XHCI_USB_CLASS_HUB) != 0) {
        *class_code = 0x09;
        *subclass = 0;
        *protocol = 0;
    }
}

static void add_usb_devices(void)
{
    xhci_usb_device_info_t devices[32];
    u32 count = xhci_usb_device_snapshot(devices,
                                         sizeof(devices) / sizeof(devices[0]));

    if (count > sizeof(devices) / sizeof(devices[0]))
        count = sizeof(devices) / sizeof(devices[0]);
    for (u32 index = 0; index < count; index++) {
        mg_device_info_t info = {0};
        /* xHCI slot IDs are deliberately reused.  Publish the monotonic
         * instance generation so a stale snapshot cannot identify a later
         * device attached to the same port/slot. */
        info.id = XHCI_USB_DEVICE_ID_BASE | devices[index].instance_generation;
        info.category = MG_DEVICE_CATEGORY_USB;
        /* Only a fully configured instance is published.  A failed or
         * partially torn-down enumeration remains kernel-private and must not
         * become a ghost entry in userspace snapshots. */
        info.state = MG_DEVICE_STATE_PRESENT;
        info.vendor_id = devices[index].vendor_id;
        info.device_id = devices[index].product_id;
        /* Mangrove currently has one xHCI controller.  Keep the native
         * topology visible without inventing a Linux bus numbering scheme:
         * bus 1 is the controller and slot is the root port. */
        info.bus = 1;
        info.slot = devices[index].port_id;
        info.function = devices[index].slot_id;
        usb_class_identity(&devices[index], &info.class_code,
                           &info.subclass, &info.prog_if);
        info.usb_speed = devices[index].speed;
        copy_text(info.name, sizeof(info.name),
                  usb_class_name(devices[index].class_flags));
        copy_text(info.driver, sizeof(info.driver),
                  usb_driver_name(devices[index].class_flags));
        add_snapshot(&info);
    }
}

static void add_network_devices(void)
{
    mg_net_interface_info_t interfaces[NET_MAX_DEVICES];
    usize count = 0;

    if (!net_fill_interface(interfaces, sizeof(interfaces), &count)) return;
    if (count > NET_MAX_DEVICES) count = NET_MAX_DEVICES;
    for (usize index = 0; index < count; index++) {
        mg_device_info_t info = {0};
        info.id = interfaces[index].id ? interfaces[index].id :
                  NETWORK_ID_BASE | (u64)(index + 1U);
        info.category = MG_DEVICE_CATEGORY_NETWORK;
        info.state = MG_DEVICE_STATE_PRESENT;
        copy_text(info.name, sizeof(info.name), interfaces[index].name);
        copy_text(info.driver, sizeof(info.driver), interfaces[index].type);
        add_snapshot(&info);
    }
}

void device_snapshot_refresh(void)
{
    snapshot_count = 0;
    add_pci_devices();
    add_usb_devices();
    add_block_devices();
    add_network_devices();
    snapshot_generation++;
    if (snapshot_generation == 0) snapshot_generation = 1;
}

mg_result_t device_snapshot_read(u32 filter, u64 device_id, u32 offset,
                                 u64 requested_generation,
                                 mg_device_info_t *output, u32 capacity,
                                 u32 *out_total, u64 *out_generation)
{
    u32 total = 0;
    u32 copied = 0;

    if (!output || !out_total || capacity == 0) return MG_ERR_BAD_ARGUMENT;
    if (!requested_generation) {
        device_snapshot_refresh();
    } else if (requested_generation != snapshot_generation) {
        return MG_ERR_RETRY;
    }
    for (u32 index = 0; index < snapshot_count; index++) {
        mg_device_info_t *info = &snapshot[index];
        if (!filter_matches(filter, info->category) ||
            (device_id && info->id != device_id)) continue;
        if (total >= offset && copied < capacity)
            output[copied++] = *info;
        total++;
    }
    *out_total = total;
    if (out_generation) *out_generation = snapshot_generation;
    return copied;
}
