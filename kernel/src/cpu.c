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
static char cpu_model[MG_INSPECTION_CPU_MODEL_MAX];

static void cpu_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx,
                      u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static void cpu_capture_model(void)
{
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 max_extended;
    u32 words[12];
    char raw[49];
    usize start;
    usize end;
    usize length;

    memset(cpu_model, 0, sizeof(cpu_model));
    cpu_cpuid(0x80000000U, 0, &max_extended, &ebx, &ecx, &edx);
    if (max_extended < 0x80000004U)
        return;

    for (u32 leaf = 0; leaf < 3U; leaf++) {
        cpu_cpuid(0x80000002U + leaf, 0,
                  &words[leaf * 4U], &words[leaf * 4U + 1U],
                  &words[leaf * 4U + 2U], &words[leaf * 4U + 3U]);
    }
    memcpy(raw, words, sizeof(words));
    raw[sizeof(raw) - 1U] = '\0';

    start = 0;
    while (raw[start] == ' ') start++;
    end = sizeof(raw) - 1U;
    while (end > start && raw[end - 1U] == ' ') end--;
    length = end - start;
    if (length >= sizeof(cpu_model)) length = sizeof(cpu_model) - 1U;
    if (length) memcpy(cpu_model, raw + start, length);
    cpu_model[length] = '\0';
}

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
    cpu_capture_model();

    KERNEL_BOOT_DEBUG_LOG(
        "[CPU] BSP index=%u APIC=%u online=%u discovered=%u\n",
        cpus[0].index, (u32)cpus[0].apic_id,
        online_cpu_count, discovered_cpu_count);
    return true;
}

void cpu_mark_online(cpu_local_t *cpu)
{
    if (!cpu || !cpu->present)
        return;
    if (!__atomic_exchange_n(&cpu->online, true, __ATOMIC_ACQ_REL))
        __atomic_add_fetch(&online_cpu_count, 1U, __ATOMIC_RELAXED);
}

void cpu_mark_offline(cpu_local_t *cpu)
{
    if (!cpu || !cpu->present)
        return;
    if (__atomic_exchange_n(&cpu->online, false, __ATOMIC_ACQ_REL))
        __atomic_sub_fetch(&online_cpu_count, 1U, __ATOMIC_RELAXED);
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
    return __atomic_load_n(&online_cpu_count, __ATOMIC_ACQUIRE);
}

bool cpu_model_copy(char *output, usize capacity)
{
    usize length;

    if (!output || capacity == 0U || !cpu_model[0])
        return false;
    length = strlen(cpu_model);
    if (length >= capacity) length = capacity - 1U;
    memcpy(output, cpu_model, length);
    output[length] = '\0';
    return true;
}

u32 cpu_snapshot_read(u32 offset, mg_cpu_info_t *output,
                      u32 capacity, u32 *out_total)
{
    u32 total = discovered_cpu_count;
    u32 copied = 0;

    if (!output || !capacity || capacity > MG_CPU_SNAPSHOT_PAGE_MAX ||
        !out_total)
        return 0;

    for (u32 index = offset; index < total && copied < capacity; index++) {
        cpu_local_t *cpu = cpu_by_index(index);
        mg_cpu_info_t info;

        if (!cpu)
            continue;
        memset(&info, 0, sizeof(info));
        info.index = cpu->index;
        info.apic_id = cpu->apic_id;
        if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE))
            info.flags |= MG_CPU_FLAG_ONLINE;
        if (cpu->bsp)
            info.flags |= MG_CPU_FLAG_BSP;
        info.total_ticks = __atomic_load_n(&cpu->scheduler_accounted_ticks,
                                           __ATOMIC_RELAXED);
        info.busy_ticks = __atomic_load_n(&cpu->scheduler_busy_ticks,
                                          __ATOMIC_RELAXED);
        output[copied++] = info;
    }
    *out_total = total;
    return copied;
}
