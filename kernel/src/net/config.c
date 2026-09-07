/* SPDX-License-Identifier: GPL-3.0-only */
#include <net/config.h>
#include <net/arp.h>
#include <config_parser.h>
#include <net/dhcp.h>
#include <net/dns.h>
#include <kprint.h>
#include <timer.h>
#include <vfs.h>
#include <string.h>

#define NETWORK_CONFIG_PATH       "/conf/network/config"
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

static bool interface_name_valid(const char *name)
{
    usize length = 0;
    usize prefix_length;

    if (!name || !name[0]) return false;
    if (strncmp(name, "eth", 3) == 0) prefix_length = 3;
    else if (strncmp(name, "wifi", 4) == 0) prefix_length = 4;
    else return false;
    length = strlen(name);
    if (length <= prefix_length || length >= 16U) return false;
    for (usize index = prefix_length; index < length; index++)
        if (name[index] < '0' || name[index] > '9') return false;
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

static bool netmask_valid(net_ipv4_t mask)
{
    bool zero_seen = false;

    for (u32 octet = 0; octet < 4; octet++) {
        for (u8 bit = 0x80U; bit != 0; bit >>= 1U) {
            if (mask.octet[octet] & bit) {
                if (zero_seen) return false;
            } else {
                zero_seen = true;
            }
        }
    }
    return true;
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
    vfs_node_t *conf = NULL;
    vfs_node_t *network = NULL;
    vfs_node_t *file = NULL;
    usize size = sizeof(default_network_config) - 1U;

    if (!root || root->type != VFS_TYPE_DIRECTORY) return false;
    if (!ensure_directory(root, "conf", &conf) ||
        !ensure_directory(conf, "network", &network)) return false;
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
        if (strcmp(key, "interface") != 0 && strcmp(key, "mode") != 0 &&
            strcmp(key, "address") != 0 &&
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
    if (kernel_config_find(&document, "interface")) {
        const char *interface_name = kernel_config_find(&document, "interface");
        if (!interface_name_valid(interface_name) ||
            strlen(interface_name) >= sizeof(output->interface_name)) {
            if (reason) *reason = "network configuration has invalid interface";
            return false;
        }
        strcpy(output->interface_name, interface_name);
    }
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
    dns_reset();
    dhcp_reset();
}

const net_config_t *net_config(void)
{
    return &configuration;
}

static bool dhcp_timing_normalize(u32 lease_seconds, u32 *renewal_seconds,
                                  u32 *rebinding_seconds)
{
    u32 renewal;
    u32 rebinding;

    if (!renewal_seconds || !rebinding_seconds || lease_seconds < 3U)
        return false;
    renewal = *renewal_seconds ? *renewal_seconds : lease_seconds / 2U;
    rebinding = *rebinding_seconds ? *rebinding_seconds :
        (u32)(((u64)lease_seconds * 7ULL) / 8ULL);
    if (!renewal) renewal = 1U;
    if (rebinding <= renewal) rebinding = renewal + 1U;
    if (rebinding >= lease_seconds) rebinding = lease_seconds - 1U;
    if (!renewal || renewal >= rebinding || rebinding >= lease_seconds)
        return false;
    *renewal_seconds = renewal;
    *rebinding_seconds = rebinding;
    return true;
}

static bool deadline_after(u64 start, u32 seconds, u64 *deadline)
{
    u64 duration;

    if (!deadline) return false;
    duration = (u64)seconds * 1000ULL;
    if (start > ~(u64)0 - duration) return false;
    *deadline = start + duration;
    return true;
}

static void remember_primary_interface(void)
{
    const net_device_t *device = net_primary_device();

    configuration.interface_name[0] = '\0';
    if (device && device->name)
        strncpy(configuration.interface_name, device->name,
                sizeof(configuration.interface_name) - 1U);
}

void net_config_begin_dhcp(void)
{
    configuration = (net_config_t){0};
    configuration.mode = NET_CONFIG_MODE_DHCP;
    configuration.dhcp_state = NET_DHCP_STATE_ACQUIRING;
    remember_primary_interface();
}

bool net_config_apply_dhcp(const net_ipv4_t *address,
                           const net_ipv4_t *netmask,
                           const net_ipv4_t *gateway, bool has_gateway,
                           const net_ipv4_t *dns, bool has_dns,
                           const net_ipv4_t *server, u32 lease_seconds,
                           u32 renewal_seconds, u32 rebinding_seconds)
{
    u64 now;

    if (!address || !netmask || !server || ipv4_zero(*address) ||
        !netmask_valid(*netmask)) return false;
    if (!dhcp_timing_normalize(lease_seconds, &renewal_seconds,
                               &rebinding_seconds)) return false;
    now = timer_uptime_ms();
    if (!deadline_after(now, lease_seconds, &configuration.expiry_deadline_ms) ||
        !deadline_after(now, renewal_seconds,
                        &configuration.renewal_deadline_ms) ||
        !deadline_after(now, rebinding_seconds,
                        &configuration.rebinding_deadline_ms))
        return false;
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
    configuration.renewal_seconds = renewal_seconds;
    configuration.rebinding_seconds = rebinding_seconds;
    configuration.lease_acquired_ms = now;
    configuration.retry_deadline_ms = configuration.renewal_deadline_ms;
    configuration.retry_delay_ms = 1000U;
    configuration.dhcp_state = NET_DHCP_STATE_BOUND;
    configuration.configured = true;
    remember_primary_interface();
    return true;
}

void net_config_mark_dhcp_attempt(bool rebinding)
{
    u64 now;
    u64 retry;
    net_dhcp_state_t next_state;

    if (configuration.mode != NET_CONFIG_MODE_DHCP ||
        !configuration.configured) return;
    now = timer_uptime_ms();
    next_state = rebinding ? NET_DHCP_STATE_REBINDING :
        NET_DHCP_STATE_RENEWING;
    if (configuration.dhcp_state != next_state) {
        configuration.dhcp_state = next_state;
        configuration.retry_delay_ms = 1000U;
    } else if (!configuration.retry_delay_ms) {
        configuration.retry_delay_ms = 1000U;
    }
    retry = now > ~(u64)0 - configuration.retry_delay_ms ?
        ~(u64)0 : now + configuration.retry_delay_ms;
    configuration.retry_deadline_ms = retry;
}

void net_config_mark_dhcp_failure(void)
{
    u64 now;
    u64 limit;
    u64 retry;

    if (configuration.mode != NET_CONFIG_MODE_DHCP ||
        !configuration.configured ||
        (configuration.dhcp_state != NET_DHCP_STATE_RENEWING &&
         configuration.dhcp_state != NET_DHCP_STATE_REBINDING)) return;
    now = timer_uptime_ms();
    if (configuration.retry_delay_ms < 4000U) {
        configuration.retry_delay_ms *= 2U;
        if (configuration.retry_delay_ms > 4000U)
            configuration.retry_delay_ms = 4000U;
    }
    limit = configuration.dhcp_state == NET_DHCP_STATE_RENEWING ?
        configuration.rebinding_deadline_ms : configuration.expiry_deadline_ms;
    retry = now > ~(u64)0 - configuration.retry_delay_ms ?
        ~(u64)0 : now + configuration.retry_delay_ms;
    configuration.retry_deadline_ms = retry < limit ? retry : limit;
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
    configuration.renewal_seconds = 0;
    configuration.rebinding_seconds = 0;
    configuration.lease_acquired_ms = 0;
    configuration.renewal_deadline_ms = 0;
    configuration.rebinding_deadline_ms = 0;
    configuration.expiry_deadline_ms = 0;
    configuration.retry_deadline_ms = 0;
    configuration.retry_delay_ms = 0;
    configuration.dhcp_state = NET_DHCP_STATE_NONE;
    configuration.prefix_length = prefix_length;
    configuration.mode = NET_CONFIG_MODE_MANUAL;
    configuration.has_gateway = true;
    configuration.has_dns = true;
    configuration.configured = true;
    remember_primary_interface();
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
