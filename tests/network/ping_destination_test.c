/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char ping_output[32768];
static size_t ping_output_length;
static int ping_fixture_printf(const char *format, ...);

#define printf ping_fixture_printf
#define main ping_main
#include "../../userspace/ping/main.c"
#undef main
#undef printf

static int ping_fixture_printf(const char *format, ...)
{
    int result;
    va_list arguments;

    if (ping_output_length >= sizeof(ping_output)) return 0;
    va_start(arguments, format);
    result = vsnprintf(ping_output + ping_output_length,
                       sizeof(ping_output) - ping_output_length,
                       format, arguments);
    va_end(arguments);
    if (result > 0) {
        size_t remaining = sizeof(ping_output) - ping_output_length;
        ping_output_length += (size_t)result < remaining ?
            (size_t)result : remaining - 1U;
    }
    return result;
}

static void reset_ping_output(void)
{
    ping_output_length = 0;
    ping_output[0] = '\0';
}

static const char *expected_host;
static mg_ipv4_addr_t expected_address;
static mg_result_t resolve_status;
static unsigned resolve_calls, open_calls, echo_calls, close_calls;
static unsigned timed_out_echoes;
static u64 fake_time_ms;
static u64 echo_latency_ms;
static u64 sent_at_ms[2048];
static u64 slept_ms[2048];
static unsigned sleep_calls;
static bool interrupt_echo;
static bool interrupt_sleep;
static bool interrupt_before_transmit;
static unsigned usage_error_calls;

bool command_help_requested(int argc, char **argv)
{ (void)argc; (void)argv; return false; }
int command_print_help(const char *name)
{ (void)name; assert(0); return 1; }
void command_usage_error(const char *name, const char *usage, const char *option)
{ (void)name; (void)usage; (void)option; usage_error_calls++; }
const char *error_string(mg_result_t result)
{
    if (result == MG_ERR_INTERRUPTED) return "interrupted";
    (void)result; return "test error";
}

mg_result_t mg_clock_monotonic(mg_monotonic_time_t *out)
{
    assert(out);
    out->milliseconds = fake_time_ms;
    return MG_OK;
}

mg_result_t process_sleep_ms(u64 milliseconds)
{
    assert(milliseconds && sleep_calls < sizeof(slept_ms) / sizeof(slept_ms[0]));
    slept_ms[sleep_calls++] = milliseconds;
    if (interrupt_sleep) return MG_ERR_INTERRUPTED;
    fake_time_ms += milliseconds;
    return MG_OK;
}

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
        assert(echo_calls < sizeof(sent_at_ms) / sizeof(sent_at_ms[0]));
        sent_at_ms[echo_calls] = fake_time_ms;
        echo_calls++;
        if (interrupt_before_transmit) {
            *reply = (mg_icmp_echo_result_t){0};
            return MG_ERR_INTERRUPTED;
        }
        reply->sequence = (u16)echo_calls;
        if (interrupt_echo) return MG_ERR_INTERRUPTED;
        if (echo_calls <= timed_out_echoes) {
            fake_time_ms += request->timeout_ms;
            return MG_ERR_TIMEOUT;
        }
        *reply = (mg_icmp_echo_result_t){ .source = expected_address,
            .sequence = (u16)echo_calls, .reply_length = 56, .rtt_ms = 1 };
        fake_time_ms += echo_latency_ms;
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
    fake_time_ms = 0U;
    echo_latency_ms = 0U;
    sleep_calls = 0U;
    interrupt_echo = false;
    interrupt_sleep = false;
    interrupt_before_transmit = false;
    int status = ping_main(2, argv);
    assert(resolve_calls == (numeric ? 0U : 1U));
    if (dns_status != MG_OK) {
        assert(status == 1 && !open_calls && !echo_calls && !close_calls);
    } else {
        assert(status == (timeouts ? 1 : 0));
        assert(open_calls == 1 && echo_calls == 4 && close_calls == 1);
    }
}

static void test_count_option_forms(void)
{
    const mg_ipv4_addr_t address = {{10, 0, 2, 2}};
    char *separate[] = { "ping", "-c", "20", "10.0.2.2", NULL };
    char *attached[] = { "ping", "-c20", "10.0.2.2", NULL };
    char *long_separate[] = { "ping", "--count", "4", "10.0.2.2", NULL };
    char *long_equals[] = { "ping", "--count=4", "10.0.2.2", NULL };
    char *bad_group[] = { "ping", "-ci", "20", "0.2", "10.0.2.2", NULL };
    char *invalid[] = { "ping", "-c", "0", "10.0.2.2", NULL };
    char **cases[] = { separate, attached, long_separate, long_equals };
    int counts[] = { 4, 3, 4, 3 };
    unsigned expected_calls[] = { 20, 20, 4, 4 };

    for (unsigned index = 0; index < 4U; index++) {
        expected_host = "10.0.2.2";
        expected_address = address;
        resolve_status = MG_OK;
        resolve_calls = open_calls = echo_calls = close_calls = 0;
        timed_out_echoes = 0;
        assert(ping_main(counts[index], cases[index]) == 0);
        assert(!resolve_calls && open_calls == 1U &&
               echo_calls == expected_calls[index] && close_calls == 1U);
    }
    expected_host = "10.0.2.2";
    expected_address = address;
    resolve_status = MG_OK;
    resolve_calls = open_calls = echo_calls = close_calls = 0;
    assert(ping_main(5, bad_group) == 1);
    assert(!resolve_calls && !open_calls && !echo_calls);
    assert(ping_main(4, invalid) == 1);
    assert(!open_calls && !echo_calls);
}

static void test_interval_option_forms(void)
{
    const mg_ipv4_addr_t address = {{10, 0, 2, 2}};
    char *separate[] = {"ping", "-c", "3", "-i", "0.2", "10.0.2.2", NULL};
    char *attached[] = {"ping", "-c3", "-i0.2", "10.0.2.2", NULL};
    char *long_equals[] = {"ping", "--count=3", "--interval=0.2", "10.0.2.2", NULL};
    char *long_separate[] = {"ping", "--count", "3", "--interval", "0.2", "10.0.2.2", NULL};
    char *invalid[][6] = {
        {"ping", "-i", "0", "10.0.2.2", NULL},
        {"ping", "-i", "0.001", "10.0.2.2", NULL},
        {"ping", "-i", "0.001999", "10.0.2.2", NULL},
        {"ping", "-i", "-0.2", "10.0.2.2", NULL},
        {"ping", "--interval=fast", "10.0.2.2", NULL, NULL},
        {"ping", "-i", NULL, NULL, NULL, NULL},
        {"ping", "--interval", NULL, NULL, NULL, NULL},
        {"ping", "--interval=", "10.0.2.2", NULL, NULL, NULL},
    };
    int argc[] = {6, 4, 4, 6};
    char **forms[] = {separate, attached, long_equals, long_separate};
    unsigned requests[] = {3U, 3U, 3U, 3U};

    for (unsigned index = 0; index < 4U; index++) {
        expected_host = "10.0.2.2";
        expected_address = address;
        resolve_status = MG_OK;
        resolve_calls = open_calls = echo_calls = close_calls = 0U;
        timed_out_echoes = 0U;
        fake_time_ms = 0U;
        echo_latency_ms = 75U;
        sleep_calls = 0U;
        assert(ping_main(argc[index], forms[index]) == 0);
        assert(echo_calls == requests[index] && sent_at_ms[0] == 0U &&
               sent_at_ms[1] == 200U && sent_at_ms[2] == 400U);
        assert(sleep_calls == 2U && slept_ms[0] == 125U &&
               slept_ms[1] == 125U);
    }

    expected_host = "10.0.2.2";
    expected_address = address;
    resolve_status = MG_OK;
    echo_calls = open_calls = close_calls = 0U;
    timed_out_echoes = 0U;
    fake_time_ms = 0U;
    echo_latency_ms = 350U;
    sleep_calls = 0U;
    char *drift[] = {"ping", "-c4", "10.0.2.2", NULL};
    assert(ping_main(3, drift) == 0);
    assert(sent_at_ms[0] == 0U && sent_at_ms[1] == 1000U &&
           sent_at_ms[2] == 2000U && sent_at_ms[3] == 3000U);
    assert(sleep_calls == 3U && slept_ms[0] == 650U);

    char *minimum_args[] = {"ping", "-i", "0.002", "10.0.2.2", NULL};
    echo_calls = open_calls = close_calls = 0U;
    fake_time_ms = 0U;
    echo_latency_ms = 0U;
    sleep_calls = 0U;
    assert(ping_main(4, minimum_args) == 0);
    assert(sent_at_ms[1] == 2U && sent_at_ms[2] == 4U && sent_at_ms[3] == 6U);

    char *round_args[] = {"ping", "-c2", "-i0.0020001", "10.0.2.2", NULL};
    echo_calls = open_calls = close_calls = 0U;
    fake_time_ms = 0U;
    sleep_calls = 0U;
    assert(ping_main(4, round_args) == 0 && sent_at_ms[1] == 3U);

    /* The synchronous ICMP API has one outstanding request slot. When a
       reply is lost, bound its wait by a shorter requested interval so later
       requests still respect the configured cadence. */
    char *short_timeout[] = {"ping", "-c3", "-i0.2", "10.0.2.2", NULL};
    echo_calls = open_calls = close_calls = 0U;
    timed_out_echoes = 3U;
    fake_time_ms = 0U;
    echo_latency_ms = 0U;
    sleep_calls = 0U;
    assert(ping_main(4, short_timeout) == 1);
    assert(sent_at_ms[0] == 0U && sent_at_ms[1] == 200U &&
           sent_at_ms[2] == 400U);

    for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        int count = 0;
        while (count < 6 && invalid[index][count]) count++;
        echo_calls = open_calls = close_calls = 0U;
        assert(ping_main(count, invalid[index]) == 1);
        assert(!open_calls && !echo_calls);
    }
}

static void test_interruptible_waits(void)
{
    const mg_ipv4_addr_t address = {{10, 0, 2, 2}};
    char *args[] = {"ping", "-c3", "-i0.2", "10.0.2.2", NULL};
    expected_host = "10.0.2.2";
    expected_address = address;
    resolve_status = MG_OK;
    resolve_calls = open_calls = echo_calls = close_calls = 0U;
    timed_out_echoes = 0U;
    fake_time_ms = 0U;
    echo_latency_ms = 0U;
    sleep_calls = 0U;
    interrupt_sleep = true;
    reset_ping_output();
    assert(ping_main(4, args) == 130);
    assert(echo_calls == 1U && close_calls == 1U);
    assert(strstr(ping_output, "1 sent, 1 received, 0% loss") != NULL);

    echo_calls = open_calls = close_calls = 0U;
    sleep_calls = 0U;
    interrupt_sleep = false;
    interrupt_echo = true;
    reset_ping_output();
    assert(ping_main(4, args) == 130);
    assert(echo_calls == 1U && close_calls == 1U);
    assert(strstr(ping_output, "1 sent, 0 received, 100% loss") != NULL);

    /* An interrupt delivered before packet submission must not claim a
       transmission in the final statistics. */
    echo_calls = open_calls = close_calls = 0U;
    interrupt_echo = false;
    interrupt_before_transmit = true;
    reset_ping_output();
    assert(ping_main(4, args) == 130);
    assert(echo_calls == 1U && close_calls == 1U);
    assert(strstr(ping_output, "0 sent, 0 received, 0% loss") != NULL);
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
    test_count_option_forms();
    test_interval_option_forms();
    test_interruptible_waits();
    puts("Ping preserves resolved destination bytes through production syscall wrappers");
    return 0;
}
