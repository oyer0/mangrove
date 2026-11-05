/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <bootinfo.h>
#include <gdt.h>
#include <idt.h>
#include <cpu.h>
#include <pmm.h>
#include <memory_types.h>
#include <vmm.h>
#include <mangrove_version.h>
#include <heap.h>
#include <pic.h>
#include <pit.h>
#include <timer.h>
#include <timekeeping.h>
#include <drivers/input/keyboard.h>
#include <font.h>
#include <terminal.h>
#include <terminal_palette.h>
#include <framebuffer.h>
#include <x86_cache.h>
#include <kprint.h>
#include <console.h>
#include <pci.h>
#include <net/net.h>
#include <net/e1000.h>
#include <net/config.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/dhcp.h>
#include <net/dns.h>
#include <net/http.h>
#include <net/tcp.h>
#include <acpi.h>
#include <ec.h>
#include <lapic.h>
#include <ioapic.h>
#include <ahci.h>
#include <xhci.h>
#include <irq.h> // Ensure we can register the xHCI interrupt
#include <vfs.h>
#include <scheduler.h>
#include <initramfs.h>
#include <storage/fat32.h>
#include <storage/mgfs.h>
#include <string.h>
#include <syscall.h>
#include <process.h>
#include <identity.h>
#include <elf_loader.h>
#include <init.h>
#include <mg/service.h>
#include <boot_checkpoint.h>
#include <x250_boot_trace.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

extern char __stack_top[];
extern char __stack_bottom[];
extern char __stack_guard_bottom[];
extern char __kernel_text_virt_start[];
extern char __kernel_text_virt_end[];
extern char __kernel_rodata_virt_start[];
extern char __kernel_rodata_virt_end[];
extern char __kernel_data_virt_start[];
extern char __kernel_data_virt_end[];
extern char __kernel_bss_virt_start[];
extern char __kernel_bss_virt_end[];

/* kmain_high receives a copied handoff record while running on the kernel
 * image's high stack.  Nothing after the permanent CR3 load dereferences the
 * loader's low identity aliases. */
static BOOT_INFO kernel_boot_info;

static void boot_info_convert_to_direct_map(BOOT_INFO *source)
{
    phys_addr_t memory_map_phys = (phys_addr_t)(uintptr_t)source->MemoryMap;
    phys_addr_t rsdp_phys = (phys_addr_t)(uintptr_t)source->Rsdp;

    kernel_boot_info = *source;
    phys_map_activate();
    kernel_boot_info.MemoryMap = (u8 *)phys_to_virt(memory_map_phys);
    kernel_boot_info.Rsdp = rsdp_phys ? phys_to_virt(rsdp_phys) : NULL;
}

static bool direct_map_memory_type(u32 type)
{
    return type == EFI_LOADER_CODE ||
           type == EFI_LOADER_DATA ||
           type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA ||
           type == EFI_CONVENTIONAL_MEMORY ||
           type == EFI_ACPI_RECLAIM_MEMORY ||
           type == EFI_ACPI_MEMORY_NVS;
}

static bool map_kernel_image_range(page_table_t *pml4, uintptr_t start,
                                   uintptr_t end, u64 flags)
{
    uintptr_t page = start & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t limit = (end + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);

    for (; page < limit; page += PAGE_SIZE) {
        phys_addr_t phys = kernel_image_virt_to_phys(
            page, kernel_boot_info.KernelPhysicalBase);
        if (!vmm_map(pml4, (void *)page, phys,
                     flags | PTE_PRESENT | PTE_GLOBAL)) {
            return false;
        }
    }
    return true;
}

static void scheduler_probe_entry(void *argument)
{
    (void)argument;
}

static kernel_thread_t *scheduler_priority_high;
static kernel_thread_t *scheduler_priority_n1;
static kernel_thread_t *scheduler_priority_n2;
static kernel_thread_t *scheduler_priority_background;
static bool scheduler_test_failed;
static char scheduler_priority_log[8];
static u32 scheduler_priority_log_length;

static void scheduler_priority_record(char marker)
{
    if (scheduler_priority_log_length < sizeof(scheduler_priority_log)) {
        scheduler_priority_log[scheduler_priority_log_length++] = marker;
    } else {
        scheduler_test_failed = true;
    }
}
static void scheduler_priority_high_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_high) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('H');
}

static void scheduler_priority_n1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_n1) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('1');
    if (!scheduler_yield()) {
        scheduler_test_failed = true;
    }
    if (thread_current() != scheduler_priority_n1) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('1');
}

static void scheduler_priority_n2_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_n2) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('2');
    if (!scheduler_yield()) {
        scheduler_test_failed = true;
    }
    if (thread_current() != scheduler_priority_n2) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('2');
}

static void scheduler_priority_background_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_background) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('B');
}

static void scheduler_priority_test(void)
{
    bool passed;

    scheduler_test_failed = false;
    scheduler_priority_log_length = 0;
    scheduler_priority_high = thread_create_with_priority(
        "scheduler-high", scheduler_priority_high_entry, NULL,
        THREAD_PRIORITY_HIGH);
    scheduler_priority_n1 = thread_create_with_priority(
        "scheduler-normal-1", scheduler_priority_n1_entry, NULL,
        THREAD_PRIORITY_NORMAL);
    scheduler_priority_n2 = thread_create_with_priority(
        "scheduler-normal-2", scheduler_priority_n2_entry, NULL,
        THREAD_PRIORITY_NORMAL);
    scheduler_priority_background = thread_create_with_priority(
        "scheduler-background", scheduler_priority_background_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);

    if (!scheduler_priority_high || !scheduler_priority_n1 ||
        !scheduler_priority_n2 || !scheduler_priority_background ||
        scheduler_priority_high->effective_priority != THREAD_PRIORITY_HIGH ||
        scheduler_priority_n1->effective_priority != THREAD_PRIORITY_NORMAL ||
        scheduler_priority_n2->effective_priority != THREAD_PRIORITY_NORMAL ||
        scheduler_priority_background->effective_priority != THREAD_PRIORITY_BACKGROUND ||
        !scheduler_reschedule()) {
        scheduler_test_failed = true;
    }

    passed = !scheduler_test_failed && scheduler_priority_log_length == 6 &&
        scheduler_priority_log[0] == 'H' &&
        scheduler_priority_log[1] == '1' &&
        scheduler_priority_log[2] == '2' &&
        scheduler_priority_log[3] == '1' &&
        scheduler_priority_log[4] == '2' &&
        scheduler_priority_log[5] == 'B' &&
        thread_current() && thread_current()->id == 1 &&
        scheduler_priority_high->state == THREAD_STATE_TERMINATED &&
        scheduler_priority_n1->state == THREAD_STATE_TERMINATED &&
        scheduler_priority_n2->state == THREAD_STATE_TERMINATED &&
        scheduler_priority_background->state == THREAD_STATE_TERMINATED;

    if (passed) {
        kprint("Scheduler: priority queues and round-robin test passed\n");
    } else {
        kprint("Scheduler: priority queues and round-robin test failed\n");
    }

    if (scheduler_priority_high) thread_destroy(scheduler_priority_high);
    if (scheduler_priority_n1) thread_destroy(scheduler_priority_n1);
    if (scheduler_priority_n2) thread_destroy(scheduler_priority_n2);
    if (scheduler_priority_background) thread_destroy(scheduler_priority_background);
    scheduler_priority_high = NULL;
    scheduler_priority_n1 = NULL;
    scheduler_priority_n2 = NULL;
    scheduler_priority_background = NULL;
}

static volatile char scheduler_timer_log[32];
static volatile u32 scheduler_timer_log_length;
static bool scheduler_timer_test_failed;
static volatile u32 scheduler_timer_worker_completions;
static volatile bool scheduler_timer_background_completed;
static volatile bool scheduler_timer_bootstrap_returned;
static kernel_thread_t *scheduler_timer_h1;
static kernel_thread_t *scheduler_timer_h2;
static kernel_thread_t *scheduler_timer_n1;
static kernel_thread_t *scheduler_timer_b1;

static void scheduler_timer_record(char marker)
{
    if (scheduler_timer_log_length < sizeof(scheduler_timer_log)) {
        scheduler_timer_log[scheduler_timer_log_length++] = marker;
    } else {
        scheduler_timer_test_failed = true;
    }
}

static void scheduler_timer_wait(void)
{
    u64 start = timer_ticks();
    while (timer_ticks() - start < 8) {
        __asm__ volatile("pause");
    }
}

static void scheduler_timer_h1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_h1) scheduler_timer_test_failed = true;
    scheduler_timer_record('1');
    scheduler_timer_wait();
    scheduler_timer_record('1');
    scheduler_timer_worker_completions++;
}

static void scheduler_timer_h2_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_h2) scheduler_timer_test_failed = true;
    scheduler_timer_record('2');
    scheduler_timer_wait();
    scheduler_timer_record('2');
    scheduler_timer_worker_completions++;
}

static void scheduler_timer_n1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_n1) scheduler_timer_test_failed = true;
    scheduler_timer_record('N');
    scheduler_timer_wait();
    scheduler_timer_record('N');
    scheduler_timer_worker_completions++;
}

static void scheduler_timer_b1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_b1) scheduler_timer_test_failed = true;
    scheduler_timer_record('B');
    scheduler_timer_wait();
    scheduler_timer_record('B');
    scheduler_timer_worker_completions++;
    scheduler_timer_background_completed = true;
}

static void scheduler_timer_test(void)
{
    bool passed;
    bool preemptions_ok;
    bool workers_ok;
    bool background_ok;
    bool bootstrap_ok;
    bool log_ok;
    u64 preemptions_before;
    u32 first_normal = 0;
    u32 first_background = 0;

    __asm__ volatile("cli");
    scheduler_timer_test_failed = false;
    scheduler_timer_log_length = 0;
    scheduler_timer_worker_completions = 0;
    scheduler_timer_background_completed = false;
    scheduler_timer_bootstrap_returned = false;
    preemptions_before = timer_preemptions();
    scheduler_timer_h1 = thread_create_with_priority(
        "timer-high-1", scheduler_timer_h1_entry, NULL, THREAD_PRIORITY_HIGH);
    scheduler_timer_h2 = thread_create_with_priority(
        "timer-high-2", scheduler_timer_h2_entry, NULL, THREAD_PRIORITY_HIGH);
    scheduler_timer_n1 = thread_create_with_priority(
        "timer-normal", scheduler_timer_n1_entry, NULL, THREAD_PRIORITY_NORMAL);
    scheduler_timer_b1 = thread_create_with_priority(
        "timer-background", scheduler_timer_b1_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);
    __asm__ volatile("sti");

    if (!scheduler_timer_h1 || !scheduler_timer_h2 || !scheduler_timer_n1 ||
        !scheduler_timer_b1 || !scheduler_reschedule()) {
        scheduler_timer_test_failed = true;
    }
    scheduler_timer_bootstrap_returned = true;

    for (u32 i = 0; i < scheduler_timer_log_length; i++) {
        if (!first_normal && scheduler_timer_log[i] == 'N') first_normal = i + 1;
        if (!first_background && scheduler_timer_log[i] == 'B') first_background = i + 1;
    }
    preemptions_ok = timer_preemptions() > preemptions_before;
    workers_ok = scheduler_timer_worker_completions == 4 &&
        scheduler_timer_h1 && scheduler_timer_h2 && scheduler_timer_n1 &&
        scheduler_timer_b1 &&
        scheduler_timer_h1->state == THREAD_STATE_TERMINATED &&
        scheduler_timer_h2->state == THREAD_STATE_TERMINATED &&
        scheduler_timer_n1->state == THREAD_STATE_TERMINATED &&
        scheduler_timer_b1->state == THREAD_STATE_TERMINATED;
    background_ok = scheduler_timer_background_completed;
    bootstrap_ok = scheduler_timer_bootstrap_returned && thread_current() &&
        thread_current()->id == 1;
    /* The second pair records completion, not dispatch.  A timer may fire
     * between a worker's first marker and its wait-start snapshot, so either
     * completion order is valid while FIFO dispatch is checked separately by
     * the buffered scheduler trace. */
    log_ok = scheduler_timer_log_length == 8 &&
        scheduler_timer_log[0] == '1' &&
        scheduler_timer_log[1] == '2' &&
        ((scheduler_timer_log[2] == '1' && scheduler_timer_log[3] == '2') ||
         (scheduler_timer_log[2] == '2' && scheduler_timer_log[3] == '1')) &&
        scheduler_timer_log[4] == 'N' &&
        scheduler_timer_log[5] == 'N' &&
        scheduler_timer_log[6] == 'B' &&
        scheduler_timer_log[7] == 'B' &&
        first_normal == 5 && first_background == 7;
    passed = !scheduler_timer_test_failed && preemptions_ok && workers_ok &&
        background_ok && bootstrap_ok && log_ok;
    kprint("Scheduler: timer preemption test %s\n",
           passed ? "passed" : "failed");

    if (scheduler_timer_h1) thread_destroy(scheduler_timer_h1);
    if (scheduler_timer_h2) thread_destroy(scheduler_timer_h2);
    if (scheduler_timer_n1) thread_destroy(scheduler_timer_n1);
    if (scheduler_timer_b1) thread_destroy(scheduler_timer_b1);
    scheduler_timer_h1 = NULL;
    scheduler_timer_h2 = NULL;
    scheduler_timer_n1 = NULL;
    scheduler_timer_b1 = NULL;
}

static volatile bool scheduler_sleep_a_woke;
static volatile bool scheduler_sleep_b_woke;
static volatile bool scheduler_sleep_b_priority_ok;
static volatile bool scheduler_sleep_b_requeue_ok;
static kernel_thread_t *scheduler_sleep_a;
static kernel_thread_t *scheduler_sleep_b;

static void scheduler_sleep_a_entry(void *argument)
{
    (void)argument;
    if (!scheduler_sleep(scheduler_sleep_a ? 4 : 0)) {
        return;
    }
    scheduler_sleep_a_woke = thread_current() == scheduler_sleep_a;
}

static void scheduler_sleep_b_entry(void *argument)
{
    (void)argument;
    if (!scheduler_sleep(8)) {
        return;
    }
    scheduler_sleep_b_priority_ok =
        thread_current() == scheduler_sleep_b &&
        scheduler_sleep_b->base_priority == THREAD_PRIORITY_BACKGROUND &&
        scheduler_sleep_b->effective_priority == THREAD_PRIORITY_BACKGROUND &&
        scheduler_sleep_b->last_selected_priority == THREAD_PRIORITY_NORMAL &&
        scheduler_sleep_b->last_selection_was_wakeup_boost;
    /* With no alternative runnable worker, yield legitimately returns false
     * after selecting this same thread again.  The queue round trip still
     * clears the one-shot wakeup boost and is what this test verifies. */
    (void)scheduler_yield();
    scheduler_sleep_b_requeue_ok =
        scheduler_sleep_b->last_selected_priority == THREAD_PRIORITY_BACKGROUND &&
        !scheduler_sleep_b->last_selection_was_wakeup_boost &&
        scheduler_sleep_b->base_priority == THREAD_PRIORITY_BACKGROUND &&
        scheduler_sleep_b->effective_priority == THREAD_PRIORITY_BACKGROUND;
    scheduler_sleep_b_woke = thread_current() == scheduler_sleep_b;
}

static void scheduler_sleep_idle_test(bool timer_ready)
{
    bool passed;
    bool idle_ran;
    scheduler_stats_t stats_before;
    scheduler_stats_t stats_after;

    if (!timer_ready) {
        kprint("Scheduler: sleep and idle test failed (timer probe timeout)\n");
        __asm__ volatile("cli" ::: "memory");
        return;
    }

    __asm__ volatile("cli" ::: "memory");
    scheduler_sleep_a_woke = false;
    scheduler_sleep_b_woke = false;
    scheduler_sleep_b_priority_ok = false;
    scheduler_sleep_b_requeue_ok = false;
    scheduler_get_stats(&stats_before);
    scheduler_sleep_a = thread_create_with_priority(
        "sleep-a", scheduler_sleep_a_entry, NULL, THREAD_PRIORITY_HIGH);
    scheduler_sleep_b = thread_create_with_priority(
        "sleep-b", scheduler_sleep_b_entry, NULL, THREAD_PRIORITY_BACKGROUND);
    passed = scheduler_sleep_a && scheduler_sleep_b;
    if (passed) {
        __asm__ volatile("sti" ::: "memory");
        passed = scheduler_reschedule();
        __asm__ volatile("cli" ::: "memory");
    }
    scheduler_get_stats(&stats_after);
    idle_ran = stats_after.idle_runtime_ticks > stats_before.idle_runtime_ticks;
    passed = passed &&
        scheduler_sleep_a_woke && scheduler_sleep_b_woke &&
        scheduler_sleep_b_priority_ok && scheduler_sleep_b_requeue_ok &&
        idle_ran &&
        scheduler_sleep_a->state == THREAD_STATE_TERMINATED &&
        scheduler_sleep_b->state == THREAD_STATE_TERMINATED;

    __asm__ volatile("cli" ::: "memory");
    kprint("Scheduler: sleep and idle test %s\n",
           passed ? "passed" : "failed");

    if (scheduler_sleep_a) thread_destroy(scheduler_sleep_a);
    if (scheduler_sleep_b) thread_destroy(scheduler_sleep_b);
    scheduler_sleep_a = NULL;
    scheduler_sleep_b = NULL;
    /* The caller publishes the fairness test before re-enabling IRQs. */
    __asm__ volatile("cli" ::: "memory");
}

static volatile bool scheduler_fair_normal_ran;
static volatile bool scheduler_fair_background_ran;
static volatile bool scheduler_fair_normal_priority_ok;
static volatile bool scheduler_fair_background_priority_ok;
static volatile bool scheduler_fair_high_started;
static volatile bool scheduler_fair_bootstrap_returned;
static kernel_thread_t *scheduler_fair_high;
static kernel_thread_t *scheduler_fair_normal;
static kernel_thread_t *scheduler_fair_background;

static void scheduler_fair_high_entry(void *argument)
{
    (void)argument;
    scheduler_fair_high_started = true;
    while (!scheduler_fair_normal_ran || !scheduler_fair_background_ran) {
        __asm__ volatile("pause");
    }
}

static void scheduler_fair_normal_entry(void *argument)
{
    (void)argument;
    scheduler_fair_normal_priority_ok =
        thread_current() == scheduler_fair_normal &&
        scheduler_fair_normal->last_selected_priority == THREAD_PRIORITY_HIGH &&
        scheduler_fair_normal->effective_priority == THREAD_PRIORITY_NORMAL &&
        scheduler_fair_normal->base_priority == THREAD_PRIORITY_NORMAL;
    scheduler_fair_normal_ran = true;
}

static void scheduler_fair_background_entry(void *argument)
{
    (void)argument;
    scheduler_fair_background_priority_ok =
        thread_current() == scheduler_fair_background &&
        scheduler_fair_background->last_selected_priority ==
            THREAD_PRIORITY_HIGH &&
        scheduler_fair_background->effective_priority == THREAD_PRIORITY_BACKGROUND &&
        scheduler_fair_background->base_priority == THREAD_PRIORITY_BACKGROUND;
    scheduler_fair_background_ran = true;
}

static void scheduler_fairness_test(void)
{
    bool passed;
    bool dispatch_ok;
    bool workers_created;
    kernel_thread_t *bootstrap;

    __asm__ volatile("cli" ::: "memory");
    bootstrap = thread_current();

    scheduler_fair_normal_ran = false;
    scheduler_fair_background_ran = false;
    scheduler_fair_normal_priority_ok = false;
    scheduler_fair_background_priority_ok = false;
    scheduler_fair_high_started = false;
    scheduler_fair_bootstrap_returned = false;
    scheduler_fair_high = thread_create_with_priority(
        "fair-high", scheduler_fair_high_entry, NULL, THREAD_PRIORITY_HIGH);
    scheduler_fair_normal = thread_create_with_priority(
        "fair-normal", scheduler_fair_normal_entry, NULL, THREAD_PRIORITY_NORMAL);
    scheduler_fair_background = thread_create_with_priority(
        "fair-background", scheduler_fair_background_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);
    workers_created = scheduler_fair_high && scheduler_fair_normal &&
        scheduler_fair_background;
    __asm__ volatile("sti" ::: "memory");

    dispatch_ok = workers_created && scheduler_reschedule();
    __asm__ volatile("cli" ::: "memory");
    scheduler_fair_bootstrap_returned = thread_current() == bootstrap;
    passed = dispatch_ok && scheduler_fair_high_started &&
        scheduler_fair_bootstrap_returned && scheduler_fair_normal_ran &&
        scheduler_fair_background_ran && scheduler_fair_normal_priority_ok &&
        scheduler_fair_background_priority_ok &&
        scheduler_fair_normal->effective_priority ==
            scheduler_fair_normal->base_priority &&
        scheduler_fair_background->effective_priority ==
            scheduler_fair_background->base_priority &&
        scheduler_fair_high->state == THREAD_STATE_TERMINATED &&
        scheduler_fair_normal->state == THREAD_STATE_TERMINATED &&
        scheduler_fair_background->state == THREAD_STATE_TERMINATED &&
        scheduler_validate_state();

    if (passed) {
        kprint("Scheduler: fairness and integrity test passed\n");
    } else {
        kprint("Scheduler: fairness and integrity test failed\n");
    }

    if (!passed) {
        /* Leave failed self-test threads intact for the diagnostic kernel. */
    }

    if (scheduler_fair_high) thread_destroy(scheduler_fair_high);
    if (scheduler_fair_normal) thread_destroy(scheduler_fair_normal);
    if (scheduler_fair_background) thread_destroy(scheduler_fair_background);
    scheduler_fair_high = NULL;
    scheduler_fair_normal = NULL;
    scheduler_fair_background = NULL;
    __asm__ volatile("sti" ::: "memory");
}

static volatile u32 scheduler_stress_steps[3];
static volatile u32 scheduler_stress_once;

static void scheduler_stress_entry(void *argument)
{
    volatile u32 *steps = (volatile u32 *)argument;
    u32 i;

    for (i = 0; i < 128; i++) {
        (*steps)++;
        if (!scheduler_yield()) {
            return;
        }
    }
}

static void scheduler_stress_once_entry(void *argument)
{
    (void)argument;
    scheduler_stress_once++;
}

static void scheduler_stress_test(void)
{
    kernel_thread_t *threads[3];
    u32 i;
    bool passed = true;

    memset((void *)scheduler_stress_steps, 0,
           sizeof(scheduler_stress_steps));
    threads[0] = thread_create("stress-0", scheduler_stress_entry,
                               (void *)&scheduler_stress_steps[0]);
    threads[1] = thread_create("stress-1", scheduler_stress_entry,
                               (void *)&scheduler_stress_steps[1]);
    threads[2] = thread_create("stress-2", scheduler_stress_entry,
                               (void *)&scheduler_stress_steps[2]);
    passed = threads[0] && threads[1] && threads[2] && scheduler_reschedule();
    for (i = 0; i < 3; i++) {
        passed = passed && scheduler_stress_steps[i] == 128 &&
            threads[i]->state == THREAD_STATE_TERMINATED;
    }
    passed = passed && scheduler_validate_state();
    for (i = 0; i < 3; i++) {
        if (threads[i]) {
            passed = thread_destroy(threads[i]) && passed;
        }
    }

    scheduler_stress_once = 0;
    for (i = 0; i < 16 && passed; i++) {
        kernel_thread_t *thread = thread_create(
            "stress-once", scheduler_stress_once_entry, NULL);
        passed = thread && scheduler_reschedule() &&
            thread->state == THREAD_STATE_TERMINATED &&
            thread_destroy(thread) && scheduler_validate_state();
    }
    passed = passed && scheduler_stress_once == 16;

    if (passed) {
        kprint("Scheduler: stress and validation test passed\n");
    } else {
        kprint("Scheduler: stress and validation test failed\n");
    }
}

#if defined(SCHEDULER_CONTENTION_GUEST_TEST)
#define SCHEDULER_CONTENTION_MAX_CPUS 4U
#define SCHEDULER_CONTENTION_MAX_HIGH_WORKERS \
    (2U * SCHEDULER_CONTENTION_MAX_CPUS + 1U)
#define SCHEDULER_CONTENTION_MAX_WORKERS \
    (SCHEDULER_CONTENTION_MAX_HIGH_WORKERS + 4U)
#define SCHEDULER_CONTENTION_TIMEOUT_MS 16000U

typedef enum {
    SCHEDULER_CONTENTION_HIGH = 0,
    SCHEDULER_CONTENTION_NORMAL,
    SCHEDULER_CONTENTION_BACKGROUND,
    SCHEDULER_CONTENTION_WAKE_BACKGROUND,
    SCHEDULER_CONTENTION_CONTROLLER,
} scheduler_contention_kind_t;

typedef struct {
    kernel_thread_t *thread;
    scheduler_contention_kind_t kind;
    volatile u64 work_units;
    u64 first_run_ms;
    u64 sleep_start_ms;
    u64 wake_latency_ms;
    u64 wake_ready_after_ms;
    u64 wake_high_after_ms;
    thread_priority_t selected_priority;
    thread_priority_t base_priority;
    thread_priority_t effective_priority;
    volatile bool started;
    volatile bool completed;
    volatile bool sleep_succeeded;
    volatile bool wake_ready_seen;
    volatile bool wake_high_seen;
    volatile u32 queue_validation_samples;
    volatile bool queue_validation_failed;
} scheduler_contention_worker_t;

static scheduler_contention_worker_t scheduler_contention_high[
    SCHEDULER_CONTENTION_MAX_HIGH_WORKERS];
static scheduler_contention_worker_t scheduler_contention_normal;
static scheduler_contention_worker_t scheduler_contention_background;
static scheduler_contention_worker_t scheduler_contention_wake_background;
static scheduler_contention_worker_t scheduler_contention_controller;
static volatile bool scheduler_contention_stop;
static volatile bool scheduler_contention_timeout;
static volatile u64 scheduler_contention_start_ms;
static volatile u32 scheduler_contention_high_worker_count;
static volatile u32 scheduler_contention_high_started;
static volatile u32 scheduler_contention_high_completed;
static volatile bool scheduler_contention_normal_seen;
static volatile bool scheduler_contention_background_seen;
static volatile bool scheduler_contention_wake_seen;
static volatile u64 scheduler_contention_cpu_mask;
static volatile u64 scheduler_contention_active_cpus;
static volatile u64 scheduler_contention_overlap_samples;
static volatile u64 scheduler_contention_useful_work;

static void scheduler_contention_window(scheduler_contention_worker_t *worker)
{
    u64 flags;
    u64 bit;
    u32 cpu;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    cpu = cpu_current_index();
    if (cpu >= 64U) {
        __atomic_store_n(&scheduler_test_failed, true, __ATOMIC_RELAXED);
        __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
        return;
    }
    bit = 1ULL << cpu;
    __atomic_or_fetch(&scheduler_contention_cpu_mask, bit, __ATOMIC_RELAXED);
    __atomic_or_fetch(&scheduler_contention_active_cpus, bit,
                      __ATOMIC_RELAXED);
    {
        u64 active = __atomic_load_n(&scheduler_contention_active_cpus,
                                     __ATOMIC_RELAXED);
        if (active & (active - 1U)) {
        __atomic_add_fetch(&scheduler_contention_overlap_samples, 1U,
                           __ATOMIC_RELAXED);
        }
    }

    /* Keep a short non-preemptible useful-work window so the active CPU mask
     * is a simultaneous execution witness, not a historical participation
     * union. Scheduling and timer preemption happen outside this window. */
    for (u32 i = 0; i < 512U; i++) {
        worker->work_units += (u64)(i + 1U);
        __asm__ volatile("pause" ::: "memory");
    }
    __atomic_add_fetch(&scheduler_contention_useful_work, 512U,
                       __ATOMIC_RELAXED);
    __atomic_and_fetch(&scheduler_contention_active_cpus, ~bit,
                       __ATOMIC_RELAXED);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static void scheduler_contention_high_entry(void *argument)
{
    scheduler_contention_worker_t *worker = argument;
    u32 windows = 0;

    if (!worker || thread_current() != worker->thread) {
        __atomic_store_n(&scheduler_test_failed, true, __ATOMIC_RELAXED);
        return;
    }
    worker->first_run_ms = timer_uptime_ms();
    __atomic_store_n(&worker->started, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&scheduler_contention_high_started, 1U,
                       __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&scheduler_contention_high_started,
                           __ATOMIC_ACQUIRE) <
               __atomic_load_n(&scheduler_contention_high_worker_count,
                               __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&scheduler_contention_stop, __ATOMIC_ACQUIRE)) {
        scheduler_contention_window(worker);
        /* The workload is deliberately oversubscribed at every CPU count.
         * Cooperate while forming the start cohort so a single CPU, or a
         * smaller worker set than the cohort, cannot spin ahead of workers
         * that have not yet received their first dispatch. Timer preemption
         * is measured separately by the native SMP workload. */
        (void)scheduler_yield();
    }

    while (!__atomic_load_n(&scheduler_contention_stop, __ATOMIC_ACQUIRE)) {
        scheduler_contention_window(worker);
        if ((++windows & 3U) == 0U)
            (void)scheduler_yield();
    }
    __atomic_add_fetch(&scheduler_contention_high_completed, 1U,
                       __ATOMIC_RELEASE);
    __atomic_store_n(&worker->completed, true, __ATOMIC_RELEASE);
}

static void scheduler_contention_low_entry(void *argument)
{
    scheduler_contention_worker_t *worker = argument;
    bool background;

    if (!worker || thread_current() != worker->thread) {
        __atomic_store_n(&scheduler_test_failed, true, __ATOMIC_RELAXED);
        return;
    }
    background = worker->kind == SCHEDULER_CONTENTION_BACKGROUND;
    worker->first_run_ms = timer_uptime_ms();
    worker->selected_priority = worker->thread->last_selected_priority;
    worker->base_priority = worker->thread->base_priority;
    worker->effective_priority = worker->thread->effective_priority;
    worker->work_units = 1U;
    __atomic_store_n(&worker->started, true, __ATOMIC_RELEASE);
    if (background)
        __atomic_store_n(&scheduler_contention_background_seen, true,
                         __ATOMIC_RELEASE);
    else
        __atomic_store_n(&scheduler_contention_normal_seen, true,
                         __ATOMIC_RELEASE);
    __atomic_store_n(&worker->completed, true, __ATOMIC_RELEASE);
}

static void scheduler_contention_wake_entry(void *argument)
{
    scheduler_contention_worker_t *worker = argument;

    if (!worker || thread_current() != worker->thread) {
        __atomic_store_n(&scheduler_test_failed, true, __ATOMIC_RELAXED);
        return;
    }
    worker->sleep_start_ms = timer_uptime_ms();
    __atomic_store_n(&worker->started, true, __ATOMIC_RELEASE);
    worker->sleep_succeeded = scheduler_sleep(250U);
    worker->wake_latency_ms = timer_uptime_ms() - worker->sleep_start_ms;
    worker->first_run_ms = timer_uptime_ms();
    worker->selected_priority = worker->thread->last_selected_priority;
    worker->base_priority = worker->thread->base_priority;
    worker->effective_priority = worker->thread->effective_priority;
    worker->work_units = worker->sleep_succeeded ? 1U : 0U;
    __atomic_store_n(&worker->completed, true, __ATOMIC_RELEASE);
    __atomic_store_n(&scheduler_contention_wake_seen,
                     worker->sleep_succeeded, __ATOMIC_RELEASE);
}

static void scheduler_contention_controller_entry(void *argument)
{
    scheduler_contention_worker_t *worker = argument;

    if (!worker || thread_current() != worker->thread) {
        __atomic_store_n(&scheduler_test_failed, true, __ATOMIC_RELAXED);
        __atomic_store_n(&scheduler_contention_stop, true, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&worker->started, true, __ATOMIC_RELEASE);
    for (;;) {
        thread_state_t wake_state;
        thread_priority_t wake_effective;
        thread_priority_t wake_selected;
        u64 now_ms = timer_uptime_ms();
        if (scheduler_contention_thread_snapshot(
                scheduler_contention_wake_background.thread, &wake_state,
                NULL, &wake_effective, &wake_selected, NULL, NULL, NULL)) {
            scheduler_contention_worker_t *wake =
                &scheduler_contention_wake_background;
            if (!wake->wake_ready_seen && wake_state != THREAD_STATE_BLOCKED) {
                wake->wake_ready_after_ms = now_ms >=
                    scheduler_contention_start_ms ? now_ms -
                    scheduler_contention_start_ms : 0U;
                __atomic_store_n(&wake->wake_ready_seen, true,
                                 __ATOMIC_RELEASE);
            }
            if (!wake->wake_high_seen &&
                (wake_effective == THREAD_PRIORITY_HIGH ||
                 wake_selected == THREAD_PRIORITY_HIGH)) {
                wake->wake_high_after_ms = now_ms >=
                    scheduler_contention_start_ms ? now_ms -
                    scheduler_contention_start_ms : 0U;
                __atomic_store_n(&wake->wake_high_seen, true,
                                 __ATOMIC_RELEASE);
            }
        }
        if (!scheduler_validate_state()) {
            worker->queue_validation_failed = true;
            __atomic_store_n(&scheduler_test_failed, true, __ATOMIC_RELAXED);
            break;
        }
        __atomic_add_fetch(&worker->queue_validation_samples, 1U,
                           __ATOMIC_RELAXED);
        bool done =
            __atomic_load_n(&scheduler_contention_high_started,
                            __ATOMIC_ACQUIRE) ==
                __atomic_load_n(&scheduler_contention_high_worker_count,
                                __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&scheduler_contention_normal_seen,
                            __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&scheduler_contention_background_seen,
                            __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&scheduler_contention_wake_seen,
                            __ATOMIC_ACQUIRE);
        if (done)
            break;
        if (now_ms - scheduler_contention_start_ms >=
            SCHEDULER_CONTENTION_TIMEOUT_MS) {
            __atomic_store_n(&scheduler_contention_timeout, true,
                             __ATOMIC_RELEASE);
            break;
        }
        if (!scheduler_sleep(100U)) {
            __atomic_store_n(&scheduler_test_failed, true,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&scheduler_contention_timeout, true,
                             __ATOMIC_RELEASE);
            break;
        }
    }

    __atomic_store_n(&scheduler_contention_stop, true, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&scheduler_contention_wake_seen, __ATOMIC_ACQUIRE))
        (void)scheduler_wake_waiter(
            scheduler_contention_wake_background.thread);
    __atomic_store_n(&worker->completed, true, __ATOMIC_RELEASE);
}

static bool scheduler_contention_prepare_worker(
    scheduler_contention_worker_t *worker, const char *name,
    thread_entry_t entry, scheduler_contention_kind_t kind,
    thread_priority_t priority)
{
    memset(worker, 0, sizeof(*worker));
    worker->kind = kind;
    worker->thread = thread_create_suspended_with_priority(
        name, entry, worker, priority);
    return worker->thread != NULL;
}

static bool scheduler_contention_worker_stopped(
    scheduler_contention_worker_t *worker)
{
    return worker && worker->thread &&
        scheduler_thread_exit_complete(worker->thread);
}

bool kernel_scheduler_contention_guest_test(void)
{
    scheduler_contention_worker_t *all[SCHEDULER_CONTENTION_MAX_WORKERS];
    scheduler_stats_t before;
    scheduler_stats_t after;
    u64 busy_before[SCHEDULER_CONTENTION_MAX_CPUS] = {0};
    u64 busy_after[SCHEDULER_CONTENTION_MAX_CPUS] = {0};
    u64 busy_delta[SCHEDULER_CONTENTION_MAX_CPUS] = {0};
    u64 start_ms;
    u64 end_ms;
    u64 cleanup_deadline;
    u64 expected_mask;
    u64 observed_mask;
    u64 busy_mask = 0;
    u64 overlap_samples;
    u32 cpus = cpu_online_count();
    u32 high_count;
    u32 all_count = 0;
    bool created = true;
    bool timeout = false;
    bool drained = true;
    bool validated;
    bool passed;

    if (cpus == 0U || cpus > SCHEDULER_CONTENTION_MAX_CPUS) {
        kprint("SCHED_CONTENTION pass=0 cpus=%u reason=unsupported-online-cpu-count\n",
               cpus);
        return false;
    }
    high_count = 2U * cpus + 1U;
    scheduler_contention_high_worker_count = high_count;
    expected_mask = (1ULL << cpus) - 1ULL;
    scheduler_contention_stop = false;
    scheduler_contention_timeout = false;
    scheduler_contention_start_ms = 0;
    scheduler_contention_high_started = 0;
    scheduler_contention_high_completed = 0;
    scheduler_contention_normal_seen = false;
    scheduler_contention_background_seen = false;
    scheduler_contention_wake_seen = false;
    scheduler_contention_cpu_mask = 0;
    scheduler_contention_active_cpus = 0;
    scheduler_contention_overlap_samples = 0;
    scheduler_contention_useful_work = 0;
    scheduler_test_failed = false;

    created = scheduler_contention_prepare_worker(
        &scheduler_contention_wake_background, "sched-wake-background",
        scheduler_contention_wake_entry,
        SCHEDULER_CONTENTION_WAKE_BACKGROUND, THREAD_PRIORITY_BACKGROUND);
    all[all_count++] = &scheduler_contention_wake_background;
    for (u32 i = 0; created && i < high_count; i++) {
        created = scheduler_contention_prepare_worker(
            &scheduler_contention_high[i], "sched-high",
            scheduler_contention_high_entry, SCHEDULER_CONTENTION_HIGH,
            THREAD_PRIORITY_HIGH);
        all[all_count++] = &scheduler_contention_high[i];
    }
    if (created) {
        created = scheduler_contention_prepare_worker(
            &scheduler_contention_normal, "sched-fair-normal",
            scheduler_contention_low_entry, SCHEDULER_CONTENTION_NORMAL,
            THREAD_PRIORITY_NORMAL);
        all[all_count++] = &scheduler_contention_normal;
    }
    if (created) {
        created = scheduler_contention_prepare_worker(
            &scheduler_contention_background, "sched-fair-background",
            scheduler_contention_low_entry,
            SCHEDULER_CONTENTION_BACKGROUND, THREAD_PRIORITY_BACKGROUND);
        all[all_count++] = &scheduler_contention_background;
    }
    if (created) {
        created = scheduler_contention_prepare_worker(
            &scheduler_contention_controller, "sched-contention-control",
            scheduler_contention_controller_entry,
            SCHEDULER_CONTENTION_CONTROLLER, THREAD_PRIORITY_HIGH);
        all[all_count++] = &scheduler_contention_controller;
    }
    if (!created) {
        for (u32 i = 0; i < all_count; i++)
            if (all[i]->thread) (void)thread_destroy(all[i]->thread);
        kprint("SCHED_CONTENTION pass=0 cpus=%u reason=thread-creation-failed\n",
               cpus);
        return false;
    }

    if (!scheduler_enqueue(scheduler_contention_wake_background.thread) ||
        !scheduler_reschedule()) {
        created = false;
    }
    start_ms = timer_uptime_ms();
    while (created && scheduler_thread_state_snapshot(
               scheduler_contention_wake_background.thread) !=
               THREAD_STATE_BLOCKED && timer_uptime_ms() - start_ms < 1000U) {
        (void)scheduler_yield();
    }
    if (scheduler_thread_state_snapshot(
            scheduler_contention_wake_background.thread) !=
        THREAD_STATE_BLOCKED) {
        created = false;
    }
    if (!created) {
        scheduler_contention_stop = true;
        (void)scheduler_wake_waiter(
            scheduler_contention_wake_background.thread);
        for (u32 i = 0; i < all_count; i++) {
            if (all[i]->thread && !all[i]->started)
                (void)scheduler_terminate_thread(all[i]->thread);
            if (all[i]->thread &&
                scheduler_thread_exit_complete(all[i]->thread))
                (void)thread_destroy(all[i]->thread);
        }
        kprint("SCHED_CONTENTION pass=0 cpus=%u reason=wake-worker-did-not-block\n",
               cpus);
        return false;
    }
    scheduler_get_stats(&before);
    scheduler_contention_counters_reset();
    for (u32 i = 0; i < cpus; i++) {
        cpu_local_t *cpu = cpu_by_index(i);
        if (!cpu || !cpu->online) {
            created = false;
            continue;
        }
        busy_before[i] = __atomic_load_n(&cpu->scheduler_busy_ticks,
                                         __ATOMIC_RELAXED);
    }
    start_ms = timer_uptime_ms();
    scheduler_contention_start_ms = start_ms;
    for (u32 i = 0; created && i < high_count; i++)
        created = scheduler_enqueue(scheduler_contention_high[i].thread);
    if (created)
        created = scheduler_enqueue(scheduler_contention_normal.thread) &&
                  scheduler_enqueue(scheduler_contention_background.thread);
    if (created)
        created = scheduler_enqueue(scheduler_contention_controller.thread);
    if (!created) {
        __atomic_store_n(&scheduler_contention_stop, true,
                         __ATOMIC_RELEASE);
        (void)scheduler_wake_waiter(
            scheduler_contention_wake_background.thread);
    }

    /* The boot callback runs on the special BSP bootstrap context, which is
     * resumed only after the runnable queue drains. A same-priority control
     * worker owns the deadline and stop transition so oversubscribed workers
     * cannot starve the callback that launched them. */
    while (created &&
           !__atomic_load_n(&scheduler_contention_controller.completed,
                            __ATOMIC_ACQUIRE)) {
        (void)scheduler_yield();
    }
    timeout = !created || __atomic_load_n(&scheduler_contention_timeout,
                                          __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < all_count; i++) {
        if (all[i]->thread && !all[i]->started &&
            scheduler_thread_state_snapshot(all[i]->thread) ==
                THREAD_STATE_READY)
            (void)scheduler_terminate_thread(all[i]->thread);
    }
    cleanup_deadline = timer_uptime_ms() + 4000U;
    while (timer_uptime_ms() < cleanup_deadline) {
        bool all_stopped = true;
        for (u32 i = 0; i < all_count; i++) {
            if (!scheduler_contention_worker_stopped(all[i])) {
                all_stopped = false;
                break;
            }
        }
        if (all_stopped)
            break;
        (void)scheduler_yield();
    }
    for (u32 i = 0; i < all_count; i++) {
        if (!scheduler_contention_worker_stopped(all[i])) {
            drained = false;
            continue;
        }
        if (!thread_destroy(all[i]->thread))
            drained = false;
        all[i]->thread = NULL;
    }
    end_ms = timer_uptime_ms();
    scheduler_get_stats(&after);
    observed_mask = __atomic_load_n(&scheduler_contention_cpu_mask,
                                    __ATOMIC_RELAXED);
    overlap_samples = __atomic_load_n(
        &scheduler_contention_overlap_samples, __ATOMIC_RELAXED);
    for (u32 i = 0; i < cpus; i++) {
        cpu_local_t *cpu = cpu_by_index(i);
        if (!cpu) continue;
        busy_after[i] = __atomic_load_n(&cpu->scheduler_busy_ticks,
                                        __ATOMIC_RELAXED);
        busy_delta[i] = busy_after[i] - busy_before[i];
        if (busy_delta[i]) busy_mask |= 1ULL << i;
    }
    validated = scheduler_validate_state();
    passed = created && !timeout && drained && validated &&
        !__atomic_load_n(&scheduler_test_failed, __ATOMIC_RELAXED) &&
        __atomic_load_n(&scheduler_contention_high_completed,
                        __ATOMIC_ACQUIRE) == high_count &&
        __atomic_load_n(&scheduler_contention_normal_seen,
                        __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&scheduler_contention_background_seen,
                        __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&scheduler_contention_wake_seen,
                        __ATOMIC_ACQUIRE) &&
        scheduler_contention_normal.selected_priority == THREAD_PRIORITY_HIGH &&
        scheduler_contention_background.selected_priority ==
            THREAD_PRIORITY_HIGH &&
        scheduler_contention_wake_background.sleep_succeeded &&
        scheduler_contention_wake_background.selected_priority ==
            THREAD_PRIORITY_HIGH &&
        scheduler_contention_normal.base_priority == THREAD_PRIORITY_NORMAL &&
        scheduler_contention_normal.effective_priority ==
            THREAD_PRIORITY_NORMAL &&
        scheduler_contention_background.base_priority ==
            THREAD_PRIORITY_BACKGROUND &&
        scheduler_contention_background.effective_priority ==
            THREAD_PRIORITY_BACKGROUND &&
        scheduler_contention_wake_background.base_priority ==
            THREAD_PRIORITY_BACKGROUND &&
        scheduler_contention_wake_background.effective_priority ==
            THREAD_PRIORITY_BACKGROUND &&
        __atomic_load_n(&scheduler_contention_wake_background.wake_ready_seen,
                        __ATOMIC_ACQUIRE) &&
        scheduler_contention_wake_background.wake_ready_after_ms >= 100U &&
        scheduler_contention_wake_background.wake_ready_after_ms <= 500U &&
        __atomic_load_n(&scheduler_contention_wake_background.wake_high_seen,
                        __ATOMIC_ACQUIRE) &&
        scheduler_contention_controller.queue_validation_samples >= 20U &&
        !scheduler_contention_controller.queue_validation_failed &&
        scheduler_contention_wake_background.wake_high_after_ms >= 650U &&
        scheduler_contention_wake_background.wake_high_after_ms <= 1500U &&
        scheduler_contention_normal.first_run_ms >= start_ms + 4500U &&
        scheduler_contention_background.first_run_ms >= start_ms + 5400U &&
        scheduler_contention_wake_background.wake_latency_ms >= 450U &&
        scheduler_contention_wake_background.wake_latency_ms <= 2500U &&
        observed_mask == expected_mask && busy_mask == expected_mask &&
        (cpus == 1U ? overlap_samples == 0U : overlap_samples > 0U) &&
        __atomic_load_n(&scheduler_contention_useful_work,
                        __ATOMIC_RELAXED) > 0U &&
        after.dispatches[THREAD_PRIORITY_HIGH] >
            before.dispatches[THREAD_PRIORITY_HIGH] &&
        after.dispatches[THREAD_PRIORITY_HIGH] -
            before.dispatches[THREAD_PRIORITY_HIGH] >= high_count + 2U &&
        after.wakeups > before.wakeups &&
        after.timer_preemptions > before.timer_preemptions &&
        after.context_switches > before.context_switches &&
        (cpus == 1U ? after.lock_contended_acquisitions == 0U :
                      after.lock_contended_acquisitions > 0U) &&
        after.lock_wait_iterations >= after.lock_contended_acquisitions;

    kprint("SCHED_CONTENTION pass=%u cpus=%u online_mask=%llx workers=%u "
           "started=%u completed=%u normal_seen=%u background_seen=%u "
           "wake_seen=%u normal_priority=%u background_priority=%u "
           "wake_priority=%u normal_latency_ms=%llu background_latency_ms=%llu "
           "wake_latency_ms=%llu wake_ready_ms=%llu wake_high_ms=%llu "
           "busy_mask=%llx busy_delta=%llu,%llu,%llu,%llu cpu_mask=%llx "
           "overlap_samples=%llu useful_work=%llu lock_contended=%llu "
           "lock_wait_iterations=%llu wakeups=%llu timer_preemptions=%llu "
           "context_switches=%llu high_dispatches=%llu normal_dispatches=%llu "
           "elapsed_ms=%llu timeout=%u queue_validation_samples=%u "
           "queue_valid=%u drained=%u\n",
           passed ? 1U : 0U, cpus, expected_mask, high_count,
           __atomic_load_n(&scheduler_contention_high_started,
                           __ATOMIC_RELAXED),
           __atomic_load_n(&scheduler_contention_high_completed,
                           __ATOMIC_RELAXED),
           __atomic_load_n(&scheduler_contention_normal_seen,
                           __ATOMIC_RELAXED) ? 1U : 0U,
           __atomic_load_n(&scheduler_contention_background_seen,
                           __ATOMIC_RELAXED) ? 1U : 0U,
           __atomic_load_n(&scheduler_contention_wake_seen,
                           __ATOMIC_RELAXED) ? 1U : 0U,
           (u32)scheduler_contention_normal.selected_priority,
           (u32)scheduler_contention_background.selected_priority,
           (u32)scheduler_contention_wake_background.selected_priority,
           (unsigned long long)(scheduler_contention_normal.first_run_ms -
                                start_ms),
           (unsigned long long)(scheduler_contention_background.first_run_ms -
                                start_ms),
           (unsigned long long)
               scheduler_contention_wake_background.wake_latency_ms,
           (unsigned long long)
               scheduler_contention_wake_background.wake_ready_after_ms,
           (unsigned long long)
               scheduler_contention_wake_background.wake_high_after_ms,
           busy_mask,
           (unsigned long long)busy_delta[0],
           (unsigned long long)busy_delta[1],
           (unsigned long long)busy_delta[2],
           (unsigned long long)busy_delta[3], observed_mask,
           (unsigned long long)overlap_samples,
           (unsigned long long)__atomic_load_n(
               &scheduler_contention_useful_work, __ATOMIC_RELAXED),
           (unsigned long long)after.lock_contended_acquisitions,
           (unsigned long long)after.lock_wait_iterations,
           (unsigned long long)(after.wakeups - before.wakeups),
           (unsigned long long)(after.timer_preemptions -
                                before.timer_preemptions),
           (unsigned long long)(after.context_switches -
                                before.context_switches),
           (unsigned long long)(after.dispatches[THREAD_PRIORITY_HIGH] -
                                before.dispatches[THREAD_PRIORITY_HIGH]),
           (unsigned long long)(after.dispatches[THREAD_PRIORITY_NORMAL] -
                                before.dispatches[THREAD_PRIORITY_NORMAL]),
           (unsigned long long)(end_ms - start_ms), timeout ? 1U : 0U,
           scheduler_contention_controller.queue_validation_samples,
           validated ? 1U : 0U, drained ? 1U : 0U);
    return passed;
}
#endif

#ifdef PITH_DEBUG_BOOT_TESTS
void kernel_debug_scheduler_tests(void)
{
    kernel_thread_t *probe = thread_create("scheduler-probe",
                                            scheduler_probe_entry, NULL);
    if (probe && probe->id != thread_current()->id &&
        probe->state == THREAD_STATE_READY &&
        probe->kernel_stack_base != thread_current()->kernel_stack_base &&
        probe->saved_stack_pointer >= probe->kernel_stack_base &&
        probe->saved_stack_pointer <
            probe->kernel_stack_base + probe->kernel_stack_size &&
        (probe->saved_stack_pointer & 0x0f) == 0) {
        kprint("[OK] Scheduler prepared thread %u with a dedicated stack\n",
               (u32)probe->id);
        if (!thread_destroy(probe))
            kprint("[FAIL] Scheduler probe cleanup failed\n");
    } else {
        kprint("[FAIL] Scheduler thread preparation verification failed\n");
        if (probe)
            thread_destroy(probe);
    }
    scheduler_priority_test();
}

void kernel_debug_runtime_tests(void)
{
#ifdef PITH_HTTP_GET_TEST
    {
        net_device_t *network_device = net_primary_device();
        if (network_device && net_network_configured()) {
            http_response_t http_response;
            http_result_t http_status;
            if (http_get(network_device, "http://example.com/", &http_response,
                         &http_status)) {
                kprint("[OK] HTTP example.com: status=%u body=%u bytes\n",
                       http_response.status_code, (u32)http_response.body_length);
            } else {
                kprint("[WARN] HTTP example.com unavailable (error=%u)\n",
                       (u32)http_status);
            }
        }
    }
#endif
#ifdef PITH_TCP_ECHO_TEST
    {
        net_device_t *network_device = net_primary_device();
        if (network_device && net_network_configured() &&
            net_config()->has_gateway) {
            static const u8 tcp_echo_payload[] = "hello from Mangrove";
            tcp_connection_t *tcp_connection;
            tcp_status_t tcp_status;
            u8 received[sizeof(tcp_echo_payload) - 1U];
            usize received_length = 0;
            u64 tcp_start;
            bool equal = true;

            if (tcp_connect(network_device, *net_gateway_ipv4(), 12345, 5000U,
                            NULL, NULL, &tcp_connection, &tcp_status) &&
                tcp_send(tcp_connection, tcp_echo_payload,
                         sizeof(tcp_echo_payload) - 1U, 5000U, NULL, NULL,
                         &tcp_status) == sizeof(tcp_echo_payload) - 1U) {
                tcp_start = timer_ticks();
                while (timer_ticks() - tcp_start < 5000U &&
                       received_length < sizeof(received)) {
                    received_length += tcp_receive_bytes(
                        tcp_connection, received + received_length,
                        sizeof(received) - received_length, &tcp_status);
                    if (received_length < sizeof(received))
                        __asm__ volatile("hlt");
                }
                for (usize i = 0; i < sizeof(received); i++) {
                    if (i >= received_length ||
                        received[i] != tcp_echo_payload[i])
                        equal = false;
                }
                if (equal && tcp_close(tcp_connection, 5000U, NULL, NULL,
                                       &tcp_status))
                    kprint("[OK] TCP echo validation passed\n");
                else
                    kprint("[WARN] TCP echo validation failed\n");
            } else {
                kprint("[WARN] TCP echo connection unavailable\n");
            }
        }
    }
#endif

    scheduler_timer_test();
    {
        const u64 probe_spin_limit = 100000000ULL;
        bool timer_ready;
        kernel_thread_t *current;
        u64 before = timer_ticks();
        u64 spins = 0;
        u64 saved_slice = 0;
        __asm__ volatile("cli" ::: "memory");
        current = thread_current();
        if (current) {
            saved_slice = current->remaining_time_slice;
            current->remaining_time_slice = ~(u64)0;
        }
        __asm__ volatile("sti" ::: "memory");
        while (timer_ticks() - before < 2 && spins++ < probe_spin_limit)
            __asm__ volatile("pause");
        __asm__ volatile("cli" ::: "memory");
        if (current)
            current->remaining_time_slice = saved_slice;
        timer_ready = timer_ticks() - before >= 2;
        scheduler_sleep_idle_test(timer_ready);
    }
    scheduler_fairness_test();
    scheduler_stress_test();
}
#endif

static bool early_bootstrap(BOOT_INFO *source_boot_info)
{
    x250_boot_trace_event(source_boot_info, 7, true,
                          "early Pith C entry reached");
    x250_boot_trace_event(source_boot_info, 8, false,
                          "copy BOOT_INFO; enable physical direct map");
    boot_info_convert_to_direct_map(source_boot_info);
    BOOT_INFO *BootInfo = &kernel_boot_info;
    x250_boot_trace_event(BootInfo, 8, true,
                          "BOOT_INFO direct-map conversion complete");
#if BOOT_HANDOFF_CHECKPOINT == 7
    boot_checkpoint_mark((volatile u32 *)BootInfo->FramebufferBase,
                         BootInfo->FramebufferWidth, BootInfo->FramebufferHeight,
                         BootInfo->PixelsPerScanLine, 0x00ff00ffU);
    boot_checkpoint_halt();
#endif
    x250_boot_trace_event(BootInfo, 36, false,
                          "establish BSP CPU-local GS base");
    if (!cpu_activate_kernel_gs(cpu_bootstrap_local())) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    x250_boot_trace_event(BootInfo, 36, true,
                          "BSP CPU-local GS base established");
    x250_boot_trace_event(BootInfo, 9, false,
                          "initialize framebuffer and terminal");
    u32 *fb = (u32 *)BootInfo->FramebufferBase;
    usize total_pixels = BootInfo->FramebufferSize / sizeof(u32);
#if !X250_BOOT_TRACE
    for (usize i = 0; i < total_pixels; i++)
        fb[i] = terminal_palette_rgb[TERMINAL_COLOR_BLACK];
#else
    (void)fb;
    (void)total_pixels;
#endif

    x250_boot_trace_event(BootInfo, 24, false,
                          "09.1 initialize framebuffer state");
    framebuffer_init(BootInfo);
    x250_boot_trace_event(BootInfo, 24, true,
                          "09.1 framebuffer state initialized");
    x250_boot_trace_event(BootInfo, 25, false,
                          "09.2 switch trace to framebuffer API");
    x250_boot_trace_use_framebuffer_api();
    x250_boot_trace_event(BootInfo, 25, true,
                          "09.2 framebuffer API is active");
    x250_boot_trace_event(BootInfo, 26, false,
                          "09.3 parse embedded font");
    font_init(BootInfo);
    x250_boot_trace_event(BootInfo, 26, true,
                          "09.3 embedded font initialized");
    x250_boot_trace_event(BootInfo, 27, false,
                          "09.4 initialize terminal");
    terminal_init(BootInfo);
    x250_boot_trace_event(BootInfo, 33, false,
                          "09.5 initialize console");
    console_init();
    x250_boot_trace_event(BootInfo, 33, true,
                          "09.5 console initialized");
    x250_boot_trace_event(BootInfo, 9, true,
                          "framebuffer and terminal initialized");
    kprint("%s %s\n", MANGROVE_NAME, MANGROVE_VERSION);
#if BOOT_HANDOFF_CHECKPOINT == 8
    boot_checkpoint_mark((volatile u32 *)BootInfo->FramebufferBase,
                         BootInfo->FramebufferWidth, BootInfo->FramebufferHeight,
                         BootInfo->PixelsPerScanLine, 0x0000ff00U);
    boot_checkpoint_halt();
#endif

    x250_boot_trace_event(BootInfo, 10, false,
                          "initialize CPU descriptors and interrupt devices");
    if (!gdt_init()) {
        kprint("[FAIL] CPU descriptor initialization failed\n");
        return false;
    }
    idt_init();
    pic_disable();
    pit_init(TIMER_FREQUENCY);
    timer_init();
    keyboard_init();
    x250_boot_trace_event(BootInfo, 10, true,
                          "CPU descriptors and interrupt devices ready");
    KERNEL_BOOT_DEBUG_LOG(
        "[BOOT] CPU descriptors, timer, and interrupt foundations ready\n");

    x250_boot_trace_event(BootInfo, 11, false,
                          "initialize physical memory manager");
    if (!pmm_init(BootInfo)) {
        kprint("[BOOT] physical memory map cannot be managed safely\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 11, true,
                          "physical memory manager ready");
    x250_boot_trace_event(BootInfo, 12, false,
                          "discover ACPI platform tables");
    acpi_init(BootInfo);
    if (!acpi_present())
        kprint("[WARN] ACPI platform unavailable\n");
    x250_boot_trace_event(BootInfo, 12, true,
                          "ACPI discovery returned");
    x250_boot_trace_event(BootInfo, 13, false,
                          "establish BSP CPU cache policy");
    if (!x86_cache_init_bsp()) {
        kprint("[BOOT] CPU cache policy cannot be established safely\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 13, true,
                          "BSP CPU cache policy ready");
    x250_boot_trace_event(BootInfo, 14, false,
                          "create permanent page-table root");
    vmm_init();
    phys_addr_t k_pml4_phys = pmm_alloc_frame();
    page_table_t *k_pml4 = (page_table_t *)phys_to_virt(k_pml4_phys);
    if (!k_pml4)
        return false;
    for (int i = 0; i < 512; i++)
        k_pml4->entries[i] = 0;

    vmm_set_kernel_pml4(k_pml4_phys);
    if (!map_kernel_image_range(k_pml4,
            (uintptr_t)__kernel_text_virt_start,
            (uintptr_t)__kernel_text_virt_end, 0) ||
        !map_kernel_image_range(k_pml4,
            (uintptr_t)__kernel_rodata_virt_start,
            (uintptr_t)__kernel_rodata_virt_end, PTE_NX) ||
        !map_kernel_image_range(k_pml4,
            (uintptr_t)__kernel_data_virt_start,
            (uintptr_t)__kernel_data_virt_end, PTE_READWRITE | PTE_NX) ||
        !map_kernel_image_range(k_pml4,
            (uintptr_t)__kernel_bss_virt_start,
            (uintptr_t)__stack_guard_bottom, PTE_READWRITE | PTE_NX) ||
        !map_kernel_image_range(k_pml4,
            (uintptr_t)__stack_bottom,
            (uintptr_t)__kernel_bss_virt_end, PTE_READWRITE | PTE_NX)) {
        kprint("[BOOT] kernel image mapping failed\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 14, true,
                          "kernel image mapped in permanent root");

    x250_boot_trace_event(BootInfo, 15, false,
                          "map RAM and permanent framebuffer");
    MANGROVE_MEMORY_DESCRIPTOR *mmap =
        (MANGROVE_MEMORY_DESCRIPTOR *)BootInfo->MemoryMap;
    u64 mmap_entries = BootInfo->MemoryMapSize / BootInfo->DescriptorSize;
    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc =
            (MANGROVE_MEMORY_DESCRIPTOR *)((u64)mmap +
                i * BootInfo->DescriptorSize);
        if (!direct_map_memory_type(desc->Type))
            continue;
        if (!vmm_map_physical_ram(desc->PhysicalStart, desc->NumberOfPages)) {
            kprint("[BOOT] physical memory direct-map setup failed\n");
            return false;
        }
    }

    bool framebuffer_wc;
    void *framebuffer_mmio = vmm_map_boot_framebuffer(BootInfo, &framebuffer_wc);
    if (!framebuffer_mmio) {
        kprint("[BOOT] framebuffer MMIO mapping failed\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 15, true,
                          "RAM and framebuffer mappings prepared");

    /* Drain the loader alias before activating the sole permanent mapping.
     * This is a boot transition, not an in-place SMP cache-type change. */
    x250_boot_trace_event(BootInfo, 16, false,
                          "activate permanent kernel page tables");
    framebuffer_set_mapping(framebuffer_mmio, framebuffer_wc);
    BootInfo->FramebufferBase = framebuffer_mmio;
    x86_cache_activate_boot_root(k_pml4_phys);
    x250_boot_trace_rebind(framebuffer_mmio, BootInfo->FramebufferWidth,
                           BootInfo->FramebufferHeight,
                           BootInfo->PixelsPerScanLine);
    x250_boot_trace_event(BootInfo, 16, true,
                          "permanent kernel page tables active");

    x250_boot_trace_event(BootInfo, 17, false,
                          "enable direct map and verify mappings");
    vmm_enable_direct_map();
    pmm_enable_direct_map();
    k_pml4 = vmm_get_kernel_pml4();
    if (!vmm_direct_map_valid(k_pml4_phys) ||
        vmm_kernel_mapping_present((void *)__stack_guard_bottom) ||
        vmm_kernel_mapping_present((void *)(uintptr_t)KERNEL_PHYS_BASE) ||
        !vmm_kernel_mapping_supervisor((void *)(uintptr_t)PHYS_MAP_BASE) ||
        !vmm_kernel_mapping_supervisor((void *)framebuffer_mmio) ||
        !vmm_kernel_mappings_supervisor_only()) {
        kprint("[BOOT] permanent kernel mapping invariant failed\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 17, true,
                          "direct map and mapping invariants ready");

    x250_boot_trace_event(BootInfo, 18, false,
                          "initialize kernel heap");
    if (!heap_init()) {
        kprint("[BOOT] initial kernel heap mapping failed\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 18, true,
                          "kernel heap ready");
    x250_boot_trace_event(BootInfo, 19, false,
                          "allocate guarded exception stacks");
    if (!gdt_install_guarded_ists(cpu_bootstrap_local())) {
        kprint("[BOOT] guarded exception-stack allocation failed\n");
        return false;
    }
    x250_boot_trace_event(BootInfo, 19, true,
                          "guarded exception stacks ready");
    framebuffer_enable_backbuffer();
    x250_boot_trace_event(BootInfo, 20, false,
                          "initialize clocks and embedded controller");
    KERNEL_BOOT_DEBUG_LOG("[BOOT] memory, permanent VMM, and heap ready\n");
    if (timer_monotonic_init())
        KERNEL_BOOT_DEBUG_LOG(
            "[BOOT] monotonic clocksource ready\n");
    else
        kprint("[WARN] degraded PIT clocksource active\n");
#if defined(RTC_UTC_TRUST) && RTC_UTC_TRUST == 1
    timekeeping_init(true);
#else
    timekeeping_init(false);
#endif
    if (!timekeeping_realtime_available())
        kprint("[WARN] realtime clock unavailable\n");
    (void)ec_init();
    x250_boot_trace_event(BootInfo, 20, true,
                          "early hardware initialization complete");
    return true;
}

static void start_pid1(void)
{
    x250_boot_trace_event(&kernel_boot_info, 22, false,
                          "prepare PID 1 process and Shoot image");
    /* Represent the loaded userspace image as PID 1 and expose
     * only explicitly installed, process-local capabilities to it. */
    __asm__ volatile("cli" ::: "memory");
    const char *creation_stage = NULL;
    int creation_status = VFS_OK;
    /* Guarded kernel-stack mappings are published into every process root
     * and require remote CPUs to acknowledge the VMM shootdown. Keep IF on
     * while constructing the suspended child; other CPUs are idle until its
     * resources are fully prepared. */
    __asm__ volatile("sti" ::: "memory");
    process_t *ring3_process = process_create_with_failure(
        "sprout", NULL, thread_current(), &creation_stage,
        &creation_status);
    __asm__ volatile("cli" ::: "memory");
    if (!ring3_process) {
        x250_boot_trace_result(22U, false, "PID 1 process creation", true,
                               true, creation_status, "FAILED",
                               creation_stage ? creation_stage :
                                   "unknown stage");
        kprint("[FAIL] Ring 3 process creation failed at %s (status=%d)\n",
               creation_stage ? creation_stage : "unknown stage",
               creation_status);
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (!ring3_process->address_space) {
        x250_boot_trace_result(22U, false, "PID 1 address space", true,
                               false, 0, "FAILED",
                               "process has no address space");
        kprint("[FAIL] Ring 3 process has no address space\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (ring3_process->address_space == vmm_get_kernel_pml4()) {
        x250_boot_trace_result(22U, false, "PID 1 address space", true,
                               false, 0, "FAILED",
                               "aliases kernel address space");
        kprint("[FAIL] Ring 3 process aliases the kernel address space\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    strncpy(ring3_process->executable_path, "/core/sprout",
            sizeof(ring3_process->executable_path) - 1U);
    ring3_process->executable_path[
        sizeof(ring3_process->executable_path) - 1U] = '\0';
    if (!process_assign_system_service(ring3_process, MG_SERVICE_SPROUT)) {
        x250_boot_trace_result(22U, false, "PID 1 service identity", true,
                               false, 0, "FAILED",
                               "Sprout service assignment failed");
        kprint("[FAIL] Sprout service identity assignment failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Install only the capability Sprout needs for console access. The
     * userspace value is a generation-tagged handle, never a kernel pointer. */
    kernel_object_t *console_object = object_console_create();
    process_handle_t console_handle = 0;
    if (!console_object ||
        !process_handle_install(ring3_process, console_object,
                                PROCESS_HANDLE_RIGHT_READ |
                                PROCESS_HANDLE_RIGHT_WRITE, &console_handle)) {
        x250_boot_trace_result(22U, false, "PID 1 console handle", true,
                               false, 0, "FAILED",
                               "console capability installation failed");
        kprint("[FAIL] PID 1 console handle installation failed\n");
        if (console_object) object_release(console_object);
        for (;;) __asm__ volatile("cli; hlt");
    }
    object_release(console_object); /* the process table owns its reference */
#ifdef PITH_DEBUG_BOOT_TESTS
    if (console_handle != PROCESS_INITIAL_CONSOLE_HANDLE ||
        process_handle_lookup(ring3_process, 0xdeadbeef,
                              OBJECT_TYPE_CONSOLE,
                              OBJECT_RIGHT_WRITE) != NULL ||
        process_handle_close(ring3_process, 0xdeadbeef)) {
        x250_boot_trace_result(22U, false, "PID 1 console handle validation",
                               true, false, 0, "FAILED",
                               "initial handle or invalid-handle check failed");
        kprint("[FAIL] PID 1 console handle validation failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
#endif

    uintptr_t user_entry;
    uintptr_t user_stack;
    /* Keep filesystem I/O on the shared kernel address space while loading
     * the first image; switch to PID 1's table only after the image is ready. */
    /* /core/sprout may be read from USB-backed storage.  Do not hold the
       process-construction critical section across blocking I/O: the xHCI
       service owner and timer need normal IRQ delivery while it completes. */
    __asm__ volatile("sti" ::: "memory");
    vmm_switch_address_space(vmm_get_kernel_pml4());
    if (!elf_load_process(ring3_process, "/core/sprout", &user_entry,
                          &user_stack)) {
        x250_boot_trace_result(22U, false, "PID 1 image load", true,
                               false, 0, "FAILED",
                               "could not load /core/sprout ELF");
        kprint("[FAIL] Could not load /core/sprout ELF\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    const char *sprout_argv[] = { "/core/sprout" };
    if (!process_setup_argv(ring3_process, sprout_argv, 1U)) {
        x250_boot_trace_result(22U, false, "PID 1 arguments", true,
                               false, 0, "FAILED",
                               "could not construct /core/sprout arguments");
        kprint("[FAIL] Could not construct /core/sprout arguments\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (!process_publish_launch(ring3_process)) {
        x250_boot_trace_result(22U, false, "PID 1 publication", true,
                               false, 0, "FAILED",
                               "process publication failed");
        kprint("[FAIL] PID 1 process publication failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Create terminal workers while the kernel address space is active and
     * interrupts are enabled.  Their allocations may grow the heap, which
     * can require the cross-CPU mapping publication path. */
    __asm__ volatile("sti" ::: "memory");
    (void)terminal_cursor_blink_start();
    (void)terminal_presentation_start();
    __asm__ volatile("cli" ::: "memory");
    vmm_switch_address_space(ring3_process->address_space);
#if !X250_BOOT_TRACE
    terminal_clear();
#endif
    x250_boot_trace_result(22U, true, "PID 1 preparation", true, false, 0,
                           "READY", "Sprout loaded and published");
    x250_boot_trace_event(&kernel_boot_info, 23, false,
                          "enter PID 1 userspace");
    __asm__ volatile("sti" ::: "memory");
    process_user_context_enter();

    for (;;)
    {
        asm volatile("hlt");
    }
}

#ifdef TIMEKEEPING_QMP_TEST
/* Dedicated test-kernel probe. The host stops the VM after the first marker,
 * waits a controlled interval, then resumes it. The bounded guest interval
 * distinguishes RTC policy from the monotonic hardware clock. */
static void timekeeping_qmp_probe(void)
{
    timekeeping_realtime_sample_t before_wall;
    timekeeping_realtime_sample_t after_wall;
    mg_monotonic_info_t source;
    u64 before_ms;
    u64 after_ms;
    bool before_valid;
    bool after_valid;

    timer_monotonic_info(&source);
    before_ms = timer_uptime_ms();
    before_valid = timekeeping_realtime_sample(&before_wall);
    kprint("[CLOCK-QMP] before mono=%llu wall_valid=%u wall=%lld source=%u quality=%u\n",
           (unsigned long long)before_ms, before_valid ? 1U : 0U,
           (long long)(before_valid ? before_wall.time.seconds : 0),
           source.source, source.quality);

    while (timer_uptime_ms() - before_ms < 5000U)
        __asm__ volatile("pause");

    after_ms = timer_uptime_ms();
    after_valid = timekeeping_realtime_sample(&after_wall);
    kprint("[CLOCK-QMP] after mono=%llu wall_valid=%u wall=%lld\n",
           (unsigned long long)after_ms, after_valid ? 1U : 0U,
           (long long)(after_valid ? after_wall.time.seconds : 0));
}
#endif

void kmain_high(BOOT_INFO *source_boot_info)
{
    if (!early_bootstrap(source_boot_info))
        return;
    if (!kernel_bringup()) {
        kprint("[FAIL] Required kernel bring-up phase failed\n");
        /* Console rendering can repaint the shared framebuffer over the
         * temporary diagnostic layer. Leave the final report as the last
         * framebuffer writer before halting this failed boot. */
        x250_boot_trace_refresh();
        for (;;) __asm__ volatile("cli; hlt");
    }
#ifdef TIMEKEEPING_QMP_TEST
    timekeeping_qmp_probe();
#endif
    x250_boot_trace_event(&kernel_boot_info, 22, false,
                          "prepare PID 1 and transfer to Shoot");
    start_pid1();
}
