#include <net/config.h>
#include <net/arp.h>
#include <config_parser.h>
#include <net/dhcp.h>
#include <kprint.h>
#include <vfs.h>
#include <string.h>

#define NETWORK_CONFIG_PATH       "/core/network/config"
#define NETWORK_CONFIG_MAX_BYTES  1024U

static const char default_network_config[] =
    "// Mangrove network configuration\n"
    "\n"
    "mode=dhcp\n";

static net_config_t configuration;

static bool ipv4_zero(net_ipv4_t address)
{
    return address.octet[0] == 0 && address.octet[1] == 0 &&
           address.octet[2] == 0 && address.octet[3] == 0;
}

static bool parse_ipv4(const char *text, net_ipv4_t *address)
{
    usize position = 0;

    if (!text || !address) return false;
    for (u32 octet = 0; octet < 4; octet++) {
        u32 value = 0;
        u32 digits = 0;
        while (text[position] >= '0' && text[position] <= '9') {
            value = value * 10U + (u32)(text[position++] - '0');
            if (++digits > 3U || value > 255U) return false;
        }
        if (!digits) return false;
        address->octet[octet] = (u8)value;
        if (octet != 3U) {
            if (text[position++] != '.') return false;
        } else if (text[position] != '\0') {
            return false;
        }
    }
    return true;
}

static bool parse_decimal_prefix(const char *text, u8 *prefix)
{
    u32 value = 0;
    u32 digits = 0;
    usize position = 0;

    if (!text || !prefix) return false;
    while (text[position] >= '0' && text[position] <= '9') {
        value = value * 10U + (u32)(text[position++] - '0');
        if (++digits > 2U || value > 32U) return false;
    }
    if (!digits || text[position] != '\0') return false;
    *prefix = (u8)value;
    return true;
}

static bool parse_cidr(const char *text, net_ipv4_t *address, u8 *prefix)
{
    char address_text[16];
    usize slash = 0;
    usize length;

    if (!text || !address || !prefix) return false;
    length = strlen(text);
    for (usize index = 0; index < length; index++) {
        if (text[index] == '/') {
            if (slash || index == 0 || index >= sizeof(address_text))
                return false;
            slash = index;
        }
    }
    if (!slash || slash >= length || length - slash - 1U >= 4U ||
        slash >= sizeof(address_text)) return false;
    memcpy(address_text, text, slash);
    address_text[slash] = '\0';
    if (!parse_ipv4(address_text, address) ||
        !parse_decimal_prefix(text + slash + 1U, prefix)) return false;
    return true;
}

static net_ipv4_t netmask_from_prefix(u8 prefix)
{
    net_ipv4_t mask = {{0, 0, 0, 0}};
    for (u32 index = 0; index < 4; index++) {
        if (prefix >= 8U) {
            mask.octet[index] = 0xffU;
            prefix -= 8U;
        } else if (prefix != 0U) {
            mask.octet[index] = (u8)(0xffU << (8U - prefix));
            prefix = 0;
        }
    }
    return mask;
}

static u8 prefix_from_netmask(net_ipv4_t mask)
{
    u8 prefix = 0;
    bool zero_seen = false;

    for (u32 octet = 0; octet < 4; octet++) {
        u8 value = mask.octet[octet];
        for (u8 bit = 0x80U; bit != 0; bit >>= 1U) {
            if (value & bit) {
                if (zero_seen) return 0;
                prefix++;
            } else {
                zero_seen = true;
            }
        }
    }
    return prefix;
}

static bool ensure_directory(vfs_node_t *parent, const char *name,
                             vfs_node_t **output)
{
    if (vfs_finddir(parent, name)) {
        *output = vfs_finddir(parent, name);
        return *output && (*output)->type == VFS_TYPE_DIRECTORY;
    }
    return vfs_mkdir(parent, name, output) == VFS_OK;
}

static bool create_default_config(void)
{
    vfs_node_t *root = vfs_get_root_node();
    vfs_node_t *core = NULL;
    vfs_node_t *network = NULL;
    vfs_node_t *file = NULL;
    usize size = sizeof(default_network_config) - 1U;

    if (!root || root->type != VFS_TYPE_DIRECTORY) return false;
    if (!ensure_directory(root, "core", &core) ||
        !ensure_directory(core, "network", &network)) return false;
    if (vfs_finddir(network, "config")) return true;
    if (vfs_create(network, "config", &file) != VFS_OK || !file ||
        vfs_write(file, 0, size, default_network_config) != size) return false;
    return true;
}

static bool read_config_text(char *buffer, usize *length, bool *missing)
{
    vfs_node_t *file = NULL;
    int result;

    if (!buffer || !length || !missing) return false;
    *missing = false;
    result = vfs_lookup(NETWORK_CONFIG_PATH, &file);
    if (result == VFS_ERR_NOT_FOUND) {
        *missing = true;
        return false;
    }
    if (result != VFS_OK || !file || file->type != VFS_TYPE_FILE ||
        file->size >= NETWORK_CONFIG_MAX_BYTES) return false;
    *length = (usize)file->size;
    return vfs_read(file, 0, *length, buffer) == *length;
}

static bool parse_persistent_config(const char *text, usize length,
                                    net_persistent_config_t *output,
                                    const char **reason)
{
    kernel_config_document_t document;
    const char *mode;
    const char *address;
    const char *gateway;
    const char *dns;
    u32 line = 0;

    if (!kernel_config_parse(text, length, &document, &line)) {
        if (reason) *reason = "invalid network configuration syntax";
        return false;
    }
    for (u32 index = 0; index < document.count; index++) {
        const char *key = document.entries[index].key;
        if (strcmp(key, "mode") != 0 && strcmp(key, "address") != 0 &&
            strcmp(key, "gateway") != 0 && strcmp(key, "dns") != 0) {
            KERNEL_BOOT_DEBUG_LOG("[NET-CONFIG] ignored unknown key '%s'\n", key);
        }
    }

    mode = kernel_config_find(&document, "mode");
    if (!mode) {
        if (reason) *reason = "network configuration has no mode";
        return false;
    }
    *output = (net_persistent_config_t){0};
    if (strcmp(mode, "dhcp") == 0) {
        output->mode = NET_CONFIG_MODE_DHCP;
        return true;
    }
    if (strcmp(mode, "manual") != 0) {
        if (reason) *reason = "network configuration has invalid mode";
        return false;
    }
    address = kernel_config_find(&document, "address");
    gateway = kernel_config_find(&document, "gateway");
    dns = kernel_config_find(&document, "dns");
    if (!address || !gateway || !dns || !parse_cidr(address, &output->address,
                                                     &output->prefix_length) ||
        !parse_ipv4(gateway, &output->gateway) ||
        !parse_ipv4(dns, &output->dns) || ipv4_zero(output->address) ||
        ipv4_zero(output->gateway) || ipv4_zero(output->dns)) {
        if (reason) *reason = "invalid manual network configuration";
        return false;
    }
    output->mode = NET_CONFIG_MODE_MANUAL;
    return true;
}

void net_config_init(void)
{
    configuration = (net_config_t){0};
}

void net_config_clear(void)
{
    configuration = (net_config_t){0};
    arp_clear_cache();
    dhcp_reset();
}

const net_config_t *net_config(void)
{
    return &configuration;
}

bool net_config_apply_dhcp(const net_ipv4_t *address,
                           const net_ipv4_t *netmask,
                           const net_ipv4_t *gateway, bool has_gateway,
                           const net_ipv4_t *dns, bool has_dns,
                           const net_ipv4_t *server, u32 lease_seconds)
{
    if (!address || !netmask || !server) return false;
    configuration.address = *address;
    configuration.netmask = *netmask;
    configuration.gateway = gateway ? *gateway : (net_ipv4_t){{0}};
    configuration.dns = dns ? *dns : (net_ipv4_t){{0}};
    configuration.dhcp_server = *server;
    configuration.prefix_length = prefix_from_netmask(*netmask);
    configuration.mode = NET_CONFIG_MODE_DHCP;
    configuration.has_gateway = has_gateway && gateway != NULL;
    configuration.has_dns = has_dns && dns != NULL;
    configuration.lease_seconds = lease_seconds;
    configuration.configured = true;
    return true;
}

bool net_config_apply_manual(const net_ipv4_t *address, u8 prefix_length,
                             const net_ipv4_t *gateway,
                             const net_ipv4_t *dns)
{
    if (!address || !gateway || !dns || prefix_length > 32U ||
        ipv4_zero(*address) || ipv4_zero(*gateway) || ipv4_zero(*dns))
        return false;
    configuration.address = *address;
    configuration.netmask = netmask_from_prefix(prefix_length);
    configuration.gateway = *gateway;
    configuration.dns = *dns;
    configuration.dhcp_server = (net_ipv4_t){{0}};
    configuration.lease_seconds = 0;
    configuration.prefix_length = prefix_length;
    configuration.mode = NET_CONFIG_MODE_MANUAL;
    configuration.has_gateway = true;
    configuration.has_dns = true;
    configuration.configured = true;
    return true;
}

bool net_config_load_persistent(net_persistent_config_t *output,
                                const char **reason)
{
    char buffer[NETWORK_CONFIG_MAX_BYTES];
    usize length = 0;
    bool missing = false;

    if (reason) *reason = NULL;
    if (!output) return false;
    if (!read_config_text(buffer, &length, &missing)) {
        if (!missing) {
            if (reason) *reason = "network configuration could not be read";
            return false;
        }
        *output = (net_persistent_config_t){ .mode = NET_CONFIG_MODE_DHCP };
        (void)create_default_config();
        return true;
    }
    buffer[length] = '\0';
    return parse_persistent_config(buffer, length, output, reason);
}

bool net_network_configured(void)
{
    return configuration.configured;
}

const net_ipv4_t *net_local_ipv4(void)
{
    return &configuration.address;
}

const net_ipv4_t *net_gateway_ipv4(void)
{
    return &configuration.gateway;
}

const net_ipv4_t *net_netmask_ipv4(void)
{
    return &configuration.netmask;
}

bool net_ipv4_equal(net_ipv4_t left, net_ipv4_t right)
{
    return left.octet[0] == right.octet[0] &&
           left.octet[1] == right.octet[1] &&
           left.octet[2] == right.octet[2] &&
           left.octet[3] == right.octet[3];
}
