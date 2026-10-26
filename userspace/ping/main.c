/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"

#include "ping_args.h"

#define PING_TIMEOUT_MS 1000U

static bool resolve_host(const char *host, mg_ipv4_addr_t *address, bool *literal)
{
    mg_result_t result;

    /* Keep this classification: a failed parse may modify its output, so it
     * must not probe the resolved address again after DNS has filled it. */
    *literal = mg_ipv4_parse(host, address);
    if (*literal) return true;
    result = mg_net_resolve_a(host, address, MG_NET_TIMEOUT_DEFAULT);
    if (result < 0) {
        printf("Could not resolve %s: %s\n", host, error_string(result));
        return false;
    }
    return true;
}

static mg_result_t wait_until(u64 deadline_ms)
{
    for (;;) {
        mg_monotonic_time_t now;
        mg_result_t result = mg_clock_monotonic(&now);

        if (result != MG_OK) return result;
        if (now.milliseconds >= deadline_ms) return MG_OK;
        result = process_sleep_ms(deadline_ms - now.milliseconds);
        if (result != MG_OK) return result;
    }
}

static u64 request_deadline(u64 start_ms, u64 interval_ms, u32 sequence)
{
    u64 index = sequence - 1U;
    u64 max = ~(u64)0;

    if (index && interval_ms > (max - start_ms) / index) return max;
    return start_ms + interval_ms * index;
}

static void print_statistics(u32 sent, u32 received)
{
    u32 loss = sent ? ((sent - received) * 100U) / sent : 0U;

    printf("\n%u sent, %u received, %u%% loss\n", sent, received, loss);
}

int main(int argc, char **argv)
{
    static const u8 payload[56] = "Mangrove ICMP Echo";
    const char *host;
    mg_ipv4_addr_t destination;
    mg_icmp_echo_result_t reply;
    mg_handle_t handle;
    mg_result_t result;
    char address_text[16];
    u32 count;
    u32 request_timeout_ms;
    u32 sent = 0;
    u32 received = 0;
    u64 interval_ms;
    mg_monotonic_time_t schedule_start;
    bool literal;
    bool interrupted = false;
    bool failed = false;
    const char *bad_argument;
    cli_options_error_t option_error;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    ping_parse_result_t parse_result = ping_parse_arguments(argc, argv,
                                                            &count,
                                                            &interval_ms, &host,
                                                            &bad_argument,
                                                            &option_error);
    if (parse_result != PING_PARSE_OK) {
        if (parse_result == PING_PARSE_INVALID_COUNT) {
            printf("Invalid count: %s.\n",
                   bad_argument ? bad_argument : "(missing)");
            return 1;
        }
        if (parse_result == PING_PARSE_INVALID_INTERVAL) {
            printf("Invalid interval: %s (minimum is 0.002 seconds).\n",
                   bad_argument ? bad_argument : "(missing)");
            return 1;
        }
        if (option_error == CLI_OPTIONS_ERROR_UNKNOWN_OPTION)
            command_usage_error(argv[0],
                                "ping [-c|--count COUNT] [-i|--interval SECONDS] HOST",
                                bad_argument);
        else {
            if (option_error != CLI_OPTIONS_ERROR_NONE)
                printf("%s%s%s.\n", cli_options_error_message(option_error),
                       bad_argument ? ": " : "",
                       bad_argument ? bad_argument : "");
            command_usage_error(argv[0],
                                "ping [-c|--count COUNT] [-i|--interval SECONDS] HOST",
                                NULL);
        }
        return 1;
    }
    if (!resolve_host(host, &destination, &literal) ||
        !mg_ipv4_format(&destination, address_text, sizeof(address_text))) {
        return 1;
    }

    if (literal)
        printf("PING %s\n\n", address_text);
    else
        printf("PING %s (%s)\n\n", host, address_text);

    result = mg_icmp_open(&handle);
    if (result < 0) {
        printf("Could not open ICMP session: %s\n", error_string(result));
        return 1;
    }

    result = mg_clock_monotonic(&schedule_start);
    if (result != MG_OK) {
        (void)mg_icmp_close(handle);
        printf("Could not read monotonic clock: %s.\n", error_string(result));
        return 1;
    }
    request_timeout_ms = interval_ms < PING_TIMEOUT_MS ?
        (u32)interval_ms : PING_TIMEOUT_MS;

    for (u32 sequence = 1; sequence <= count; sequence++) {
        if (sequence > 1U) {
            result = wait_until(request_deadline(schedule_start.milliseconds,
                                                 interval_ms, sequence));
            if (result == MG_ERR_INTERRUPTED) {
                interrupted = true;
                break;
            }
            if (result != MG_OK) {
                printf("Could not wait for next request: %s.\n",
                       error_string(result));
                failed = true;
                break;
            }
        }
        reply = (mg_icmp_echo_result_t){0};
        result = mg_icmp_echo(handle, &destination, payload, sizeof(payload),
                              request_timeout_ms, &reply);
        if (reply.sequence != 0) sent++;
        if (result == MG_OK) {
            char source_text[16];
            if (!mg_ipv4_format(&reply.source, source_text, sizeof(source_text))) {
                (void)mg_icmp_close(handle);
                return 1;
            }
            received++;
            printf("%u bytes from %s: seq=%u time=%llu ms\n",
                   (u32)reply.reply_length + 8U, source_text, sequence,
                   reply.rtt_ms);
        } else if (result == MG_ERR_TIMEOUT) {
            printf("Request timeout: seq=%u\n", sequence);
        } else if (result == MG_ERR_INTERRUPTED) {
            interrupted = true;
            break;
        } else {
            printf("Request failed: seq=%u: %s\n", sequence, error_string(result));
            failed = true;
        }
    }
    (void)mg_icmp_close(handle);
    print_statistics(sent, received);
    if (interrupted) return 130;
    return !failed && received == sent ? 0 : 1;
}
