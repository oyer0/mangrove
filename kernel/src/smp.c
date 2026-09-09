/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <smp.h>

#include <acpi.h>
#include <cpu.h>
#include <cpu_relax.h>
#include <gdt.h>
#include <heap.h>
#include <kprint.h>
#include <lapic.h>
#include <pmm.h>
#include <string.h>
#include <timer.h>
#include <vmm.h>

#define SMP_AP_START_TIMEOUT_US       100000ULL
#define SMP_AP_RETRY_TIMEOUT_US       500000ULL
#define SMP_FALLBACK_WAIT_SPINS       50000000U

typedef struct {
    cpu_local_t *cpu;
    void *stack_base;
    uintptr_t stack_top;
    volatile u32 state;
} smp_ap_runtime_t;

static smp_ap_runtime_t ap_runtime[CPU_MAX_COUNT];
static volatile smp_trampoline_mailbox_t *trampoline_mailbox;
static phys_addr_t trampoline_physical;
static u8 trampoline_vector;
static bool trampoline_mapped;

extern char __smp_trampoline_start[];
extern char __smp_trampoline_end[];

static void smp_store_state(volatile u32 *state,
                            smp_ap_mailbox_state_t value)
{
    __atomic_store_n(state, (u32)value, __ATOMIC_RELEASE);
}

static smp_ap_mailbox_state_t smp_load_state(const volatile u32 *state)
{
    return (smp_ap_mailbox_state_t)
        __atomic_load_n(state, __ATOMIC_ACQUIRE);
}

static bool smp_wait_for_online(smp_ap_runtime_t *runtime, u64 timeout_us)
{
    timer_monotonic_deadline_t deadline;

    if (!runtime)
        return false;

    if (timer_monotonic_deadline_start(&deadline, timeout_us)) {
        while (!timer_monotonic_deadline_expired(&deadline)) {
            smp_ap_mailbox_state_t state = smp_load_state(&runtime->state);
            if (state == SMP_AP_MAILBOX_ONLINE)
                return true;
            if (state == SMP_AP_MAILBOX_FAILED)
                return false;
            cpu_relax();
        }
        return smp_load_state(&runtime->state) == SMP_AP_MAILBOX_ONLINE;
    }

    for (u32 i = 0; i < SMP_FALLBACK_WAIT_SPINS; i++) {
        smp_ap_mailbox_state_t state = smp_load_state(&runtime->state);
        if (state == SMP_AP_MAILBOX_ONLINE)
            return true;
        if (state == SMP_AP_MAILBOX_FAILED)
            return false;
        cpu_relax();
    }
    return smp_load_state(&runtime->state) == SMP_AP_MAILBOX_ONLINE;
}

static void smp_mark_failed(cpu_local_t *cpu, smp_ap_runtime_t *runtime,
                            const char *reason)
{
    if (runtime)
        smp_store_state(&runtime->state, SMP_AP_MAILBOX_FAILED);
    if (cpu)
        cpu_mark_offline(cpu);
    KERNEL_BOOT_DEBUG_LOG("[SMP] APIC %u startup failed: %s\n",
                          cpu ? (u32)cpu->apic_id : 0U,
                          reason ? reason : "unknown error");
}

static bool smp_prepare_trampoline(void)
{
    phys_addr_t candidate = pmm_smp_trampoline_candidate();
    uintptr_t source_start = (uintptr_t)__smp_trampoline_start;
    uintptr_t source_end = (uintptr_t)__smp_trampoline_end;
    usize source_size;
    void *destination;

    if (!candidate || (candidate & (SMP_TRAMPOLINE_PAGE_SIZE - 1ULL)) ||
        candidate >= SMP_TRAMPOLINE_VGA_LIMIT ||
        candidate > ~(u64)0 - SMP_TRAMPOLINE_PAGE_SIZE ||
        candidate + SMP_TRAMPOLINE_PAGE_SIZE > SMP_TRAMPOLINE_LOW_LIMIT) {
        KERNEL_BOOT_DEBUG_LOG(
            "[SMP] no safe low-memory trampoline page is available\n");
        return false;
    }
    if (source_end <= source_start)
        return false;
    source_size = (usize)(source_end - source_start);
    if (source_size > SMP_TRAMPOLINE_MAILBOX_OFFSET ||
        (candidate >> 12) > 0xFFULL) {
        KERNEL_BOOT_DEBUG_LOG("[SMP] trampoline does not fit its SIPI page\n");
        return false;
    }
    if (!pmm_reserve_range(candidate, 1)) {
        KERNEL_BOOT_DEBUG_LOG("[SMP] failed to reserve trampoline page\n");
        return false;
    }
    if (!vmm_map_bootstrap_page(candidate)) {
        KERNEL_BOOT_DEBUG_LOG("[SMP] failed to map trampoline page\n");
        return false;
    }

    destination = phys_to_virt(candidate);
    if (!destination) {
        (void)vmm_unmap_bootstrap_page(candidate);
        return false;
    }
    memset(destination, 0, SMP_TRAMPOLINE_PAGE_SIZE);
    memcpy(destination, (const void *)source_start, source_size);

    trampoline_mailbox = (volatile smp_trampoline_mailbox_t *)
        ((u8 *)destination + SMP_TRAMPOLINE_MAILBOX_OFFSET);
    trampoline_physical = candidate;
    trampoline_vector = (u8)(candidate >> 12);
    trampoline_mapped = true;
    KERNEL_BOOT_DEBUG_LOG(
        "[SMP] AP trampoline physical=%p vector=%x size=%u\n",
        (void *)(uintptr_t)candidate, (u32)trampoline_vector,
        (u32)source_size);
    return true;
}

static bool smp_prepare_ap(cpu_local_t *cpu, smp_ap_runtime_t *runtime)
{
    void *stack;
    gdt_cpu_state_t *descriptor;
    uintptr_t stack_address;

    if (!cpu || !runtime || cpu->bsp || !cpu->present || cpu->online)
        return false;
    descriptor = (gdt_cpu_state_t *)kmalloc(sizeof(*descriptor));
    stack = kmalloc(SMP_AP_STACK_SIZE);
    if (!descriptor || !stack) {
        if (descriptor)
            kfree(descriptor);
        if (stack)
            kfree(stack);
        return false;
    }

    memset(descriptor, 0, sizeof(*descriptor));
    memset(stack, 0, SMP_AP_STACK_SIZE);
    stack_address = (uintptr_t)stack + SMP_AP_STACK_SIZE;
    stack_address &= ~(uintptr_t)0xFULL;
    if (stack_address < (uintptr_t)stack + 8U) {
        kfree(descriptor);
        kfree(stack);
        return false;
    }

    cpu->descriptor = descriptor;
    runtime->cpu = cpu;
    runtime->stack_base = stack;
    runtime->stack_top = stack_address;
    smp_store_state(&runtime->state, SMP_AP_MAILBOX_PREPARED);
    return true;
}

static bool smp_start_one(cpu_local_t *cpu, u32 cpu_index)
{
    smp_ap_runtime_t *runtime;
    phys_addr_t kernel_cr3;

    if (!cpu || cpu_index >= CPU_MAX_COUNT)
        return false;
    runtime = &ap_runtime[cpu_index];
    memset(runtime, 0, sizeof(*runtime));
    if (!smp_prepare_ap(cpu, runtime)) {
        smp_mark_failed(cpu, runtime, "resource allocation failed");
        return false;
    }

    kernel_cr3 = vmm_get_kernel_pml4_phys();
    if (!kernel_cr3 || (kernel_cr3 & (SMP_TRAMPOLINE_PAGE_SIZE - 1ULL)) ||
        kernel_cr3 > 0xFFFFFFFFULL) {
        gdt_cpu_state_t *descriptor = runtime->cpu->descriptor;
        smp_mark_failed(cpu, runtime, "kernel page table is not SIPI-accessible");
        cpu->descriptor = 0;
        kfree(runtime->stack_base);
        kfree(descriptor);
        runtime->stack_base = 0;
        return false;
    }

    /* The low mailbox is serialized.  A timed-out AP is never followed by
     * another mailbox handoff, so a late AP cannot consume another target's
     * startup data. */
    memset((void *)trampoline_mailbox, 0, sizeof(*trampoline_mailbox));
    trampoline_mailbox->cpu_index = cpu_index;
    trampoline_mailbox->apic_id = cpu->apic_id;
    trampoline_mailbox->cr3 = kernel_cr3;
    trampoline_mailbox->stack_top = runtime->stack_top;
    trampoline_mailbox->entry = (u64)(uintptr_t)&smp_ap_entry;
    trampoline_mailbox->cpu_local = (u64)(uintptr_t)cpu;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    smp_store_state(&trampoline_mailbox->state, SMP_AP_MAILBOX_PREPARED);

    if (!lapic_send_init_ipi(cpu->apic_id)) {
        smp_mark_failed(cpu, runtime, "INIT IPI delivery timeout");
        return false;
    }
    smp_store_state(&runtime->state, SMP_AP_MAILBOX_STARTED);
    if (!lapic_send_startup_ipi(cpu->apic_id, trampoline_vector)) {
        smp_mark_failed(cpu, runtime, "first SIPI delivery timeout");
        return false;
    }
    if (smp_wait_for_online(runtime, SMP_AP_START_TIMEOUT_US))
        return true;

    KERNEL_BOOT_DEBUG_LOG("[SMP] APIC %u retrying SIPI\n",
                          (u32)cpu->apic_id);
    if (!lapic_send_startup_ipi(cpu->apic_id, trampoline_vector) ||
        !smp_wait_for_online(runtime, SMP_AP_RETRY_TIMEOUT_US)) {
        smp_mark_failed(cpu, runtime, "AP online timeout");
        return false;
    }
    return true;
}

bool smp_start(void)
{
    u32 discovered = cpu_count();
    bool all_started = true;

    if (!cpu_current() || !cpu_current()->bsp)
        return false;
    if (acpi_unsupported_x2apic_count()) {
        KERNEL_BOOT_DEBUG_LOG(
            "[SMP] %u x2APIC processor entr%s unsupported\n",
            acpi_unsupported_x2apic_count(),
            acpi_unsupported_x2apic_count() == 1U ? "y" : "ies");
    }
    if (discovered <= 1U) {
        KERNEL_BOOT_DEBUG_LOG("[SMP] discovered=%u online=%u\n",
                              discovered, cpu_online_count());
        return true;
    }
    if (!smp_prepare_trampoline())
        return false;

    for (u32 index = 1; index < discovered; index++) {
        cpu_local_t *cpu = cpu_by_index(index);
        if (!cpu || cpu->bsp || !cpu->present) {
            all_started = false;
            break;
        }
        if (!smp_start_one(cpu, index)) {
            all_started = false;
            /* Do not reuse the serialized low mailbox after a timeout. */
            break;
        }
        KERNEL_BOOT_DEBUG_LOG("[SMP] APIC %u online (cpu%u)\n",
                              (u32)cpu->apic_id, index);
    }

    if (all_started && trampoline_mapped) {
        if (vmm_unmap_bootstrap_page(trampoline_physical)) {
            trampoline_mapped = false;
            trampoline_mailbox = NULL;
        } else {
            KERNEL_BOOT_DEBUG_LOG(
                "[SMP] retaining trampoline mapping after cleanup failure\n");
        }
    }
    KERNEL_BOOT_DEBUG_LOG("[SMP] final discovered=%u online=%u\n",
                          discovered, cpu_online_count());
    return all_started;
}

void smp_ap_entry(u32 cpu_index)
{
    cpu_local_t *cpu;
    smp_ap_runtime_t *runtime;

    __asm__ volatile("cli" ::: "memory");
    if (cpu_index == 0U || cpu_index >= CPU_MAX_COUNT) {
        for (;;) cpu_relax();
    }
    cpu = cpu_by_index(cpu_index);
    runtime = &ap_runtime[cpu_index];
    if (!trampoline_mailbox ||
        smp_load_state(&trampoline_mailbox->state) !=
            SMP_AP_MAILBOX_PREPARED ||
        !cpu || !cpu->present || cpu->bsp || runtime->cpu != cpu ||
        !runtime->stack_top || !cpu->descriptor) {
        smp_store_state(&runtime->state, SMP_AP_MAILBOX_FAILED);
        for (;;) cpu_relax();
    }
    if (trampoline_mailbox->cpu_index != cpu_index ||
        trampoline_mailbox->apic_id != cpu->apic_id ||
        trampoline_mailbox->cpu_local != (u64)(uintptr_t)cpu) {
        smp_store_state(&runtime->state, SMP_AP_MAILBOX_FAILED);
        for (;;) cpu_relax();
    }

    if (!gdt_init_cpu(cpu, runtime->stack_top) ||
        !lapic_init_cpu() || lapic_current_id() != cpu->apic_id) {
        smp_store_state(&runtime->state, SMP_AP_MAILBOX_FAILED);
        for (;;) cpu_relax();
    }

    cpu->current_pml4 = vmm_get_kernel_pml4();
    cpu_mark_online(cpu);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    smp_store_state(&runtime->state, SMP_AP_MAILBOX_ONLINE);
    KERNEL_BOOT_DEBUG_LOG("[SMP] APIC %u entered idle (cpu%u)\n",
                          (u32)cpu->apic_id, cpu_index);

    for (;;) cpu_relax();
}
