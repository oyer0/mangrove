#include <mangrove.h>
#include <stdio.h>
#include <string.h>

static void ip(const mg_ipv4_addr_t *address, char out[16])
{
    if (!mg_ipv4_format(address, out, 16)) strcpy(out, "-");
}

static void mac(const u8 value[6])
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x", value[0], value[1], value[2],
           value[3], value[4], value[5]);
}

static const char *mode_name(u8 mode)
{
    if (mode == MG_NET_MODE_DHCP) return "automatic";
    if (mode == MG_NET_MODE_MANUAL) return "manual";
    return "unconfigured";
}

static void help(void)
{
    printf("Usage:\n");
    printf("  network\n");
    printf("  network interfaces\n");
    printf("  network routes\n");
    printf("  network neighbors\n");
    printf("  network connections\n");
    printf("  network dns\n");
    printf("  network renew\n");
    printf("  network automatic\n");
    printf("  network manual <address/prefix> <gateway> <dns>\n");
    printf("  network reload\n");
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

static void overview(void)
{
    mg_net_info_t network;
    mg_net_interface_info_t interface[1];
    char address[16], gateway[16], dns[16];

    if (mg_net_info(&network) < 0 ||
        mg_net_interfaces(interface, sizeof(interface)) < 1) {
        printf("Network unavailable.\n");
        return;
    }
    ip(&network.address, address);
    ip(&network.gateway, gateway);
    ip(&network.dns, dns);
    printf("Ethernet\n");
    printf("  State       %s\n", interface[0].link_up ? "connected" : "down");
    printf("  Mode        %s\n", mode_name(network.mode));
    printf("  Address     %s/%u\n", address, network.prefix_length);
    printf("  Gateway     %s\n", gateway);
    printf("  DNS         %s\n", dns);
    printf("  MAC         ");
    mac(interface[0].mac);
    printf("\n  MTU         %u\n", interface[0].mtu);
}

static void interfaces(void)
{
    mg_net_interface_info_t entries[4];
    mg_result_t count = mg_net_interfaces(entries, sizeof(entries));
    char address[16];

    if (count < 0) {
        printf("Could not query interfaces.\n");
        return;
    }
    printf("Name  Type  State  IPv4  RX  TX\n");
    for (mg_result_t index = 0; index < count && index < 4; index++) {
        ip(&entries[index].address, address);
        printf("%s  %s  %s  %s  %u  %u\n", entries[index].name,
               entries[index].type, entries[index].link_up ? "up" : "down",
               address, (u32)entries[index].rx_packets,
               (u32)entries[index].tx_packets);
    }
}

static void routes(void)
{
    mg_net_route_info_t entries[4];
    mg_result_t count = mg_net_routes(entries, sizeof(entries));
    char destination[16], netmask[16], gateway[16];

    if (count < 0) {
        printf("Could not query routes.\n");
        return;
    }
    printf("Destination      Gateway        Interface\n");
    for (mg_result_t index = 0; index < count && index < 4; index++) {
        ip(&entries[index].destination, destination);
        ip(&entries[index].netmask, netmask);
        ip(&entries[index].gateway, gateway);
        printf("%s/%s %s %s\n", entries[index].is_default ? "default" : destination,
               entries[index].is_default ? "" : netmask,
               entries[index].is_default ? gateway : "direct",
               entries[index].interface_name);
    }
}

static void neighbors(void)
{
    mg_net_neighbor_info_t entries[16];
    mg_result_t count = mg_net_neighbors(entries, sizeof(entries));
    char address[16];

    if (count < 0) {
        printf("Could not query neighbors.\n");
        return;
    }
    printf("Address  Hardware address  State\n");
    for (mg_result_t index = 0; index < count && index < 16; index++) {
        ip(&entries[index].address, address);
        printf("%s  ", address);
        mac(entries[index].mac);
        printf("  %s\n", entries[index].state ? "known" : "unknown");
    }
}

static void connections(void)
{
    mg_net_connection_info_t entries[8];
    mg_result_t count = mg_net_connections(entries, sizeof(entries));

    if (count < 0) {
        printf("Could not query connections.\n");
        return;
    }
    printf("Protocol  Local          Remote         State       Process\n");
    for (mg_result_t index = 0; index < count && index < 8; index++) {
        printf("%s %u:%u -> %u:%u %u %s\n",
               entries[index].protocol == 6 ? "TCP" : "UDP",
               entries[index].local_address.octet[3], entries[index].local_port,
               entries[index].remote_address.octet[3], entries[index].remote_port,
               entries[index].state, entries[index].process_name);
    }
}

static int configure_manual(int argc, char **argv)
{
    mg_net_manual_config_t configuration = {0};
    mg_result_t result;

    if (argc != 5 || !parse_cidr(argv[2], &configuration.address,
                                 &configuration.prefix_length) ||
        !mg_ipv4_parse(argv[3], &configuration.gateway) ||
        !mg_ipv4_parse(argv[4], &configuration.dns)) {
        printf("Usage: network manual <address/prefix> <gateway> <dns>\n");
        return 1;
    }
    result = mg_net_set_manual(&configuration);
    if (result == MG_ERR_NETWORK_UNAVAILABLE)
        printf("Network unavailable.\n");
    else if (result < 0)
        printf("Invalid manual network configuration.\n");
    else
        printf("Network configured manually.\n");
    return result < 0;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        overview();
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
        mg_net_info_t network;
        char dns[16];
        if (mg_net_info(&network) < 0) return 1;
        ip(&network.dns, dns);
        printf("DNS\n  Server      %s\n  Source      %s\n", dns,
               network.mode == MG_NET_MODE_MANUAL ? "Manual" : "DHCP");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "renew")) {
        mg_result_t result = mg_net_renew(MG_NET_TIMEOUT_DEFAULT);
        if (result == MG_ERR_BAD_ARGUMENT)
            printf("DHCP is not active.\n");
        else if (result < 0)
            printf("Network renew failed.\n");
        else
            printf("Network configuration renewed.\n");
        return result < 0;
    }
    if (argc == 2 && !strcmp(argv[1], "automatic")) {
        mg_result_t result = mg_net_set_automatic();
        printf("%s\n", result < 0 ? "Network configuration failed." :
               "Network configured automatically.");
        return result < 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reload")) {
        mg_result_t result = mg_net_reload();
        printf("%s\n", result < 0 ? "Network configuration reload failed." :
               "Network configuration reloaded.");
        return result < 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "manual"))
        return configure_manual(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        help();
        return 0;
    }
    help();
    return 1;
}
