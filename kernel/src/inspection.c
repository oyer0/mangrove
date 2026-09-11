/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <inspection.h>

#include <cpu.h>
#include <framebuffer.h>
#include <pci.h>
#include <pci_class.h>
#include <pci_vendor.h>
#include <string.h>

static void copy_text(char *output, usize capacity, const char *text)
{
    usize length;

    if (!output || capacity == 0U) return;
    output[0] = '\0';
    if (!text) return;
    length = strlen(text);
    if (length >= capacity) length = capacity - 1U;
    if (length) memcpy(output, text, length);
    output[length] = '\0';
}

static void append_text(char *output, usize capacity, const char *text)
{
    usize used;
    usize length;

    if (!output || capacity == 0U || !text) return;
    used = strlen(output);
    if (used >= capacity - 1U) return;
    length = strlen(text);
    if (length > capacity - 1U - used)
        length = capacity - 1U - used;
    if (length) memcpy(output + used, text, length);
    output[used + length] = '\0';
}

static bool known_pci_name(const char *name)
{
    return name && strncmp(name, "Unknown ", 8U) != 0;
}

static void display_adapter_name(char *output, usize capacity,
                                 const pci_device_t *device)
{
    const char *vendor;
    const char *product;

    if (!output || capacity == 0U || !device) return;
    vendor = pci_vendor_name(device->vendor_id);
    product = pci_device_name(device->vendor_id, device->device_id);
    if (known_pci_name(vendor) && known_pci_name(product)) {
        copy_text(output, capacity, vendor);
        append_text(output, capacity, " ");
        append_text(output, capacity, product);
    } else if (known_pci_name(vendor)) {
        copy_text(output, capacity, vendor);
        append_text(output, capacity, " display adapter");
    } else if (known_pci_name(product)) {
        copy_text(output, capacity, product);
    } else {
        copy_text(output, capacity, "PCI display adapter");
    }
}

void system_info_read(mg_system_info_t *output)
{
    u32 device_count;

    if (!output) return;
    memset(output, 0, sizeof(*output));
    (void)cpu_model_copy(output->cpu_model, sizeof(output->cpu_model));
    output->logical_cpu_count = cpu_online_count();
    output->display_width = framebuffer_width();
    output->display_height = framebuffer_height();

    device_count = pci_get_device_count();
    for (u32 index = 0; index < device_count; index++) {
        const pci_device_t *device = pci_get_device(index);
        mg_gpu_info_t *gpu;

        if (!device || device->class_code != PCI_CLASS_DISPLAY) continue;
        output->gpu_total++;
        if (output->gpu_count >= MG_INSPECTION_GPU_MAX) continue;
        gpu = &output->gpus[output->gpu_count++];
        gpu->vendor_id = device->vendor_id;
        gpu->device_id = device->device_id;
        gpu->class_code = device->class_code;
        gpu->subclass = device->subclass;
        gpu->prog_if = device->prog_if;
        display_adapter_name(gpu->name, sizeof(gpu->name), device);
    }
}
