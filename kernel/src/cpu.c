/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cpu.h>
#include <acpi.h>
#include <heap.h>
#include <kprint.h>
#include <msr.h>
#include <string.h>

static cpu_local_t *cpus;
static u32 discovered_cpu_count;
static u32 online_cpu_count;
static cpu_local_t bootstrap_cpu;
static bool bootstrap_ready;

cpu_local_t *cpu_bootstrap_local(void)
{
    if (!bootstrap_ready) {
        memset(&bootstrap_cpu, 0, sizeof(bootstrap_cpu));
        bootstrap_cpu.self = &bootstrap_cpu;
        bootstrap_cpu.index = 0;
        bootstrap_cpu.present = true;
        bootstrap_cpu.bsp = true;
        bootstrap_cpu.online = true;
        bootstrap_ready = true;
    }
    return &bootstrap_cpu;
}

bool cpu_activate_kernel_gs(cpu_local_t *cpu)
{
    if (!cpu || cpu->self != cpu)
        return false;

    /* Establish the pair while the kernel is active.  Writing the user side
     * first, clearing the current side, then swapping makes this safe without
     * depending on whatever GS base firmware left behind. */
    wrmsr(MSR_IA32_KERNEL_GS_BASE, (u64)(uintptr_t)cpu);
    wrmsr(MSR_IA32_GS_BASE, 0);
    __asm__ volatile("swapgs" ::: "memory");
    return cpu_current() == cpu;
}

static void cpu_record_reset(cpu_local_t *cpu, u32 index,
                             const acpi_cpu_t *topology, bool bsp,
                             bool online)
{
    cpu->self = cpu;
    cpu->index = index;
    cpu->apic_id = topology->apic_id;
    cpu->present = true;
    cpu->bsp = bsp;
    cpu->online = online;
}

bool cpu_init_bsp(void)
{
    cpu_local_t *bootstrap = cpu_bootstrap_local();
    const acpi_cpu_t *bsp = acpi_bsp_cpu();
    u32 topology_count = acpi_cpu_count();
    u32 next_index = 1;

    if (!bootstrap->descriptor || !bsp || !bsp->usable || !bsp->bsp ||
        !topology_count ||
        topology_count > CPU_MAX_COUNT)
        return false;

    cpus = (cpu_local_t *)kmalloc(
        (usize)topology_count * sizeof(*cpus));
    if (!cpus)
        return false;
    memset(cpus, 0, (usize)topology_count * sizeof(*cpus));

    /* Give the BSP index zero regardless of MADT entry order, preserving the
     * descriptor state that was already loaded during early bootstrap. */
    cpus[0] = *bootstrap;
    cpu_record_reset(&cpus[0], 0, bsp, true, true);
    cpus[0].descriptor = bootstrap->descriptor;

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
    if (!cpu_activate_kernel_gs(&cpus[0]))
        return false;

    KERNEL_BOOT_DEBUG_LOG(
        "[CPU] BSP index=%u APIC=%u online=%u discovered=%u\n",
        cpus[0].index, (u32)cpus[0].apic_id,
        online_cpu_count, discovered_cpu_count);
    return true;
}

cpu_local_t *cpu_current(void)
{
    cpu_local_t *cpu;

    __asm__ volatile("movq %%gs:0, %0" : "=r"(cpu) :: "memory");
    return cpu && cpu->self == cpu ? cpu : (cpu_local_t *)0;
}

u32 cpu_current_index(void)
{
    cpu_local_t *cpu = cpu_current();
    return cpu ? cpu->index : ~(u32)0;
}

cpu_local_t *cpu_by_index(u32 index)
{
    if (!cpus || index >= discovered_cpu_count)
        return (cpu_local_t *)0;
    return &cpus[index];
}

cpu_local_t *cpu_by_apic_id(u8 apic_id)
{
    if (!cpus)
        return (cpu_local_t *)0;
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
