#include <mangrove.h>

extern long mg_syscall(unsigned long number, unsigned long arg0,
                       unsigned long arg1, unsigned long arg2);

#define MG_SYSCALL_NETWORK 23U

static mg_result_t net_call(mg_net_request_t *request)
{
    return (mg_result_t)mg_syscall(MG_SYSCALL_NETWORK,
                                   (unsigned long)request, 0, 0);
}

bool mg_ipv4_parse(const char *text, mg_ipv4_addr_t *address)
{
    usize index = 0;
    if (!text || !address) return false;
    for (u32 octet = 0; octet < 4; octet++) {
        u32 value = 0;
        u32 digits = 0;
        while (text[index] >= '0' && text[index] <= '9') {
            value = value * 10U + (u32)(text[index++] - '0');
            if (++digits > 3 || value > 255U) return false;
        }
        if (!digits) return false;
        address->octet[octet] = (u8)value;
        if (octet != 3) {
            if (text[index++] != '.') return false;
        } else if (text[index]) return false;
    }
    return true;
}

bool mg_ipv4_format(const mg_ipv4_addr_t *address, char *text, usize capacity)
{
    usize length = 0;
    if (!address || !text || capacity < 8) return false;
    for (u32 octet = 0; octet < 4; octet++) {
        char digits[3];
        u32 value = address->octet[octet];
        usize count = 0;
        do { digits[count++] = (char)('0' + value % 10U); value /= 10U; } while (value);
        if (length + count + (octet != 3) >= capacity) return false;
        while (count) text[length++] = digits[--count];
        if (octet != 3) text[length++] = '.';
    }
    text[length] = '\0';
    return true;
}

mg_result_t mg_net_info(mg_net_info_t *info)
{
    mg_net_request_t request = { .operation = MG_NET_OP_INFO, .result = info,
                                 .result_capacity = sizeof(*info) };
    return net_call(&request);
}

static mg_result_t net_snapshot(u32 operation, void *entries, usize capacity)
{
    mg_net_request_t request = { .operation = operation, .result = entries,
                                 .result_capacity = capacity };
    return net_call(&request);
}

mg_result_t mg_net_interfaces(mg_net_interface_info_t *entries, usize capacity)
{ return net_snapshot(MG_NET_OP_INTERFACES, entries, capacity); }
mg_result_t mg_net_routes(mg_net_route_info_t *entries, usize capacity)
{ return net_snapshot(MG_NET_OP_ROUTES, entries, capacity); }
mg_result_t mg_net_neighbors(mg_net_neighbor_info_t *entries, usize capacity)
{ return net_snapshot(MG_NET_OP_NEIGHBORS, entries, capacity); }
mg_result_t mg_net_connections(mg_net_connection_info_t *entries, usize capacity)
{ return net_snapshot(MG_NET_OP_CONNECTIONS, entries, capacity); }
mg_result_t mg_net_renew(u32 timeout_ms)
{
    mg_net_request_t request = { .operation = MG_NET_OP_RENEW,
                                 .timeout_ms = timeout_ms };
    return net_call(&request);
}

mg_result_t mg_net_resolve_a(const char *hostname, mg_ipv4_addr_t *address, u32 timeout_ms)
{
    mg_net_request_t request = { .operation = MG_NET_OP_RESOLVE_A,
        .timeout_ms = timeout_ms, .buffer = hostname,
        .result = address, .result_capacity = sizeof(*address) };
    return net_call(&request);
}

mg_result_t mg_icmp_open(mg_handle_t *handle)
{
    mg_net_request_t request = { .operation = MG_NET_OP_ICMP_OPEN };
    mg_result_t result;
    if (!handle) return MG_ERR_BAD_ARGUMENT;
    result = net_call(&request);
    if (result >= 0 && handle) *handle = (mg_handle_t)result;
    return result < 0 ? result : MG_OK;
}

mg_result_t mg_icmp_echo(mg_handle_t handle, const mg_ipv4_addr_t *destination,
                         const void *payload, usize payload_length, u32 timeout_ms,
                         mg_icmp_echo_result_t *result)
{
    if (!destination || !result || (!payload && payload_length)) return MG_ERR_BAD_ARGUMENT;
    mg_net_request_t request = { .operation = MG_NET_OP_ICMP_ECHO, .handle = handle,
        .timeout_ms = timeout_ms, .endpoint = { .address = *destination },
        .buffer = payload, .buffer_length = payload_length,
        .result = result, .result_capacity = sizeof(*result) };
    return net_call(&request);
}

mg_result_t mg_icmp_close(mg_handle_t handle)
{
    mg_net_request_t request = { .operation = MG_NET_OP_ICMP_CLOSE, .handle = handle };
    return net_call(&request);
}

mg_result_t mg_datagram_open(u16 local_port, mg_handle_t *handle)
{
    mg_net_request_t request = { .operation = MG_NET_OP_DATAGRAM_OPEN,
        .endpoint = { .port = local_port } };
    mg_result_t result;
    if (!handle) return MG_ERR_BAD_ARGUMENT;
    result = net_call(&request);
    if (result >= 0 && handle) *handle = (mg_handle_t)result;
    return result < 0 ? result : MG_OK;
}

mg_result_t mg_datagram_send(mg_handle_t handle, const mg_net_endpoint_t *destination,
                             const void *data, usize length, u32 timeout_ms)
{
    if (!destination || (!data && length)) return MG_ERR_BAD_ARGUMENT;
    mg_net_request_t request = { .operation = MG_NET_OP_DATAGRAM_SEND, .handle = handle,
        .timeout_ms = timeout_ms, .endpoint = *destination, .buffer = data,
        .buffer_length = length };
    return net_call(&request);
}

mg_result_t mg_datagram_receive(mg_handle_t handle, void *data, usize capacity,
                                u32 timeout_ms, mg_datagram_result_t *result)
{
    mg_net_request_t request = { .operation = MG_NET_OP_DATAGRAM_RECEIVE, .handle = handle,
        .timeout_ms = timeout_ms, .buffer = data, .buffer_length = capacity,
        .result = result, .result_capacity = sizeof(*result) };
    if (!data || !result) return MG_ERR_BAD_ARGUMENT;
    return net_call(&request);
}

mg_result_t mg_datagram_close(mg_handle_t handle)
{
    mg_net_request_t request = { .operation = MG_NET_OP_DATAGRAM_CLOSE, .handle = handle };
    return net_call(&request);
}

mg_result_t mg_stream_connect(const mg_net_endpoint_t *remote, u32 timeout_ms,
                              mg_handle_t *handle)
{
    mg_result_t result;
    mg_net_request_t request;
    if (!remote || !remote->port || !handle) return MG_ERR_BAD_ARGUMENT;
    request = (mg_net_request_t){ .operation = MG_NET_OP_STREAM_CONNECT,
                                  .timeout_ms = timeout_ms, .endpoint = *remote };
    result = net_call(&request);
    if (result >= 0 && handle) *handle = (mg_handle_t)result;
    return result < 0 ? result : MG_OK;
}

mg_result_t mg_stream_send(mg_handle_t handle, const void *data, usize length, u32 timeout_ms)
{
    mg_net_request_t request = { .operation = MG_NET_OP_STREAM_SEND, .handle = handle,
        .timeout_ms = timeout_ms, .buffer = data, .buffer_length = length };
    if (!data && length) return MG_ERR_BAD_ARGUMENT;
    return net_call(&request);
}

mg_result_t mg_stream_receive(mg_handle_t handle, void *data, usize capacity, u32 timeout_ms)
{
    mg_net_request_t request = { .operation = MG_NET_OP_STREAM_RECEIVE, .handle = handle,
        .timeout_ms = timeout_ms, .buffer = data, .buffer_length = capacity };
    if (!data || !capacity) return MG_ERR_BAD_ARGUMENT;
    return net_call(&request);
}

mg_result_t mg_stream_close(mg_handle_t handle)
{
    mg_net_request_t request = { .operation = MG_NET_OP_STREAM_CLOSE, .handle = handle };
    return net_call(&request);
}
