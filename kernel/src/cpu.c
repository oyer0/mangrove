/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cpu.h>
#include <acpi.h>
#include <heap.h>
#include <kprint.h>
#include <string.h>

static cpu_local_t *cpus;
static u32 discovered_cpu_count;
static u32 online_cpu_count;
static cpu_local_t *current_cpu;

static void cpu_record_reset(cpu_local_t *cpu, u32 index,
                             const acpi_cpu_t *topology, bool bsp,
                             bool online)
{
    cpu->index = index;
    cpu->apic_id = topology->apic_id;
    cpu->present = true;
    cpu->bsp = bsp;
    cpu->online = online;
}

bool cpu_init_bsp(void)
{
    const acpi_cpu_t *bsp = acpi_bsp_cpu();
    u32 topology_count = acpi_cpu_count();
    u32 next_index = 1;

    if (!bsp || !bsp->usable || !bsp->bsp || !topology_count ||
        topology_count > CPU_MAX_COUNT)
        return false;

    cpus = (cpu_local_t *)kmalloc(
        (usize)topology_count * sizeof(*cpus));
    if (!cpus)
        return false;
    memset(cpus, 0, (usize)topology_count * sizeof(*cpus));

    /* Give the BSP index zero regardless of MADT entry order. */
    cpu_record_reset(&cpus[0], 0, bsp, true, true);

    for (u32 i = 0; i < topology_count; i++) {
        const acpi_cpu_t *topology = acpi_cpu(i);

        if (!topology || !topology->usable || topology == bsp)
            continue;
        if (next_index >= CPU_MAX_COUNT)
            return false;

        /* AP records are topology records only at this stage. They are
         * present for stable lookup, but remain offline until AP startup. */
        cpu_record_reset(&cpus[next_index], next_index, topology,
                         false, false);
        next_index++;
    }

    if (next_index != topology_count)
        return false;

    discovered_cpu_count = topology_count;
    online_cpu_count = 1;
    current_cpu = &cpus[0];

    KERNEL_BOOT_DEBUG_LOG(
        "[CPU] BSP index=%u APIC=%u online=%u discovered=%u\n",
        current_cpu->index, (u32)current_cpu->apic_id,
        online_cpu_count, discovered_cpu_count);
    return true;
}

cpu_local_t *cpu_current(void)
{
    return current_cpu;
}

u32 cpu_current_index(void)
{
    return current_cpu ? current_cpu->index : ~(u32)0;
}

cpu_local_t *cpu_by_index(u32 index)
{
    if (index >= discovered_cpu_count)
        return (cpu_local_t *)0;
    return &cpus[index];
}

cpu_local_t *cpu_by_apic_id(u8 apic_id)
{
    for (u32 i = 0; i < discovered_cpu_count; i++) {
        if (cpus[i].apic_id == apic_id)
            return &cpus[i];
    }
    return (cpu_local_t *)0;
}

u32 cpu_count(void)
{
    return discovered_cpu_count;
}

u32 cpu_online_count(void)
{
    return online_cpu_count;
}
