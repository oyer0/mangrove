/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded native storage and e1000 receive workloads for SMP guest tests. */
#include <mangrove.h>
#include <mg/net.h>
#include <mg/thread.h>
#include <mg/inspection.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#define DEVICE_SMP_MAX_CPUS 4U
#define DEVICE_SMP_MAX_WORKERS 5U
#define DEVICE_SMP_STORAGE_WORKERS DEVICE_SMP_MAX_CPUS
#define DEVICE_SMP_FILE_BYTES (128U * 1024U)
#define DEVICE_SMP_ECHOES 16U
#define DEVICE_SMP_START_TIMEOUT_MS 10000U
#define DEVICE_SMP_WORK_TIMEOUT_MS 30000U

typedef struct {
    u32 index;
} device_smp_worker_argument_t;

static atomic_bool device_smp_begin;
static atomic_bool device_smp_abort;
static atomic_bool device_smp_compute_run;
static atomic_bool device_smp_monitor_run;
static atomic_bool device_smp_device_active;
static atomic_bool device_smp_storage_mode;
static atomic_uint device_smp_ready;
static atomic_uint device_smp_spin_started;
static atomic_uint device_smp_spin_finished;
static atomic_uint device_smp_io_started;
static atomic_uint device_smp_io_finished;
static atomic_uint device_smp_io_active;
static atomic_uint device_smp_io_max_active;
static atomic_uint device_smp_multicore_samples;
static atomic_uint device_smp_overlap_samples;
static atomic_ullong device_smp_work_units;
static mg_thread_created_t device_smp_threads[2U * DEVICE_SMP_MAX_WORKERS + 2U];
static device_smp_worker_argument_t device_smp_spin_arguments[DEVICE_SMP_MAX_WORKERS];
static device_smp_worker_argument_t device_smp_io_arguments[DEVICE_SMP_STORAGE_WORKERS];
static u32 device_smp_thread_count;
static char device_smp_paths[DEVICE_SMP_STORAGE_WORKERS][112];
static u8 device_smp_payloads[DEVICE_SMP_STORAGE_WORKERS][DEVICE_SMP_FILE_BYTES];
static u8 device_smp_observed[DEVICE_SMP_STORAGE_WORKERS][DEVICE_SMP_FILE_BYTES];
static mg_result_t device_smp_io_status[DEVICE_SMP_STORAGE_WORKERS];

static bool device_smp_cpu_snapshot(u32 expected_cpus, u32 *online_mask,
                                   u64 busy_ticks[DEVICE_SMP_MAX_CPUS])
{
    mg_cpu_info_t entries[MG_CPU_SNAPSHOT_PAGE_MAX];
    u32 count = 0U;
    u32 total = 0U;
    u32 mask = 0U;
    mg_cpu_snapshot_request_t request = {
        .offset = 0U,
        .result_capacity = MG_CPU_SNAPSHOT_PAGE_MAX,
        .result = entries,
        .out_count = &count,
        .out_total = &total,
    };

    if (!online_mask || !busy_ticks ||
        cpu_snapshot(&request) != MG_OK || total > MG_CPU_SNAPSHOT_PAGE_MAX)
        return false;
    for (u32 cpu = 0U; cpu < DEVICE_SMP_MAX_CPUS; cpu++)
        busy_ticks[cpu] = 0U;
    for (u32 index = 0U; index < count; index++) {
        const mg_cpu_info_t *entry = &entries[index];
        if (!(entry->flags & MG_CPU_FLAG_ONLINE)) continue;
        if (entry->index >= DEVICE_SMP_MAX_CPUS) return false;
        mask |= 1U << entry->index;
        busy_ticks[entry->index] = entry->busy_ticks;
    }
    *online_mask = mask;
    return mask == ((1U << expected_cpus) - 1U);
}

static bool device_smp_process_is_multicore(u64 pid)
{
    mg_process_info_t entries[MG_PROCESS_SNAPSHOT_PAGE_MAX];
    u32 offset = 0U;
    u32 count = 0U;
    u32 total = 0U;

    if (!pid) return false;
    do {
        mg_process_snapshot_request_t request = {
            .offset = offset,
            .result_capacity = MG_PROCESS_SNAPSHOT_PAGE_MAX,
            .result = entries,
            .out_count = &count,
            .out_total = &total,
        };
        if (process_snapshot(&request) != MG_OK || total > 1024U)
            return false;
        for (u32 index = 0U; index < count; index++) {
            if (entries[index].pid == pid)
                return entries[index].running_cpu == MG_PROCESS_CPU_MULTIPLE;
        }
        offset += count;
    } while (count && offset < total);
    return false;
}

static void device_smp_raise_max(atomic_uint *maximum, u32 value)
{
    u32 observed = atomic_load_explicit(maximum, memory_order_relaxed);
    while (observed < value &&
           !atomic_compare_exchange_weak_explicit(
               maximum, &observed, value, memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

static mg_thread_result_t device_smp_spin_worker(void *raw)
{
    device_smp_worker_argument_t *argument = raw;
    volatile u64 value = (u64)argument->index + 1U;
    u64 iterations = 0U;

    atomic_fetch_add_explicit(&device_smp_ready, 1U, memory_order_release);
    while (!atomic_load_explicit(&device_smp_begin, memory_order_acquire) &&
           !atomic_load_explicit(&device_smp_abort, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
    if (atomic_load_explicit(&device_smp_abort, memory_order_acquire))
        return 1U;
    atomic_fetch_add_explicit(&device_smp_spin_started, 1U,
                              memory_order_release);
    while (atomic_load_explicit(&device_smp_compute_run,
                                memory_order_acquire)) {
        value = value * 6364136223846793005ULL + (iterations | 1U);
        iterations++;
        if ((iterations & 0xfffU) == 0U)
            atomic_fetch_add_explicit(&device_smp_work_units, 4096U,
                                      memory_order_relaxed);
    }
    if (!value) atomic_fetch_add_explicit(&device_smp_work_units, 1U,
                                          memory_order_relaxed);
    atomic_fetch_add_explicit(&device_smp_spin_finished, 1U,
                              memory_order_release);
    return iterations ? 0U : 1U;
}

static mg_thread_result_t device_smp_monitor_worker(void *raw)
{
    u64 pid = process_current_pid();
    (void)raw;
    atomic_fetch_add_explicit(&device_smp_ready, 1U, memory_order_release);
    while (atomic_load_explicit(&device_smp_monitor_run,
                                memory_order_acquire)) {
        if (atomic_load_explicit(&device_smp_device_active,
                                 memory_order_acquire) &&
            device_smp_process_is_multicore(pid)) {
            atomic_fetch_add_explicit(&device_smp_multicore_samples, 1U,
                                      memory_order_relaxed);
            bool device_operation = atomic_load_explicit(
                &device_smp_storage_mode, memory_order_acquire)
                ? atomic_load_explicit(&device_smp_io_active,
                                       memory_order_acquire) != 0U
                : atomic_load_explicit(&device_smp_device_active,
                                       memory_order_acquire);
            if (device_operation)
                atomic_fetch_add_explicit(&device_smp_overlap_samples, 1U,
                                          memory_order_relaxed);
        }
        (void)process_sleep_ms(1U);
    }
    return 0U;
}

static u8 device_smp_payload_byte(u32 worker, u32 offset, const char *run)
{
    return (u8)(offset * 37U + (offset >> 8) * 11U + worker * 53U +
                (u8)run[offset & 15U]);
}

static mg_result_t device_smp_verify_file(u32 worker)
{
    mg_result_t opened = file_open(device_smp_paths[worker], MG_OPEN_READ);
    if (opened <= 0) return opened ? opened : MG_ERR_IO;
    mg_handle_t handle = (mg_handle_t)opened;
    usize offset = 0U;
    mg_result_t result = MG_OK;

    while (offset < DEVICE_SMP_FILE_BYTES) {
        usize amount = DEVICE_SMP_FILE_BYTES - offset;
        if (amount > 8192U) amount = 8192U;
        mg_result_t received = object_read(handle,
            device_smp_observed[worker] + offset, amount);
        if (received != (mg_result_t)amount) {
            result = received < 0 ? received : MG_ERR_IO;
            break;
        }
        for (usize index = 0U; index < amount; index++) {
            u32 position = (u32)(offset + index);
            if (device_smp_observed[worker][position] !=
                device_smp_payloads[worker][position]) {
                result = MG_ERR_IO;
                break;
            }
        }
        if (result != MG_OK) break;
        offset += amount;
    }
    if (result == MG_OK) {
        u8 extra = 0U;
        mg_result_t eof = object_read(handle, &extra, 1U);
        if (eof != 0) result = eof < 0 ? eof : MG_ERR_IO;
    }
    mg_result_t closed = handle_close(handle);
    return result != MG_OK ? result : closed;
}

static mg_thread_result_t device_smp_storage_worker(void *raw)
{
    device_smp_worker_argument_t *argument = raw;
    u32 worker = argument->index;
    mg_handle_t output = 0U;
    mg_result_t result;

    atomic_fetch_add_explicit(&device_smp_ready, 1U, memory_order_release);
    while (!atomic_load_explicit(&device_smp_begin, memory_order_acquire) &&
           !atomic_load_explicit(&device_smp_abort, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
    if (atomic_load_explicit(&device_smp_abort, memory_order_acquire))
        return 1U;

    u32 active = atomic_fetch_add_explicit(&device_smp_io_active, 1U,
                                            memory_order_acq_rel) + 1U;
    device_smp_raise_max(&device_smp_io_max_active, active);
    atomic_fetch_add_explicit(&device_smp_io_started, 1U,
                              memory_order_release);
    result = file_create_contents(device_smp_paths[worker],
                                  device_smp_payloads[worker],
                                  DEVICE_SMP_FILE_BYTES, &output);
    if (result == MG_OK && !output) result = MG_ERR_IO;
    if (output) {
        mg_result_t closed = handle_close(output);
        output = 0U;
        if (result == MG_OK && closed != MG_OK) result = closed;
    }
    if (result == MG_OK) result = device_smp_verify_file(worker);
    device_smp_io_status[worker] = result;
    atomic_fetch_sub_explicit(&device_smp_io_active, 1U,
                              memory_order_release);
    atomic_fetch_add_explicit(&device_smp_io_finished, 1U,
                              memory_order_release);
    return result == MG_OK ? 0U : 1U;
}

static void device_smp_reset(void)
{
    atomic_store_explicit(&device_smp_begin, false, memory_order_relaxed);
    atomic_store_explicit(&device_smp_abort, false, memory_order_relaxed);
    atomic_store_explicit(&device_smp_compute_run, true, memory_order_relaxed);
    atomic_store_explicit(&device_smp_monitor_run, true, memory_order_relaxed);
    atomic_store_explicit(&device_smp_device_active, false, memory_order_relaxed);
    atomic_store_explicit(&device_smp_storage_mode, false,
                          memory_order_relaxed);
    atomic_store_explicit(&device_smp_ready, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_spin_started, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_spin_finished, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_io_started, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_io_finished, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_io_active, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_io_max_active, 0U, memory_order_relaxed);
    atomic_store_explicit(&device_smp_multicore_samples, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&device_smp_overlap_samples, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&device_smp_work_units, 0U, memory_order_relaxed);
    memset(device_smp_io_status, 0, sizeof(device_smp_io_status));
    device_smp_thread_count = 0U;
}

static bool device_smp_start_thread(mg_thread_entry_t entry, void *argument)
{
    if (device_smp_thread_count >=
        sizeof(device_smp_threads) / sizeof(device_smp_threads[0]))
        return false;
    if (mg_thread_create(&device_smp_threads[device_smp_thread_count], entry,
                         argument) != MG_OK)
        return false;
    device_smp_thread_count++;
    return true;
}

static bool device_smp_wait_ready(u32 expected)
{
    u64 start = uptime_ms();
    if (start == (u64)-1) return false;
    while (atomic_load_explicit(&device_smp_ready, memory_order_acquire) !=
           expected) {
        u64 now = uptime_ms();
        if (now == (u64)-1 || now < start ||
            now - start > DEVICE_SMP_START_TIMEOUT_MS)
            return false;
        (void)process_yield();
    }
    return true;
}

static bool device_smp_finish_threads(u32 spin_count, bool abort)
{
    bool passed = !abort;
    atomic_store_explicit(&device_smp_device_active, false,
                          memory_order_release);
    atomic_store_explicit(&device_smp_monitor_run, false,
                          memory_order_release);
    atomic_store_explicit(&device_smp_compute_run, false,
                          memory_order_release);
    atomic_store_explicit(&device_smp_abort, abort, memory_order_release);
    atomic_store_explicit(&device_smp_begin, true, memory_order_release);
    for (u32 index = 0U; index < device_smp_thread_count; index++) {
        mg_thread_result_t result = 1U;
        if (mg_thread_join(device_smp_threads[index].handle, &result) != MG_OK ||
            result != 0U)
            passed = false;
    }
    if (atomic_load_explicit(&device_smp_spin_finished,
                             memory_order_acquire) != spin_count)
        passed = false;
    return passed;
}

static bool device_smp_start_common(u32 cpus, u32 io_workers,
                                    u64 busy_before[DEVICE_SMP_MAX_CPUS],
                                    u32 *online_mask)
{
    device_smp_reset();
    atomic_store_explicit(&device_smp_storage_mode, io_workers != 0U,
                          memory_order_relaxed);
    if (!device_smp_cpu_snapshot(cpus, online_mask, busy_before)) return false;
    if (!device_smp_start_thread(device_smp_monitor_worker, NULL)) return false;
    for (u32 index = 0U; index < cpus + 1U; index++) {
        device_smp_spin_arguments[index].index = index;
        if (!device_smp_start_thread(device_smp_spin_worker,
                                     &device_smp_spin_arguments[index])) {
            (void)device_smp_finish_threads(0U, true);
            return false;
        }
    }
    for (u32 index = 0U; index < io_workers; index++) {
        device_smp_io_arguments[index].index = index;
        if (!device_smp_start_thread(device_smp_storage_worker,
                                     &device_smp_io_arguments[index])) {
            (void)device_smp_finish_threads(0U, true);
            return false;
        }
    }
    u32 expected = 1U + cpus + 1U + io_workers;
    if (!device_smp_wait_ready(expected)) {
        (void)device_smp_finish_threads(0U, true);
        return false;
    }
    return true;
}

static bool device_smp_finish_common(u32 cpus, u64 busy_before[DEVICE_SMP_MAX_CPUS],
                                     u32 online_mask, u32 *busy_mask,
                                     u64 busy_delta[DEVICE_SMP_MAX_CPUS])
{
    u64 busy_after[DEVICE_SMP_MAX_CPUS] = {0};
    u32 after_mask = 0U;
    bool passed = device_smp_cpu_snapshot(cpus, &after_mask, busy_after) &&
                  after_mask == online_mask;
    *busy_mask = 0U;
    for (u32 cpu = 0U; cpu < cpus; cpu++) {
        busy_delta[cpu] = busy_after[cpu] >= busy_before[cpu]
            ? busy_after[cpu] - busy_before[cpu] : 0U;
        if (busy_delta[cpu]) *busy_mask |= 1U << cpu;
    }
    if (*busy_mask != ((1U << cpus) - 1U)) passed = false;
    return passed;
}

static bool device_smp_valid_run(const char *run)
{
    if (!run || strlen(run) != 16U) return false;
    for (u32 index = 0U; index < 16U; index++)
        if (!((run[index] >= '0' && run[index] <= '9') ||
              (run[index] >= 'a' && run[index] <= 'f')))
            return false;
    return true;
}

static int device_smp_storage(u32 cpus, const char *run)
{
    u64 busy_before[DEVICE_SMP_MAX_CPUS];
    u64 busy_delta[DEVICE_SMP_MAX_CPUS] = {0};
    u32 online_mask = 0U;
    u32 busy_mask = 0U;
    u32 io_workers = cpus;
    bool passed;
    u64 started_ms;

    for (u32 worker = 0U; worker < io_workers; worker++) {
        int length = snprintf(device_smp_paths[worker],
            sizeof(device_smp_paths[worker]),
            "/home/developer/smp-storage-%s-%u.bin", run, worker);
        if (length <= 0 || (usize)length >= sizeof(device_smp_paths[worker]))
            return 2;
        for (u32 offset = 0U; offset < DEVICE_SMP_FILE_BYTES; offset++)
            device_smp_payloads[worker][offset] =
                device_smp_payload_byte(worker, offset, run);
    }
    if (!device_smp_start_common(cpus, io_workers, busy_before, &online_mask))
        return 3;
    started_ms = uptime_ms();
    atomic_store_explicit(&device_smp_device_active, true,
                          memory_order_release);
    atomic_store_explicit(&device_smp_begin, true, memory_order_release);
    u64 deadline_start = uptime_ms();
    while (atomic_load_explicit(&device_smp_io_finished,
                                memory_order_acquire) != io_workers) {
        u64 now = uptime_ms();
        if (now == (u64)-1 || deadline_start == (u64)-1 || now < deadline_start ||
            now - deadline_start > DEVICE_SMP_WORK_TIMEOUT_MS) {
            (void)device_smp_finish_threads(cpus + 1U, false);
            return 4;
        }
        (void)process_yield();
    }
    atomic_store_explicit(&device_smp_device_active, false,
                          memory_order_release);
    passed = device_smp_finish_threads(cpus + 1U, false);
    passed = device_smp_finish_common(cpus, busy_before, online_mask,
                                      &busy_mask, busy_delta) && passed;
    u32 max_active = atomic_load_explicit(&device_smp_io_max_active,
                                          memory_order_acquire);
    u32 io_started = atomic_load_explicit(&device_smp_io_started,
                                          memory_order_acquire);
    u32 io_finished = atomic_load_explicit(&device_smp_io_finished,
                                           memory_order_acquire);
    u32 io_failures = 0U;
    u32 spin_started = atomic_load_explicit(&device_smp_spin_started,
                                            memory_order_acquire);
    u32 multi = atomic_load_explicit(&device_smp_multicore_samples,
                                     memory_order_relaxed);
    u32 overlap = atomic_load_explicit(&device_smp_overlap_samples,
                                      memory_order_relaxed);
    u64 work = atomic_load_explicit(&device_smp_work_units,
                                    memory_order_relaxed);
    if (io_started != io_workers || io_finished != io_workers ||
        max_active < 2U || spin_started != cpus + 1U || !multi ||
        !overlap || !work || busy_mask != ((1U << cpus) - 1U))
        passed = false;
    for (u32 worker = 0U; worker < io_workers; worker++) {
        if (device_smp_io_status[worker] != MG_OK) {
            io_failures++;
            passed = false;
        }
    }
    u64 ended_ms = uptime_ms();
    printf("DEVSMPS1 mode=storage pass=%u cpus=%u online_mask=%x "
           "io_workers=%u io_started=%u io_completed=%u max_active=%u "
           "multicore_samples=%u overlap_samples=%u spin_workers=%u "
           "spin_started=%u spin_units=%llu io_failures=%u "
           "busy_mask=%x busy_delta=",
           passed ? 1U : 0U, cpus, online_mask, io_workers, io_started,
           io_finished, max_active, multi, overlap, cpus + 1U, spin_started,
           (unsigned long long)work, io_failures, busy_mask);
    for (u32 cpu = 0U; cpu < cpus; cpu++)
        printf("%s%llu", cpu ? "," : "",
               (unsigned long long)busy_delta[cpu]);
    printf(" exact_guest_readback=%u elapsed_ms=%llu start_ms=%llu end_ms=%llu\n",
           passed ? 1U : 0U,
           (unsigned long long)(ended_ms >= started_ms ? ended_ms - started_ms : 0U),
           (unsigned long long)started_ms, (unsigned long long)ended_ms);
    return passed ? 0 : 43;
}

static bool device_smp_network_interface(mg_net_interface_info_t *output)
{
    mg_net_interface_info_t entries[4];
    mg_ipv4_addr_t expected = {{10U, 0U, 2U, 15U}};
    mg_result_t count = mg_net_interfaces(entries, sizeof(entries));
    if (!output || count <= 0 || count > 4) return false;
    for (mg_result_t index = 0; index < count; index++) {
        if (entries[index].enabled && entries[index].link_up &&
            !memcmp(&entries[index].address, &expected, sizeof(expected))) {
            *output = entries[index];
            return true;
        }
    }
    return false;
}

static int device_smp_network(u32 cpus)
{
    static const mg_ipv4_addr_t no_reply = {{203U, 0U, 113U, 1U}};
    static const mg_ipv4_addr_t gateway = {{10U, 0U, 2U, 2U}};
    static const u8 payload[] = "Mangrove SMP e1000 receive qualification";
    u64 busy_before[DEVICE_SMP_MAX_CPUS];
    u64 busy_delta[DEVICE_SMP_MAX_CPUS] = {0};
    u32 online_mask = 0U;
    u32 busy_mask = 0U;
    mg_net_interface_info_t before, after;
    mg_handle_t handle = 0U;
    mg_result_t timeout_status = MG_ERR_NETWORK_UNAVAILABLE;
    mg_result_t status = MG_ERR_NETWORK_UNAVAILABLE;
    mg_result_t close_status = MG_ERR_NETWORK_UNAVAILABLE;
    mg_icmp_echo_result_t reply = {0};
    u32 echoes = 0U;
    u16 reply_bytes = 0U;
    u64 started_ms;
    bool passed = false;
    char source[16] = {0};

    if (!device_smp_start_common(cpus, 0U, busy_before, &online_mask))
        return 10;
    if (!device_smp_network_interface(&before)) {
        (void)device_smp_finish_threads(cpus + 1U, true);
        return 10;
    }
    started_ms = uptime_ms();
    atomic_store_explicit(&device_smp_device_active, true,
                          memory_order_release);
    atomic_store_explicit(&device_smp_begin, true, memory_order_release);
    status = mg_icmp_open(&handle);
    if (status == MG_OK) {
        timeout_status = mg_icmp_echo(handle, &no_reply, payload,
            sizeof(payload) - 1U, 1000U, &reply);
        if (timeout_status == MG_ERR_TIMEOUT) {
            status = mg_icmp_echo(handle, &gateway, payload,
                sizeof(payload) - 1U, 1500U, &reply);
            if (status == MG_OK &&
                memcmp(&reply.source, &gateway, sizeof(gateway)) == 0 &&
                reply.reply_length == sizeof(payload) - 1U) {
                for (u32 index = 0U; index < DEVICE_SMP_ECHOES; index++) {
                    memset(&reply, 0, sizeof(reply));
                    status = mg_icmp_echo(handle, &gateway, payload,
                        sizeof(payload) - 1U, 1500U, &reply);
                    if (status != MG_OK ||
                        memcmp(&reply.source, &gateway, sizeof(gateway)) != 0 ||
                        reply.reply_length != sizeof(payload) - 1U)
                        break;
                    echoes++;
                    reply_bytes = reply.reply_length;
                }
            }
        }
        close_status = mg_icmp_close(handle);
    }
    atomic_store_explicit(&device_smp_device_active, false,
                          memory_order_release);
    bool threads_ok = device_smp_finish_threads(cpus + 1U, false);
    bool cpus_ok = device_smp_finish_common(cpus, busy_before, online_mask,
                                             &busy_mask, busy_delta);
    bool interface_ok = device_smp_network_interface(&after) &&
                        before.id == after.id;
    if (interface_ok)
        (void)mg_ipv4_format(&reply.source, source, sizeof(source));
    u32 spin_started = atomic_load_explicit(&device_smp_spin_started,
                                            memory_order_acquire);
    u32 multi = atomic_load_explicit(&device_smp_multicore_samples,
                                     memory_order_relaxed);
    u32 overlap = atomic_load_explicit(&device_smp_overlap_samples,
                                      memory_order_relaxed);
    u64 work = atomic_load_explicit(&device_smp_work_units,
                                    memory_order_relaxed);
    bool counters = interface_ok && after.rx_packets >= before.rx_packets &&
        after.tx_packets >= before.tx_packets &&
        after.rx_packets - before.rx_packets >= DEVICE_SMP_ECHOES &&
        after.tx_packets - before.tx_packets >= DEVICE_SMP_ECHOES;
    passed = timeout_status == MG_ERR_TIMEOUT && close_status == MG_OK &&
        status == MG_OK && echoes == DEVICE_SMP_ECHOES &&
        reply_bytes == sizeof(payload) - 1U && !strcmp(source, "10.0.2.2") &&
        threads_ok && cpus_ok && spin_started == cpus + 1U && multi &&
        overlap && work && counters && busy_mask == ((1U << cpus) - 1U);
    u64 ended_ms = uptime_ms();
    printf("DEVSMPS1 mode=network pass=%u cpus=%u online_mask=%x "
           "spin_workers=%u spin_started=%u spin_units=%llu "
           "timeout_status=%d status=%d close_status=%d replies=%u "
           "reply_bytes=%u source=%s "
           "rx_before=%llu rx_after=%llu tx_before=%llu tx_after=%llu "
           "multicore_samples=%u overlap_samples=%u busy_mask=%x busy_delta=",
           passed ? 1U : 0U, cpus, online_mask, cpus + 1U, spin_started,
           (unsigned long long)work, timeout_status, status, close_status,
           echoes, reply_bytes,
           source[0] ? source : "none",
           (unsigned long long)before.rx_packets,
           (unsigned long long)(interface_ok ? after.rx_packets : 0U),
           (unsigned long long)before.tx_packets,
           (unsigned long long)(interface_ok ? after.tx_packets : 0U),
           multi, overlap, busy_mask);
    for (u32 cpu = 0U; cpu < cpus; cpu++)
        printf("%s%llu", cpu ? "," : "",
               (unsigned long long)busy_delta[cpu]);
    printf(" elapsed_ms=%llu start_ms=%llu end_ms=%llu\n",
           (unsigned long long)(ended_ms >= started_ms ? ended_ms - started_ms : 0U),
           (unsigned long long)started_ms, (unsigned long long)ended_ms);
    return passed ? 0 : 43;
}

int fstest_device_smp_phase(const char *mode, u32 cpus, const char *run)
{
    if (!mode || cpus < 2U || cpus > DEVICE_SMP_MAX_CPUS ||
        !device_smp_valid_run(run))
        return 2;
    if (!strcmp(mode, "storage")) return device_smp_storage(cpus, run);
    if (!strcmp(mode, "network")) return device_smp_network(cpus);
    return 2;
}
