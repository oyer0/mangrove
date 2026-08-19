#include <net/net.h>
#include <net/config.h>
#include <mg/net.h>
#include <string.h>

static net_device_t *primary_device;
static net_receive_handler_t receive_handler;
static u64 received_frames;
static u64 received_bytes;
static u64 transmitted_frames;

void net_init(void)
{
    primary_device = 0;
    receive_handler = 0;
    received_frames = 0;
    received_bytes = 0;
    transmitted_frames = 0;
}

bool net_register_device(net_device_t *device)
{
    if (!device || !device->name || !device->transmit || device->mtu == 0 ||
        primary_device) {
        return false;
    }
    primary_device = device;
    return true;
}

net_device_t *net_primary_device(void)
{
    return primary_device;
}

bool netdev_transmit(net_device_t *device, const void *frame, usize length)
{
    if (!device || !frame || length < NET_ETHERNET_HEADER_SIZE ||
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
    if (!device || !frame || length < NET_ETHERNET_HEADER_SIZE ||
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
    mg_net_interface_info_t info = {0};
    const net_config_t *configuration = net_config();
    if (!count) return false;
    *count = primary_device ? 1 : 0;
    if (!primary_device || !output || capacity < sizeof(info)) return true;
    strncpy(info.name, primary_device->name ? primary_device->name : "ethernet0", sizeof(info.name) - 1);
    strncpy(info.type, "ethernet", sizeof(info.type) - 1);
    info.link_up = true;
    for (u32 i = 0; i < 6; i++) info.mac[i] = primary_device->mac[i];
    info.mtu = (u32)primary_device->mtu;
    for (u32 i = 0; i < 4; i++) { info.address.octet[i] = configuration->address.octet[i]; info.netmask.octet[i] = configuration->netmask.octet[i]; }
    info.rx_packets = received_frames;
    info.tx_packets = transmitted_frames;
    *(mg_net_interface_info_t *)output = info;
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
