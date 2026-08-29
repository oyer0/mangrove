#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"

#include "ping_args.h"

#define PING_TIMEOUT_MS 1000U

static bool resolve_host(const char *host, mg_ipv4_addr_t *address)
{
    mg_result_t result;

    if (mg_ipv4_parse(host, address)) return true;
    result = mg_net_resolve_a(host, address, MG_NET_TIMEOUT_DEFAULT);
    if (result < 0) {
        printf("Could not resolve %s: %s\n", host, error_string(result));
        return false;
    }
    return true;
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
    u32 received = 0;
    bool literal;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (!ping_parse_arguments(argc, argv, &count, &host)) {
        command_usage_error(argv[0],
                            "ping <host> | ping [-c|--count] <count> <host>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    if (!resolve_host(host, &destination) ||
        !mg_ipv4_format(&destination, address_text, sizeof(address_text))) {
        return 1;
    }

    literal = mg_ipv4_parse(host, &destination);
    if (literal)
        printf("PING %s\n\n", address_text);
    else
        printf("PING %s (%s)\n\n", host, address_text);

    result = mg_icmp_open(&handle);
    if (result < 0) {
        printf("Could not open ICMP session: %s\n", error_string(result));
        return 1;
    }

    for (u32 sequence = 1; sequence <= count; sequence++) {
        result = mg_icmp_echo(handle, &destination, payload, sizeof(payload),
                              PING_TIMEOUT_MS, &reply);
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
        } else {
            printf("Request failed: seq=%u: %s\n", sequence, error_string(result));
        }
    }
    (void)mg_icmp_close(handle);
    printf("\n%u sent, %u received, %u%% loss\n", count, received,
           ((count - received) * 100U) / count);
    return received == count ? 0 : 1;
}
