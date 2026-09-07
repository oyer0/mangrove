#include <mangrove.h>
#include <mg/network_service.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/table.h"

static const mg_table_column_t NETWORK_INTERFACE_NAME_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_INTERFACE_TYPE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_INTERFACE_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_INTERFACE_IPV4_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_INTERFACE_COUNTER_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t NETWORK_ROUTE_DESTINATION_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_ROUTE_GATEWAY_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_ROUTE_INTERFACE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_NEIGHBOR_ADDRESS_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_NEIGHBOR_HARDWARE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_NEIGHBOR_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_CONNECTION_PROTOCOL_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_CONNECTION_ENDPOINT_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_CONNECTION_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t NETWORK_CONNECTION_PROCESS_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};

static mg_table_row_t network_rows[MG_TABLE_MAX_ROWS];

static void ip(const mg_ipv4_addr_t *address, char out[16])
{
    if (!mg_ipv4_format(address, out, 16)) strcpy(out, "-");
}

static void mac(const u8 value[6])
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x", value[0], value[1], value[2],
           value[3], value[4], value[5]);
}

static void format_mac(const u8 value[6], char out[18])
{
    (void)snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", value[0],
                   value[1], value[2], value[3], value[4], value[5]);
}

static const char *mode_name(u8 mode)
{
    if (mode == MG_NET_MODE_DHCP) return "automatic";
    if (mode == MG_NET_MODE_MANUAL) return "manual";
    return "unconfigured";
}

static const char *dhcp_state_name(u8 state)
{
    if (state == MG_NET_DHCP_STATE_ACQUIRING) return "acquiring";
    if (state == MG_NET_DHCP_STATE_BOUND) return "bound";
    if (state == MG_NET_DHCP_STATE_RENEWING) return "renewing";
    if (state == MG_NET_DHCP_STATE_REBINDING) return "rebinding";
    return "inactive";
}

static void duration(u64 milliseconds, char *text, usize capacity)
{
    u64 seconds = milliseconds / 1000ULL;
    u32 days;
    u32 hours;
    u32 minutes;

    if (!text || capacity == 0) return;
    days = (u32)(seconds / 86400ULL);
    seconds %= 86400ULL;
    hours = (u32)(seconds / 3600ULL);
    seconds %= 3600ULL;
    minutes = (u32)(seconds / 60ULL);
    seconds %= 60ULL;
    if (days) (void)snprintf(text, capacity, "%ud %uh", days, hours);
    else if (hours) (void)snprintf(text, capacity, "%uh %um", hours, minutes);
    else if (minutes) (void)snprintf(text, capacity, "%um %us", minutes,
                                     (u32)seconds);
    else (void)snprintf(text, capacity, "%us", (u32)seconds);
}

static bool parse_decimal_prefix(const char *text, u8 *prefix)
{
    u32 value = 0;
    u32 digits = 0;
    usize index = 0;

    if (!text || !prefix) return false;
    while (text[index] >= '0' && text[index] <= '9') {
        value = value * 10U + (u32)(text[index++] - '0');
        if (++digits > 2U || value > 32U) return false;
    }
    if (!digits || text[index] != '\0') return false;
    *prefix = (u8)value;
    return true;
}

static bool parse_cidr(const char *text, mg_ipv4_addr_t *address, u8 *prefix)
{
    char address_text[16];
    usize length;
    usize slash = 0;

    if (!text || !address || !prefix) return false;
    length = strlen(text);
    for (usize index = 0; index < length; index++) {
        if (text[index] == '/') {
            if (slash || index == 0 || index >= sizeof(address_text))
                return false;
            slash = index;
        }
    }
    if (!slash || slash >= length || slash >= sizeof(address_text) ||
        length - slash - 1U >= 4U) return false;
    memcpy(address_text, text, slash);
    address_text[slash] = '\0';
    return mg_ipv4_parse(address_text, address) &&
           parse_decimal_prefix(text + slash + 1U, prefix);
}

static mg_result_t network_call(const mg_network_request_t *request,
                                mg_network_response_t *response)
{
    mg_handle_t endpoint;
    mg_ipc_message_t message = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    if (!request || !response) return MG_ERR_BAD_ARGUMENT;
    result = service_lookup("network", &endpoint);
    if (result != MG_OK) return result;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_NETWORK_REQUEST;
    message.payload_length = sizeof(*request);
    memcpy(message.payload, request, sizeof(*request));
    result = ipc_request(endpoint, &message, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_NETWORK_RESPONSE ||
        reply.payload_length != sizeof(*response))
        return MG_ERR_PROTOCOL;
    memcpy(response, reply.payload, sizeof(*response));
    return response->result;
}

static void print_network_error(mg_result_t result)
{
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("This action requires administrator privileges.\n");
    else if (result == MG_ERR_CANCELLED)
        printf("Action cancelled.\n");
    else if (result == MG_ERR_SERVICE_UNAVAILABLE)
        printf("Network service unavailable.\n");
    else if (result == MG_ERR_NETWORK_UNAVAILABLE)
        printf("Network unavailable.\n");
    else if (result == MG_ERR_NOT_FOUND)
        printf("Network interface not found.\n");
    else if (result == MG_ERR_TIMEOUT)
        printf("DHCP configuration failed.\n");
    else if (result == MG_ERR_BAD_ARGUMENT)
        printf("Invalid network configuration.\n");
    else
        printf("Network operation failed: %s.\n", error_string(result));
}

static bool get_status(u16 operation, const char *name,
                       mg_network_response_t *response)
{
    mg_network_request_t request = {0};
    mg_result_t result;

    request.version = MG_NETWORK_PROTOCOL_VERSION;
    request.operation = operation;
    if (name) strncpy(request.interface_name, name,
                      sizeof(request.interface_name) - 1U);
    result = network_call(&request, response);
    if (result != MG_OK) {
        print_network_error(result);
        return false;
    }
    return true;
}

static bool first_interface(char name[MG_NET_NAME_MAX])
{
    mg_network_response_t response;
    mg_net_interface_info_t interfaces[MG_NETWORK_MAX_INTERFACES];

    if (!get_status(MG_NETWORK_OP_INTERFACES, NULL, &response) ||
        response.count == 0 || response.count > MG_NETWORK_MAX_INTERFACES)
        return false;
    memcpy(interfaces, response.data, response.count * sizeof(interfaces[0]));
    strncpy(name, interfaces[0].name, MG_NET_NAME_MAX - 1U);
    name[MG_NET_NAME_MAX - 1U] = '\0';
    return true;
}

static void print_interface(const mg_net_interface_info_t *interface,
                            const mg_net_info_t *info)
{
    char address[16];
    char netmask[16];
    char remaining[32];
    char renew[32];
    char rebind[32];

    ip(&interface->address, address);
    ip(&interface->netmask, netmask);
    printf("%s\n", interface->name);
    printf("  State       %s\n", interface->enabled ?
           (interface->link_up ? "connected" : "down") : "disabled");
    printf("  Mode        %s\n", info ? mode_name(info->mode) : "unconfigured");
    if (info && info->configured)
        printf("  Address     %s/%u\n", address, info->prefix_length);
    else
        printf("  Address     %s\n", address);
    if (info && info->mode == MG_NET_MODE_DHCP) {
        printf("  DHCP State  %s\n", dhcp_state_name(info->dhcp_state));
        if (info->configured) {
            duration(info->lease_remaining_ms, remaining, sizeof(remaining));
            duration(info->renew_in_ms, renew, sizeof(renew));
            duration(info->rebind_in_ms, rebind, sizeof(rebind));
            printf("  Lease       %s\n  Renew in    %s\n"
                   "  Rebind in   %s\n", remaining, renew, rebind);
        }
    }
    printf("  MAC         ");
    mac(interface->mac);
    printf("\n  MTU         %u\n", interface->mtu);
}

static void overview(void)
{
    mg_network_response_t response;
    mg_network_status_t status;

    if (!get_status(MG_NETWORK_OP_STATUS, NULL, &response)) return;
    if (response.count == 0) {
        printf("No network interfaces.\n");
        return;
    }
    if (response.count > MG_NETWORK_MAX_INTERFACES) {
        printf("Network status unavailable.\n");
        return;
    }
    memcpy(&status, response.data, sizeof(status));
    for (u32 index = 0; index < response.count; index++)
        print_interface(&status.interfaces[index], &status.info);
}

static void interfaces(void)
{
    mg_network_response_t response;
    mg_net_interface_info_t entries[MG_NETWORK_MAX_INTERFACES];
    mg_table_t table;

    if (!get_status(MG_NETWORK_OP_INTERFACES, NULL, &response)) return;
    if (response.count > MG_NETWORK_MAX_INTERFACES) {
        printf("Network status unavailable.\n");
        return;
    }
    memcpy(entries, response.data, response.count * sizeof(entries[0]));
    table_init(&table, network_rows, MG_TABLE_MAX_ROWS);
    {
        mg_table_row_t *row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_INTERFACE_NAME_COLUMN, "NAME");
        table_row_column(row, &NETWORK_INTERFACE_TYPE_COLUMN, "TYPE");
        table_row_column(row, &NETWORK_INTERFACE_STATE_COLUMN, "STATE");
        table_row_column(row, &NETWORK_INTERFACE_IPV4_COLUMN, "IPV4");
        table_row_column(row, &NETWORK_INTERFACE_COUNTER_COLUMN, "RX");
        table_row_column(row, &NETWORK_INTERFACE_COUNTER_COLUMN, "TX");
    }
    for (u32 index = 0; index < response.count; index++) {
        char address[16];
        mg_table_row_t *row;
        ip(&entries[index].address, address);
        row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_INTERFACE_NAME_COLUMN,
                         entries[index].name);
        table_row_column(row, &NETWORK_INTERFACE_TYPE_COLUMN,
                         entries[index].type);
        table_row_column(row, &NETWORK_INTERFACE_STATE_COLUMN,
                         entries[index].enabled && entries[index].link_up
                         ? "up" : (entries[index].enabled ? "down" :
                                   "disabled"));
        table_row_column(row, &NETWORK_INTERFACE_IPV4_COLUMN, address);
        table_row_u64_column(row, &NETWORK_INTERFACE_COUNTER_COLUMN,
                             entries[index].rx_packets);
        table_row_u64_column(row, &NETWORK_INTERFACE_COUNTER_COLUMN,
                             entries[index].tx_packets);
    }
    if (!table_render(&table)) printf("Network status unavailable.\n");
}

static void routes(void)
{
    mg_network_response_t response;
    mg_net_route_info_t entries[4];
    mg_table_t table;

    if (!get_status(MG_NETWORK_OP_ROUTES, NULL, &response)) return;
    if (response.count > 4U) {
        printf("Network status unavailable.\n");
        return;
    }
    memcpy(entries, response.data, response.count * sizeof(entries[0]));
    table_init(&table, network_rows, MG_TABLE_MAX_ROWS);
    {
        mg_table_row_t *row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_ROUTE_DESTINATION_COLUMN,
                         "DESTINATION");
        table_row_column(row, &NETWORK_ROUTE_GATEWAY_COLUMN, "GATEWAY");
        table_row_column(row, &NETWORK_ROUTE_INTERFACE_COLUMN, "INTERFACE");
    }
    for (u32 index = 0; index < response.count; index++) {
        char destination[16], netmask[16], gateway[16];
        char destination_text[40];
        mg_table_row_t *row;
        ip(&entries[index].destination, destination);
        ip(&entries[index].netmask, netmask);
        ip(&entries[index].gateway, gateway);
        (void)snprintf(destination_text, sizeof(destination_text), "%s%s%s",
                       entries[index].is_default ? "default" : destination,
                       entries[index].is_default ? "" : "/",
                       entries[index].is_default ? "" : netmask);
        row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_ROUTE_DESTINATION_COLUMN,
                         destination_text);
        table_row_column(row, &NETWORK_ROUTE_GATEWAY_COLUMN,
                         entries[index].is_default ? gateway : "direct");
        table_row_column(row, &NETWORK_ROUTE_INTERFACE_COLUMN,
                         entries[index].interface_name);
    }
    if (!table_render(&table)) printf("Network status unavailable.\n");
}

static void neighbors(void)
{
    mg_network_response_t response;
    mg_net_neighbor_info_t entries[16];
    mg_table_t table;

    if (!get_status(MG_NETWORK_OP_NEIGHBORS, NULL, &response)) return;
    if (response.count > 16U) {
        printf("Network status unavailable.\n");
        return;
    }
    memcpy(entries, response.data, response.count * sizeof(entries[0]));
    table_init(&table, network_rows, MG_TABLE_MAX_ROWS);
    {
        mg_table_row_t *row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_NEIGHBOR_ADDRESS_COLUMN, "ADDRESS");
        table_row_column(row, &NETWORK_NEIGHBOR_HARDWARE_COLUMN, "HARDWARE");
        table_row_column(row, &NETWORK_NEIGHBOR_STATE_COLUMN, "STATE");
    }
    for (u32 index = 0; index < response.count; index++) {
        char address[16];
        char hardware[18];
        mg_table_row_t *row;
        ip(&entries[index].address, address);
        format_mac(entries[index].mac, hardware);
        row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_NEIGHBOR_ADDRESS_COLUMN, address);
        table_row_column(row, &NETWORK_NEIGHBOR_HARDWARE_COLUMN, hardware);
        table_row_column(row, &NETWORK_NEIGHBOR_STATE_COLUMN,
                         entries[index].state ? "known" : "unknown");
    }
    if (!table_render(&table)) printf("Network status unavailable.\n");
}

static void connections(void)
{
    mg_network_response_t response;
    mg_net_connection_info_t entries[8];
    mg_table_t table;

    if (!get_status(MG_NETWORK_OP_CONNECTIONS, NULL, &response)) return;
    if (response.count > 8U) {
        printf("Network status unavailable.\n");
        return;
    }
    memcpy(entries, response.data, response.count * sizeof(entries[0]));
    table_init(&table, network_rows, MG_TABLE_MAX_ROWS);
    {
        mg_table_row_t *row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_CONNECTION_PROTOCOL_COLUMN,
                         "PROTOCOL");
        table_row_column(row, &NETWORK_CONNECTION_ENDPOINT_COLUMN, "LOCAL");
        table_row_column(row, &NETWORK_CONNECTION_ENDPOINT_COLUMN, "REMOTE");
        table_row_column(row, &NETWORK_CONNECTION_STATE_COLUMN, "STATE");
        table_row_column(row, &NETWORK_CONNECTION_PROCESS_COLUMN, "PROCESS");
    }
    for (u32 index = 0; index < response.count; index++) {
        char local[32], remote[32], state[16];
        mg_table_row_t *row;
        (void)snprintf(local, sizeof(local), "%u:%u",
                       entries[index].local_address.octet[3],
                       entries[index].local_port);
        (void)snprintf(remote, sizeof(remote), "%u:%u",
                       entries[index].remote_address.octet[3],
                       entries[index].remote_port);
        (void)snprintf(state, sizeof(state), "%u", entries[index].state);
        row = table_row_begin(&table);
        if (!row) {
            printf("Network status unavailable.\n");
            return;
        }
        table_row_column(row, &NETWORK_CONNECTION_PROTOCOL_COLUMN,
                         entries[index].protocol == 6 ? "TCP" : "UDP");
        table_row_column(row, &NETWORK_CONNECTION_ENDPOINT_COLUMN, local);
        table_row_column(row, &NETWORK_CONNECTION_ENDPOINT_COLUMN, remote);
        table_row_column(row, &NETWORK_CONNECTION_STATE_COLUMN, state);
        table_row_column(row, &NETWORK_CONNECTION_PROCESS_COLUMN,
                         entries[index].process_name);
    }
    if (!table_render(&table)) printf("Network status unavailable.\n");
}

static int configure_manual(int argc, char **argv)
{
    mg_network_request_t request = {0};
    const char *interface_name;
    const char *address_text;
    const char *gateway_text;
    const char *dns_text;

    if (argc == 6) {
        interface_name = argv[2];
        address_text = argv[3];
        gateway_text = argv[4];
        dns_text = argv[5];
    } else if (argc == 5 && first_interface(request.interface_name)) {
        interface_name = request.interface_name;
        address_text = argv[2];
        gateway_text = argv[3];
        dns_text = argv[4];
    } else {
        command_usage_error(argv[0],
                            "netcfg manual <interface> <address/prefix> "
                            "<gateway> <dns>", NULL);
        return 1;
    }
    if (argc == 6)
        strncpy(request.interface_name, interface_name,
                sizeof(request.interface_name) - 1U);
    if (!parse_cidr(address_text, &request.manual.address,
                    &request.manual.prefix_length) ||
        !mg_ipv4_parse(gateway_text, &request.manual.gateway) ||
        !mg_ipv4_parse(dns_text, &request.manual.dns)) {
        command_usage_error(argv[0],
                            "netcfg manual <interface> <address/prefix> "
                            "<gateway> <dns>", NULL);
        return 1;
    }
    request.version = MG_NETWORK_PROTOCOL_VERSION;
    request.operation = MG_NETWORK_OP_SET_MANUAL;
    mg_network_response_t response;
    mg_result_t result = network_call(&request, &response);
    if (result != MG_OK) print_network_error(result);
    else printf("Network configured manually.\n");
    return result < 0;
}

static int configure_automatic(int argc, char **argv)
{
    mg_network_request_t request = {0};
    mg_network_response_t response;

    if (argc == 2) {
        if (!first_interface(request.interface_name)) return 1;
    } else if (argc == 3) {
        strncpy(request.interface_name, argv[2],
                sizeof(request.interface_name) - 1U);
    } else {
        command_usage_error(argv[0], "netcfg automatic [<interface>]", NULL);
        return 1;
    }
    request.version = MG_NETWORK_PROTOCOL_VERSION;
    request.operation = MG_NETWORK_OP_SET_AUTOMATIC;
    mg_result_t result = network_call(&request, &response);
    if (result != MG_OK) print_network_error(result);
    else printf("Network configured automatically.\n");
    return result < 0;
}

static int toggle_interface(int argc, char **argv, u16 operation)
{
    mg_network_request_t request = {0};
    mg_network_response_t response;

    if (argc != 3) {
        command_usage_error(argv[0], operation == MG_NETWORK_OP_ENABLE ?
                            "netcfg enable <interface>" :
                            "netcfg disable <interface>", NULL);
        return 1;
    }
    request.version = MG_NETWORK_PROTOCOL_VERSION;
    request.operation = operation;
    strncpy(request.interface_name, argv[2],
            sizeof(request.interface_name) - 1U);
    mg_result_t result = network_call(&request, &response);
    if (result != MG_OK) print_network_error(result);
    else printf("Network interface %s.\n", operation == MG_NETWORK_OP_ENABLE ?
                "enabled" : "disabled");
    return result < 0;
}

#if defined(NETWORK_CLIENT_INFO)
int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc == 1) {
        overview();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "status")) {
        overview();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "status")) {
        mg_network_response_t response;
        if (!get_status(MG_NETWORK_OP_STATUS_INTERFACE, argv[2], &response))
            return 1;
        if (response.count != 1U) return 1;
        mg_network_status_t status;
        memcpy(&status, response.data, sizeof(status));
        print_interface(&status.interfaces[0], &status.info);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "interfaces")) {
        interfaces();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "routes")) {
        routes();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "neighbors")) {
        neighbors();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "connections")) {
        connections();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "dns")) {
        mg_network_response_t response;
        mg_network_status_t status;
        char dns[16];
        if (!get_status(MG_NETWORK_OP_STATUS, NULL, &response)) return 1;
        memcpy(&status, response.data, sizeof(status));
        ip(&status.info.dns, dns);
        printf("DNS\n  Server      %s\n  Source      %s\n", dns,
               status.info.mode == MG_NET_MODE_MANUAL ? "Manual" : "DHCP");
        return 0;
    }
    command_usage_error(argv[0],
                        "netinfo [status|interfaces|routes|neighbors|"
                        "connections|dns]", argc > 1 && argv[1][0] == '-' ?
                        argv[1] : NULL);
    return 1;
}
#elif defined(NETWORK_CLIENT_CFG)
int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc == 2 && !strcmp(argv[1], "renew")) {
        mg_network_request_t request = {
            .version = MG_NETWORK_PROTOCOL_VERSION,
            .operation = MG_NETWORK_OP_RENEW,
        };
        mg_network_response_t response;
        mg_result_t result = network_call(&request, &response);
        if (result != MG_OK) print_network_error(result);
        else printf("Network configuration renewed.\n");
        return result < 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "automatic"))
        return configure_automatic(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "manual"))
        return configure_manual(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "enable"))
        return toggle_interface(argc, argv, MG_NETWORK_OP_ENABLE);
    if (argc >= 2 && !strcmp(argv[1], "disable"))
        return toggle_interface(argc, argv, MG_NETWORK_OP_DISABLE);
    if (argc == 2 && !strcmp(argv[1], "reload")) {
        mg_network_request_t request = {
            .version = MG_NETWORK_PROTOCOL_VERSION,
            .operation = MG_NETWORK_OP_RELOAD,
        };
        mg_network_response_t response;
        mg_result_t result = network_call(&request, &response);
        if (result != MG_OK) print_network_error(result);
        else printf("Network configuration reloaded.\n");
        return result < 0;
    }
    command_usage_error(argv[0],
                        "netcfg <renew|automatic|manual|enable|disable|"
                        "reload>", argc > 1 && argv[1][0] == '-' ?
                        argv[1] : NULL);
    return 1;
}
#else
#error "select NETWORK_CLIENT_INFO or NETWORK_CLIENT_CFG"
#endif
