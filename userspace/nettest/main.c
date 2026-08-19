#include <stdio.h>
#include <mangrove.h>
#include <string.h>

int main(void)
{
    mg_ipv4_addr_t answer;
    mg_ipv4_addr_t gateway = {{10, 0, 2, 2}};
    mg_net_endpoint_t echo = { .address = {{10, 0, 2, 2}}, .port = 12345 };
    mg_net_endpoint_t udp_echo = { .address = {{10, 0, 2, 2}}, .port = 12346 };
    mg_icmp_echo_result_t reply;
    mg_datagram_result_t datagram;
    mg_net_info_t info;
    mg_handle_t handle;
    char received[32];
    char udp_received[32];
    static const char hello[] = "hello from userspace";
    mg_result_t result;

    result = mg_net_info(&info);
    if (result < 0 || !info.configured) { printf("nettest: network unavailable\n"); return 1; }
    result = mg_net_resolve_a("example.com", &answer, 5000);
    if (result < 0) { printf("nettest: DNS failed: %s\n", error_string(result)); return 1; }
    result = mg_icmp_open(&handle);
    if (result < 0) { printf("nettest: ICMP open failed\n"); return 1; }
    result = mg_icmp_echo(handle, &gateway, "mg", 2, 1000, &reply);
    (void)mg_icmp_close(handle);
    if (result < 0) { printf("nettest: ICMP failed: %s\n", error_string(result)); return 1; }
    result = mg_stream_connect(&echo, 5000, &handle);
    if (result < 0) { printf("nettest: TCP connect failed: %s\n", error_string(result)); return 1; }
    if (mg_stream_send(handle, hello, sizeof(hello) - 1, 5000) < 0 ||
        mg_stream_receive(handle, received, sizeof(hello) - 1, 5000) != sizeof(hello) - 1) {
        (void)mg_stream_close(handle); printf("nettest: TCP exchange failed\n"); return 1;
    }
    (void)mg_stream_close(handle);
    result = mg_datagram_open(0, &handle);
    if (result < 0 || mg_stream_send(handle, hello, sizeof(hello) - 1, 1000) != MG_ERR_INVALID_HANDLE ||
        mg_datagram_send(handle, &udp_echo, hello, sizeof(hello) - 1, 1000) < 0 ||
        mg_datagram_receive(handle, udp_received, sizeof(udp_received), 5000, &datagram) !=
            sizeof(hello) - 1 || memcmp(udp_received, hello, sizeof(hello) - 1) != 0) {
        (void)mg_datagram_close(handle); printf("nettest: UDP exchange failed\n"); return 1;
    }
    (void)mg_datagram_close(handle);
    if (mg_datagram_send(handle, &udp_echo, hello, sizeof(hello) - 1, 0) != MG_ERR_INVALID_HANDLE) {
        printf("nettest: stale handle accepted\n"); return 1;
    }
    printf("nettest: DNS/ICMP/UDP/TCP passed (%u ms)\n", (u32)reply.rtt_ms);
    return 0;
}
