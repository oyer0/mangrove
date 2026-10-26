/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

typedef struct {
    char source[16];
    u32 echoes;
    u16 reply_length;
    bool timeout_recovered;
    u64 rx_before;
    u64 rx_after;
    u64 tx_before;
    u64 tx_after;
} network_smoke_result_t;

static bool network_interface(mg_net_interface_info_t *output)
{
    mg_net_interface_info_t interfaces[4];
    mg_ipv4_addr_t expected = {{10, 0, 2, 15}};
    mg_result_t count = mg_net_interfaces(interfaces, sizeof(interfaces));

    if (!output || count <= 0 || count > 4) return false;
    for (mg_result_t index = 0; index < count; index++) {
        if (interfaces[index].enabled && interfaces[index].link_up &&
            !memcmp(&interfaces[index].address, &expected, sizeof(expected))) {
            *output = interfaces[index];
            return true;
        }
    }
    return false;
}

static bool network_receive_failure(const char *stage, u32 echo,
                                    mg_result_t status)
{
    printf("NET1 failure stage=%s echo=%u status=%d\n",
           stage, echo, status);
    (void)fflush(stdout);
    return false;
}

static bool network_timeout_recovery(network_smoke_result_t *output)
{
    static const u8 payload[] = "Mangrove ICMP timeout recovery";
    static const mg_ipv4_addr_t no_reply = {{203, 0, 113, 1}};
    static const mg_ipv4_addr_t gateway = {{10, 0, 2, 2}};
    mg_icmp_echo_result_t reply = {0};
    mg_handle_t handle = 0;
    mg_result_t status = mg_icmp_open(&handle);
    mg_result_t close_status;

    if (status != MG_OK)
        return network_receive_failure("timeout-open", 0U, status);
    status = mg_icmp_echo(handle, &no_reply, payload, sizeof(payload) - 1U,
                          1000U, &reply);
    if (status != MG_ERR_TIMEOUT) {
        (void)mg_icmp_close(handle);
        return network_receive_failure("timeout", 0U, status);
    }

    /* The TEST-NET destination has no echo responder in the QEMU user-mode
     * network. A following gateway echo on the same handle proves that the
     * timed-out request released the kernel's single outstanding-echo slot. */
    status = mg_icmp_echo(handle, &gateway, payload, sizeof(payload) - 1U,
                          1500U, &reply);
    close_status = mg_icmp_close(handle);
    if (close_status != MG_OK)
        return network_receive_failure("timeout-close", 0U, close_status);
    if (status != MG_OK ||
        memcmp(&reply.source, &gateway, sizeof(gateway)) != 0 ||
        reply.reply_length != sizeof(payload) - 1U)
        return network_receive_failure("timeout-recovery", 0U, status);
    output->timeout_recovered = true;
    return true;
}

static bool network_receive_smoke(network_smoke_result_t *output)
{
    static const u8 payload[] = "Mangrove e1000 receive qualification";
    static const mg_ipv4_addr_t gateway = {{10, 0, 2, 2}};
    enum { NETWORK_RX_ECHO_COUNT = 16U };
    mg_net_interface_info_t before, after;

    if (!output || !network_interface(&before))
        return network_receive_failure("interface-before", 0U,
                                       MG_ERR_NETWORK_UNAVAILABLE);
    if (!network_timeout_recovery(output)) return false;

    /* Open and close a kernel ICMP object for each request so this bounded
     * receive burst also exercises public kernel allocation ownership. The
     * first echo exercises ARP resolution inside the caller's timeout; it
     * must not need an application-level warmup retry. */
    for (u32 echo = 0; echo < NETWORK_RX_ECHO_COUNT; echo++) {
        mg_icmp_echo_result_t reply = {0};
        mg_handle_t handle = 0;
        mg_result_t status = MG_ERR_NETWORK_UNAVAILABLE;
        mg_result_t close_status;
        bool received = false;

        status = mg_icmp_open(&handle);
        if (status != MG_OK)
            return network_receive_failure("open", echo, status);
        status = mg_icmp_echo(handle, &gateway, payload,
                              sizeof(payload) - 1U, 1500U, &reply);
        received = status == MG_OK;
        close_status = mg_icmp_close(handle);
        if (close_status != MG_OK)
            return network_receive_failure("close", echo, close_status);
        if (!received)
            return network_receive_failure("echo", echo, status);
        if (!mg_ipv4_format(&reply.source, output->source,
                            sizeof(output->source)) ||
            memcmp(&reply.source, &gateway, sizeof(gateway)) != 0 ||
            reply.reply_length != sizeof(payload) - 1U)
            return network_receive_failure("reply", echo, MG_OK);
        output->echoes++;
        output->reply_length = reply.reply_length;
    }
    if (!network_interface(&after) || before.id != after.id)
        return network_receive_failure("interface-after", output->echoes,
                                       MG_ERR_NETWORK_UNAVAILABLE);
    if (output->echoes != NETWORK_RX_ECHO_COUNT ||
        after.rx_packets < before.rx_packets ||
        after.tx_packets < before.tx_packets ||
        after.rx_packets - before.rx_packets < NETWORK_RX_ECHO_COUNT ||
        after.tx_packets - before.tx_packets < NETWORK_RX_ECHO_COUNT)
        return network_receive_failure("counters", output->echoes,
                                       MG_ERR_NETWORK_UNAVAILABLE);
    output->rx_before = before.rx_packets;
    output->rx_after = after.rx_packets;
    output->tx_before = before.tx_packets;
    output->tx_after = after.tx_packets;
    return true;
}

static void network_receive_report(const network_smoke_result_t *network)
{
    printf("NET1 source=%s reply_bytes=%u echoes=%u timeout_recovered=%u "
           "rx_before=%llu rx_after=%llu "
           "tx_before=%llu tx_after=%llu\n",
           network->source, network->reply_length, network->echoes,
           network->timeout_recovered ? 1U : 0U,
           (unsigned long long)network->rx_before,
           (unsigned long long)network->rx_after,
           (unsigned long long)network->tx_before,
           (unsigned long long)network->tx_after);
}

int fstest_network_phase(void)
{
    network_smoke_result_t network = {0};

    puts("NET1 begin");
    (void)fflush(stdout);
    if (!network_receive_smoke(&network)) return 43;
    network_receive_report(&network);
    puts("NET1 complete");
    (void)fflush(stdout);
    return 0;
}

int fstest_time_phase(bool network_smoke)
{
    mg_mangrove_time_t native;
    mg_mangrove_time_t later_native;
    mg_realtime_info_t realtime_info;
    mg_unix_time_t unix_value;
    mg_monotonic_time_t mono_before, mono_after;
    struct timespec now;
    struct tm utc, local, normalized;
    time_t value, wall_start_unix, later_time;
    clock_t cpu_before, cpu_busy, cpu_sleep;
    char formatted[64], later_formatted[64];
    u64 sleep_elapsed, wall_elapsed_ms, wall_elapsed;
    network_smoke_result_t network = {0};

    if (mg_clock_realtime_info(&realtime_info) != MG_OK ||
        realtime_info.state != MG_REALTIME_TRUSTED ||
        realtime_info.resolution_ns != MG_TIME_NANOSECONDS_PER_SEC ||
        mg_clock_realtime(&native) != MG_OK ||
        native.seconds != realtime_info.time.seconds ||
        native.nanoseconds != realtime_info.time.nanoseconds ||
        mangrove_time_to_unix(&native, &unix_value) != MG_OK)
        return 43;
    value = time(NULL);
    if (value != unix_value.seconds ||
        timespec_get(&now, TIME_UTC) != TIME_UTC ||
        now.tv_sec != value || now.tv_nsec != native.nanoseconds ||
        !gmtime_r(&value, &utc) || !localtime_r(&value, &local) ||
        utc.tm_year != local.tm_year || utc.tm_mon != local.tm_mon ||
        utc.tm_mday != local.tm_mday || utc.tm_hour != local.tm_hour ||
        utc.tm_min != local.tm_min || utc.tm_sec != local.tm_sec ||
        utc.tm_isdst != 0)
        return 43;

    normalized = utc;
    if (mktime(&normalized) != value || normalized.tm_isdst != 0 ||
        strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %z %Z",
                 &utc) == 0)
        return 43;

    cpu_before = clock();
    if (cpu_before < 0 ||
        mg_clock_monotonic(&mono_before) != MG_OK)
        return 43;
    do {
        volatile u64 work = 0;
        for (u64 i = 0; i < 10000U; i++)
            work += i ^ (i >> 3);
        (void)work;
        if (mg_clock_monotonic(&mono_after) != MG_OK ||
            mono_after.milliseconds < mono_before.milliseconds)
            return 43;
    } while (mono_after.milliseconds - mono_before.milliseconds < 20U);
    cpu_busy = clock();
    if (cpu_busy <= cpu_before ||
        mg_clock_monotonic(&mono_before) != MG_OK)
        return 43;

    cpu_before = clock();
    if (cpu_before < 0 || process_sleep_ms(30U) != MG_OK ||
        mg_clock_monotonic(&mono_after) != MG_OK ||
        mono_after.milliseconds < mono_before.milliseconds)
        return 43;
    sleep_elapsed = mono_after.milliseconds - mono_before.milliseconds;
    cpu_sleep = clock();
    if (sleep_elapsed < 15U || cpu_sleep < cpu_before ||
        (u64)(cpu_sleep - cpu_before) >= sleep_elapsed)
        return 43;

    if (mg_clock_realtime(&native) != MG_OK ||
        mangrove_time_to_unix(&native, &unix_value) != MG_OK)
        return 43;
    wall_start_unix = unix_value.seconds;
    if (mg_clock_monotonic(&mono_before) != MG_OK ||
        process_sleep_ms(1200U) != MG_OK ||
        mg_clock_monotonic(&mono_after) != MG_OK ||
        mg_clock_realtime(&later_native) != MG_OK ||
        mangrove_time_to_unix(&later_native, &unix_value) != MG_OK)
        return 43;
    wall_elapsed_ms = mono_after.milliseconds - mono_before.milliseconds;
    if (wall_elapsed_ms < 1000U || later_native.seconds < native.seconds)
        return 43;
    wall_elapsed = (u64)(later_native.seconds - native.seconds);
    later_time = unix_value.seconds;
    if (wall_elapsed == 0U || wall_elapsed > wall_elapsed_ms / 1000U + 1U ||
        !gmtime_r(&later_time, &local) ||
        strftime(later_formatted, sizeof(later_formatted),
                 "%Y-%m-%d %H:%M:%S", &local) == 0)
        return 43;

    if (network_smoke) {
        puts("NET1 begin");
        (void)fflush(stdout);
        if (!network_receive_smoke(&network)) return 43;
    }

    printf("TIME1 unix=%lld nanoseconds=%ld date=%s busy_cpu=%lld "
           "sleep_cpu=%lld sleep_elapsed_ms=%llu wall_delta_s=%llu "
           "wall_elapsed_ms=%llu wall_start_unix=%lld later_unix=%lld "
           "later_date=%s\n",
           (long long)value, now.tv_nsec, formatted,
           (long long)cpu_busy, (long long)cpu_sleep,
           (unsigned long long)sleep_elapsed,
           (unsigned long long)wall_elapsed,
           (unsigned long long)wall_elapsed_ms,
           (long long)wall_start_unix, (long long)unix_value.seconds,
           later_formatted);
    if (network_smoke) network_receive_report(&network);
    puts("TIME1 complete");
    return 0;
}
