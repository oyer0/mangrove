#include <net/net.h>
#include <net/config.h>
#include <mg/net.h>
#include <mg/event.h>
#include <ipc.h>
#include <string.h>

static net_device_t *primary_device;
static net_device_t *devices[NET_MAX_DEVICES];
static u32 device_count;
static u64 next_device_serial;
static u32 next_display_name;
#define NET_DISPLAY_NAME_COUNT 256U
#define NET_DISPLAY_NAME_MAX   16U
static char ethernet_names[NET_DISPLAY_NAME_COUNT][NET_DISPLAY_NAME_MAX];
static net_receive_handler_t receive_handler;
static u64 received_frames;
static u64 received_bytes;
static u64 transmitted_frames;

static bool format_display_name(char *output, usize capacity, u32 number)
{
    char digits[10];
    u32 count = 0;

    if (!output || capacity < 5U || number >= 1000000000U) return false;
    output[0] = 'e';
    output[1] = 't';
    output[2] = 'h';
    do {
        digits[count++] = (char)('0' + number % 10U);
        number /= 10U;
    } while (number && count < sizeof(digits));
    if (3U + count + 1U > capacity) return false;
    for (u32 index = 0; index < count; index++)
        output[3U + index] = digits[count - index - 1U];
    output[3U + count] = '\0';
    return true;
}

void net_init(void)
{
    primary_device = 0;
    memset(devices, 0, sizeof(devices));
    device_count = 0;
    next_device_serial = 0;
    next_display_name = 0;
    receive_handler = 0;
    received_frames = 0;
    received_bytes = 0;
    transmitted_frames = 0;
}

bool net_register_device(net_device_t *device)
{
    u32 name_index;
    if (!device || !device->name || !device->transmit || device->mtu == 0 ||
        device_count >= NET_MAX_DEVICES) {
        return false;
    }
    for (u32 index = 0; index < device_count; index++)
        if (devices[index] == device) return false;
    /* Names are boot-local presentation identities.  Do not recycle one
       after removal: an old eth0 reference must never describe a new NIC. */
    name_index = next_display_name++;
    if (name_index >= NET_DISPLAY_NAME_COUNT ||
        !format_display_name(ethernet_names[name_index],
                             sizeof(ethernet_names[name_index]), name_index))
        return false;
    next_device_serial++;
    if (next_device_serial == 0) next_device_serial++;
    device->id = NET_DEVICE_ID_BASE | next_device_serial;
    device->generation = device->id;
    /* Drivers provide the low-level device object; the generic registry owns
       the user-facing namespace so two Ethernet drivers cannot both claim
       eth0. */
    device->name = ethernet_names[name_index];
    devices[device_count++] = device;
    if (!primary_device) primary_device = device;
    device->administrative_enabled = true;
    ipc_publish_event(MG_EVENT_CLASS_NETWORK,
                      MG_EVENT_NETWORK_INTERFACE_ADDED,
                      device->id, device->name);
    return true;
}

net_device_t *net_primary_device(void)
{
    return primary_device;
}

u32 net_device_count(void)
{
    return device_count;
}

net_device_t *net_device_at(u32 index)
{
    return index < device_count ? devices[index] : NULL;
}

bool net_unregister_device(net_device_t *device)
{
    u32 index;
    if (!device) return false;
    for (index = 0; index < device_count; index++)
        if (devices[index] == device) break;
    if (index == device_count) return false;
    device->administrative_enabled = false;
    if (primary_device == device)
        net_config_clear();
    for (; index + 1U < device_count; index++)
        devices[index] = devices[index + 1U];
    devices[--device_count] = NULL;
    if (primary_device == device)
        primary_device = device_count ? devices[0] : NULL;
    ipc_publish_event(MG_EVENT_CLASS_NETWORK,
                      MG_EVENT_NETWORK_INTERFACE_REMOVED,
                      device->id,
                      device->name);
    return true;
}

bool net_device_enabled(const net_device_t *device)
{
    return device && device->administrative_enabled;
}

bool net_device_current(const net_device_t *device)
{
    return device && net_device_instance_current(device, device->generation);
}

bool net_device_instance_current(const net_device_t *device, u64 generation)
{
    if (!device || !generation) return false;
    for (u32 index = 0; index < device_count; index++)
        if (devices[index] == device &&
            devices[index]->generation == generation)
            return true;
    return false;
}

bool net_set_device_enabled(net_device_t *device, bool enabled)
{
    if (!device) return false;
    bool registered = false;
    for (u32 index = 0; index < device_count; index++)
        registered |= devices[index] == device;
    if (!registered) return false;
    device->administrative_enabled = enabled;
    return true;
}

bool net_device_set_link(net_device_t *device, bool known, bool up)
{
    bool changed;
    bool registered = false;
    if (!device) return false;
    for (u32 index = 0; index < device_count; index++) {
        if (devices[index] == device) {
            registered = true;
            break;
        }
    }
    if (!registered) return false;
    changed = device->link_known != known ||
              (known && device->link_up != up);
    device->link_known = known;
    device->link_up = known && up;
    if (changed && known)
        ipc_publish_event(MG_EVENT_CLASS_NETWORK,
                          up ? MG_EVENT_NETWORK_LINK_UP :
                               MG_EVENT_NETWORK_LINK_DOWN,
                          device->id,
                          device->name);
    return changed;
}

bool netdev_transmit(net_device_t *device, const void *frame, usize length)
{
    if (!net_device_current(device) || !device->administrative_enabled ||
        (device->link_known && !device->link_up) || !frame ||
        length < NET_ETHERNET_HEADER_SIZE ||
        length > NET_ETHERNET_MAX_FRAME || !device->transmit) {
        return false;
    }
    if (!device->transmit(device, frame, length)) {
        return false;
    }
    transmitted_frames++;
    return true;
}

void net_set_receive_handler(net_receive_handler_t handler)
{
    receive_handler = handler;
}

void net_receive_frame(net_device_t *device, const u8 *frame, usize length)
{
    if (!net_device_current(device) || !device->administrative_enabled ||
        (device->link_known && !device->link_up) || !frame ||
        length < NET_ETHERNET_HEADER_SIZE ||
        length > NET_ETHERNET_MAX_FRAME) {
        return;
    }
    received_frames++;
    received_bytes += length;
    if (receive_handler) {
        receive_handler(device, frame, length);
    }
}

u64 net_received_frames(void)
{
    return received_frames;
}

u64 net_received_bytes(void)
{
    return received_bytes;
}

u64 net_transmitted_frames(void)
{
    return transmitted_frames;
}

bool net_fill_interface(void *output, usize capacity, usize *count)
{
    mg_net_interface_info_t *entries = output;
    const net_config_t *configuration = net_config();
    usize available;
    if (!count) return false;
    *count = device_count;
    if (!entries) return true;
    available = capacity / sizeof(*entries);
    for (u32 index = 0; index < device_count && index < available; index++) {
        mg_net_interface_info_t info = {0};
        net_device_t *device = devices[index];
        info.id = device->id;
        strncpy(info.name, device->name ? device->name : "ethernet0",
                sizeof(info.name) - 1U);
        strncpy(info.type, "ethernet", sizeof(info.type) - 1U);
        info.link_up = device->link_up;
        info.enabled = device->administrative_enabled;
        info.link_known = device->link_known ? 1U : 0U;
        for (u32 i = 0; i < 6; i++) info.mac[i] = device->mac[i];
        info.mtu = (u32)device->mtu;
        if (device == primary_device) {
            for (u32 i = 0; i < 4; i++) {
                info.address.octet[i] = configuration->address.octet[i];
                info.netmask.octet[i] = configuration->netmask.octet[i];
            }
            info.rx_packets = received_frames;
            info.tx_packets = transmitted_frames;
        }
        entries[index] = info;
    }
    return true;
}

bool net_fill_routes(void *output, usize capacity, usize *count)
{
    mg_net_route_info_t *routes = output;
    const net_config_t *c = net_config();
    usize needed = c->configured ? (c->has_gateway ? 2 : 1) : 0;
    if (!count) return false;
    *count = needed;
    if (!output || capacity < sizeof(*routes) || !needed) return true;
    routes[0] = (mg_net_route_info_t){0};
    for (u32 i = 0; i < 4; i++) {
        routes[0].destination.octet[i] = c->address.octet[i] & c->netmask.octet[i];
        routes[0].netmask.octet[i] = c->netmask.octet[i];
    }
    strncpy(routes[0].interface_name, primary_device && primary_device->name ? primary_device->name : "ethernet0", MG_NET_NAME_MAX - 1);
    if (c->has_gateway) {
        routes[1] = (mg_net_route_info_t){0};
        for (u32 i = 0; i < 4; i++) routes[1].gateway.octet[i] = c->gateway.octet[i];
        routes[1].is_default = true;
        strncpy(routes[1].interface_name, routes[0].interface_name, MG_NET_NAME_MAX - 1);
    }
    return true;
}
