#include <bootinfo.h>
#include <gdt.h>
#include <idt.h>
#include <pmm.h>
#include <memory_types.h>
#include <vmm.h>
#include <mangrove_version.h>
#include <heap.h>
#include <pic.h>
#include <pit.h>
#include <timer.h>
#include <keyboard.h>
#include <font.h>
#include <terminal.h>
#include <framebuffer.h>
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

#ifndef NULL
#define NULL ((void*)0)
#endif

extern char __stack_top[];
extern char __stack_bottom[];
extern char __kernel_text_virt_start[];
extern char __kernel_text_virt_end[];
extern char __kernel_rodata_virt_start[];
extern char __kernel_rodata_virt_end[];
extern char __kernel_data_virt_start[];
extern char __kernel_data_virt_end[];
extern char __kernel_bss_virt_start[];
extern char __kernel_bss_virt_end[];
extern void ring3_enter(uintptr_t entry, uintptr_t stack_pointer, uintptr_t argc, uintptr_t argv);

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
        phys_addr_t phys = kernel_image_virt_to_phys(page);
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

            if (tcp_connect(network_device, *net_gateway_ipv4(), 12345,
                            &tcp_connection, &tcp_status) &&
                tcp_send(tcp_connection, tcp_echo_payload,
                         sizeof(tcp_echo_payload) - 1U, &tcp_status)) {
                tcp_start = timer_ticks();
                while (timer_ticks() - tcp_start < 5000U &&
                       received_length < sizeof(received)) {
                    received_length += tcp_receive_bytes(
                        tcp_connection, received + received_length,
                        sizeof(received) - received_length);
                    if (received_length < sizeof(received))
                        __asm__ volatile("hlt");
                }
                for (usize i = 0; i < sizeof(received); i++) {
                    if (i >= received_length ||
                        received[i] != tcp_echo_payload[i])
                        equal = false;
                }
                if (equal && tcp_close(tcp_connection, &tcp_status))
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
    boot_info_convert_to_direct_map(source_boot_info);
    BOOT_INFO *BootInfo = &kernel_boot_info;
    u32 *fb = (u32 *)BootInfo->FramebufferBase;
    usize total_pixels = BootInfo->FramebufferSize / sizeof(u32);
    for (usize i = 0; i < total_pixels; i++)
        fb[i] = 0xFFFFFFFF;

    framebuffer_init(BootInfo);
    font_init(BootInfo);
    terminal_init(BootInfo);
    console_init();
    kprint("%s %s\n\nStarting system...\n", MANGROVE_NAME,
           MANGROVE_VERSION);

    gdt_init();
    idt_init();
    pic_disable();
    pit_init(TIMER_FREQUENCY);
    timer_init();
    keyboard_init();
    kprint("Input ready\n");
    KERNEL_BOOT_DEBUG_LOG(
        "[BOOT] CPU descriptors, timer, and interrupt foundations ready\n");

    pmm_init(BootInfo);
    acpi_init(BootInfo);
    if (!acpi_present())
        kprint("[WARN] ACPI platform unavailable\n");

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
            (uintptr_t)__kernel_bss_virt_end, PTE_READWRITE | PTE_NX)) {
        kprint("[BOOT] kernel image mapping failed\n");
        return false;
    }

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

    void *framebuffer_mmio = vmm_map_mmio(
        (phys_addr_t)BootInfo->FramebufferPhysicalBase,
        BootInfo->FramebufferSize);
    if (!framebuffer_mmio) {
        kprint("[BOOT] framebuffer MMIO mapping failed\n");
        return false;
    }
    framebuffer_set_mmio(framebuffer_mmio);
    BootInfo->FramebufferBase = framebuffer_mmio;

    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "jmp 1f\n\t"
        "1:\n\t"
        :: "r"(k_pml4_phys) : "memory");

    vmm_enable_direct_map();
    pmm_enable_direct_map();
    k_pml4 = vmm_get_kernel_pml4();
    if (!vmm_direct_map_valid(k_pml4_phys) ||
        vmm_kernel_mapping_present((void *)(uintptr_t)KERNEL_PHYS_BASE) ||
        !vmm_kernel_mapping_supervisor((void *)(uintptr_t)PHYS_MAP_BASE) ||
        !vmm_kernel_mapping_supervisor((void *)framebuffer_mmio) ||
        !vmm_kernel_mappings_supervisor_only()) {
        kprint("[BOOT] permanent kernel mapping invariant failed\n");
        return false;
    }

    heap_init();
    framebuffer_enable_backbuffer();
    KERNEL_BOOT_DEBUG_LOG("[BOOT] memory, permanent VMM, and heap ready\n");
    if (timer_monotonic_init())
        KERNEL_BOOT_DEBUG_LOG(
            "[BOOT] interrupt-independent monotonic clock ready\n");
    else
        kprint("[WARN] monotonic platform timing unavailable\n");
    (void)ec_init();
    kprint("Platform ready\n");
    return true;
}

static void start_pid1(void)
{
    /* Represent the loaded userspace image as PID 1 and expose
     * only explicitly installed, process-local capabilities to it. */
    __asm__ volatile("cli" ::: "memory");
    process_t *ring3_process = process_create("ring3-test", NULL,
                                              thread_current());
    if (!ring3_process || !ring3_process->address_space ||
        ring3_process->address_space == vmm_get_kernel_pml4()) {
        kprint("[FAIL] Ring 3 process creation failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    user_identity_t autologin_identity;
    if (identity_registry_autologin_user(&autologin_identity)) {
        if (!process_assign_initial_credentials(ring3_process,
                                                &autologin_identity)) {
            kprint("[FAIL] Initial userspace identity assignment failed\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
    } else if (!process_assign_system_credentials(ring3_process)) {
        kprint("[FAIL] Initial system session assignment failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Install only the capability Sprout needs for this milestone.  The
     * userspace value is a generation-tagged handle, never a kernel pointer. */
    kernel_object_t *console_object = object_console_create();
    process_handle_t console_handle = 0;
    if (!console_object ||
        !process_handle_install(ring3_process, console_object,
                                PROCESS_HANDLE_RIGHT_READ |
                                PROCESS_HANDLE_RIGHT_WRITE, &console_handle)) {
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
        kprint("[FAIL] PID 1 console handle validation failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
#endif

    uintptr_t user_entry;
    uintptr_t user_stack;
    /* Keep filesystem I/O on the shared kernel address space while loading
     * the first image; switch to PID 1's table only after the image is ready. */
    /* /bin/sprout may be read from USB-backed storage.  Do not hold the
       process-construction critical section across blocking I/O: the xHCI
       service owner and timer need normal IRQ delivery while it completes. */
    __asm__ volatile("sti" ::: "memory");
    vmm_switch_address_space(vmm_get_kernel_pml4());
    if (!elf_load_process(ring3_process, "/bin/sprout", &user_entry,
                          &user_stack)) {
        kprint("[FAIL] Could not load /bin/sprout ELF\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (!process_setup_cmdline(ring3_process, "/bin/sprout")) {
        kprint("[FAIL] Could not construct /bin/sprout arguments\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    __asm__ volatile("cli" ::: "memory");
    vmm_switch_address_space(ring3_process->address_space);
    kprint("Starting Sprout...\n");
    terminal_clear();
    __asm__ volatile("sti" ::: "memory");
    ring3_enter(user_entry, ring3_process->user_stack_sp,
                ring3_process->user_argc, ring3_process->user_argv);

    for (;;)
    {
        asm volatile("hlt");
    }
}

void kmain_high(BOOT_INFO *source_boot_info)
{
    if (!early_bootstrap(source_boot_info))
        return;
    if (!kernel_bringup()) {
        kprint("[FAIL] Required kernel bring-up phase failed\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    start_pid1();
}
