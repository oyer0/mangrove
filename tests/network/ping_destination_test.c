/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define main ping_main
#include "../../userspace/ping/main.c"
#undef main

static const char *expected_host;
static mg_ipv4_addr_t expected_address;
static mg_result_t resolve_status;
static unsigned resolve_calls, open_calls, echo_calls, close_calls;
static unsigned timed_out_echoes;

bool command_help_requested(int argc, char **argv)
{ (void)argc; (void)argv; return false; }
int command_print_help(const char *name)
{ (void)name; assert(0); return 1; }
void command_usage_error(const char *name, const char *usage, const char *option)
{ (void)name; (void)usage; (void)option; assert(0); }
const char *error_string(mg_result_t result)
{ (void)result; return "test error"; }

long mg_syscall(unsigned long number, unsigned long arg0,
                unsigned long arg1, unsigned long arg2)
{
    mg_net_request_t *request = (mg_net_request_t *)arg0;
    assert(number == 23U && !arg1 && !arg2);
    switch (request->operation) {
    case MG_NET_OP_RESOLVE_A:
        resolve_calls++;
        assert(!strcmp(request->buffer, expected_host));
        assert(request->result_capacity == sizeof(expected_address));
        if (resolve_status == MG_OK)
            memcpy(request->result, &expected_address, sizeof(expected_address));
        return resolve_status;
    case MG_NET_OP_ICMP_OPEN:
        open_calls++;
        return 42;
    case MG_NET_OP_ICMP_ECHO: {
        mg_icmp_echo_result_t *reply = request->result;
        assert(request->handle == 42);
        assert(request->buffer_length == 56U);
        assert(request->result_capacity == sizeof(*reply));
        if (memcmp(&request->endpoint.address, &expected_address,
                   sizeof(expected_address))) {
            const u8 *actual = request->endpoint.address.octet;
            fprintf(stderr, "%s: transmitted destination %u.%u.%u.%u\n",
                    expected_host, actual[0], actual[1], actual[2], actual[3]);
            assert(0 && "ping changed the resolved destination before syscall");
        }
        echo_calls++;
        if (echo_calls <= timed_out_echoes) return MG_ERR_TIMEOUT;
        *reply = (mg_icmp_echo_result_t){ .source = expected_address,
            .sequence = (u16)echo_calls, .reply_length = 56, .rtt_ms = 1 };
        return MG_OK;
    }
    case MG_NET_OP_ICMP_CLOSE:
        assert(request->handle == 42);
        close_calls++;
        return MG_OK;
    default:
        assert(0);
        return MG_ERR_BAD_ARGUMENT;
    }
}

static void run_case(const char *host, mg_ipv4_addr_t address, bool numeric,
                      mg_result_t dns_status, unsigned timeouts)
{
    char *argv[] = {"ping", (char *)host, NULL};
    expected_host = host;
    expected_address = address;
    resolve_status = dns_status;
    resolve_calls = open_calls = echo_calls = close_calls = 0;
    timed_out_echoes = timeouts;
    int status = ping_main(2, argv);
    assert(resolve_calls == (numeric ? 0U : 1U));
    if (dns_status != MG_OK) {
        assert(status == 1 && !open_calls && !echo_calls && !close_calls);
    } else {
        assert(status == (timeouts ? 1 : 0));
        assert(open_calls == 1 && echo_calls == 4 && close_calls == 1);
    }
}

int main(void)
{
    const mg_ipv4_addr_t first = {{104, 21, 6, 36}};
    const mg_ipv4_addr_t second = {{172, 67, 154, 203}};
    run_case("104.21.6.36", first, true, MG_OK, 0);
    run_case("172.67.154.203", second, true, MG_OK, 0);
    run_case("gnu.org", first, false, MG_OK, 0);
    run_case("5x40.org", first, false, MG_OK, 0);
    run_case("5x40.org", second, false, MG_OK, 0);
    run_case("12.34.example", second, false, MG_OK, 0);
    run_case("5x40.org", second, false, MG_OK, 1);
    run_case("5x40.org", first, false, MG_OK, 0);
    run_case("5missing.invalid", first, false, MG_ERR_NOT_FOUND, 0);
    run_case("missing.invalid", first, false, MG_ERR_TIMEOUT, 0);
    puts("Ping preserves resolved destination bytes through production syscall wrappers");
    return 0;
}
