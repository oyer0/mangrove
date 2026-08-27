#include <heap.h>
#include <mangrove_errors.h>
#include <net/config.h>
#include <net/dhcp.h>
#include <net/arp.h>
#include <net/dns.h>
#include <net/icmp.h>
#include <net/net.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/user.h>
#include <scheduler.h>
#include <timer.h>
#include <string.h>

#define NET_USER_DATAGRAM_MAX 1400U
#define NET_USER_DATAGRAM_QUEUE 4U
#define NET_USER_DEFAULT_TIMEOUT_MS 5000U
#define NET_USER_ICMP_MAX 1400U

typedef struct { kernel_object_t object; u16 identifier, next_sequence; } net_icmp_object_t;
typedef struct { mg_net_endpoint_t source; usize length; u8 data[NET_USER_DATAGRAM_MAX]; } net_datagram_packet_t;
typedef struct {
    kernel_object_t object;
    net_device_t *device;
    u16 local_port;
    volatile u32 head, tail, count;
    net_datagram_packet_t packets[NET_USER_DATAGRAM_QUEUE];
} net_datagram_object_t;
typedef struct { kernel_object_t object; tcp_connection_t *connection; } net_stream_object_t;

static bool icmp_object_active;
static volatile bool resolver_active;
static u16 next_icmp_identifier = 0x4d80U, next_udp_port = 49160U;
static net_datagram_object_t *datagram_objects[4];

static net_ipv4_t kernel_ip(mg_ipv4_addr_t address) { net_ipv4_t r; for (u32 i=0;i<4;i++) r.octet[i]=address.octet[i]; return r; }
static mg_ipv4_addr_t user_ip(net_ipv4_t address) { mg_ipv4_addr_t r; for (u32 i=0;i<4;i++) r.octet[i]=address.octet[i]; return r; }
static u64 timeout_ms(u32 value) { return value == ~(u32)0 ? NET_USER_DEFAULT_TIMEOUT_MS : value; }
static void wait_tick(void) { (void)scheduler_sleep(1); }

static void clear_runtime_configuration(void)
{
    net_config_clear();
    dns_reset();
}

static i64 apply_dhcp_configuration(u32 timeout, const char **reason)
{
    dhcp_lease_t lease;

    (void)timeout;
    if (reason) *reason = NULL;
    if (!net_primary_device()) {
        if (reason) *reason = "no network device to configure";
        return MG_ERR_NETWORK_UNAVAILABLE;
    }
    clear_runtime_configuration();
    if (!dhcp_acquire(net_primary_device(), &lease)) {
        if (reason) *reason = "DHCP configuration unavailable";
        return MG_ERR_TIMEOUT;
    }
    if (!net_config_apply_dhcp(&lease.address, &lease.netmask,
                               &lease.gateway, lease.has_gateway,
                               &lease.dns, lease.has_dns, &lease.server,
                               lease.lease_seconds)) {
        if (reason) *reason = "DHCP configuration invalid";
        clear_runtime_configuration();
        return MG_ERR_IO;
    }
    return MG_OK;
}

static net_datagram_object_t *datagram_find(u16 port)
{
    for (u32 i=0;i<sizeof(datagram_objects)/sizeof(datagram_objects[0]);i++)
        if (datagram_objects[i] && datagram_objects[i]->local_port == port) return datagram_objects[i];
    return 0;
}

static void datagram_rx(net_device_t *device, net_ipv4_t source, u16 source_port,
                        net_ipv4_t destination, u16 destination_port,
                        const u8 *payload, usize length)
{
    net_datagram_object_t *d = datagram_find(destination_port);
    net_datagram_packet_t *packet;
    u32 tail;
    (void)device; (void)destination;
    if (!d || !payload || length > NET_USER_DATAGRAM_MAX ||
        __atomic_load_n(&d->count, __ATOMIC_ACQUIRE) >= NET_USER_DATAGRAM_QUEUE) return;
    tail = __atomic_load_n(&d->tail, __ATOMIC_RELAXED);
    packet = &d->packets[tail];
    packet->source.address = user_ip(source);
    packet->source.port = source_port;
    packet->source.reserved = 0;
    packet->length = length;
    for (usize i=0;i<length;i++) packet->data[i]=payload[i];
    __atomic_store_n(&d->tail, (tail+1U)%NET_USER_DATAGRAM_QUEUE, __ATOMIC_RELEASE);
    __atomic_fetch_add(&d->count, 1U, __ATOMIC_RELEASE);
}

static void icmp_destroy(kernel_object_t *object) { icmp_object_active=false; kfree(object); }
static void datagram_destroy(kernel_object_t *object)
{
    net_datagram_object_t *d=(net_datagram_object_t *)object;
    (void)udp_unregister_handler(d->local_port, datagram_rx);
    for (u32 i=0;i<sizeof(datagram_objects)/sizeof(datagram_objects[0]);i++) if (datagram_objects[i]==d) datagram_objects[i]=0;
    kfree(d);
}
static void stream_destroy(kernel_object_t *object)
{
    net_stream_object_t *s=(net_stream_object_t *)object;
    /* Process teardown must not block in a FIN/TIME_WAIT exchange.  Explicit
     * close performs the graceful path; destruction makes the core reusable
     * even after failed connect/send/close paths. */
    if (s->connection) tcp_abort(s->connection);
    kfree(s);
}

kernel_object_t *net_user_icmp_create(void)
{
    net_icmp_object_t *i;
    if (icmp_object_active) return 0;
    i=(net_icmp_object_t *)kmalloc(sizeof(*i)); if (!i) return 0;
    *i=(net_icmp_object_t){0}; object_init(&i->object, OBJECT_TYPE_NETWORK_ICMP, icmp_destroy);
    i->identifier=++next_icmp_identifier; icmp_object_active=true; return &i->object;
}

kernel_object_t *net_user_datagram_create(u16 port, i64 *error_out)
{
    net_datagram_object_t *d; u32 slot;
    if (error_out) *error_out=MG_ERR_NO_MEMORY;
    if (!net_network_configured() || !net_primary_device()) { if(error_out)*error_out=MG_ERR_NETWORK_UNAVAILABLE; return 0; }
    if (!port) { do { port=next_udp_port++; if(port<49152U) port=next_udp_port=49152U; } while(datagram_find(port)); }
    if (datagram_find(port)) { if(error_out)*error_out=MG_ERR_ADDRESS_IN_USE; return 0; }
    for(slot=0;slot<sizeof(datagram_objects)/sizeof(datagram_objects[0]) && datagram_objects[slot];slot++) {}
    if(slot==sizeof(datagram_objects)/sizeof(datagram_objects[0])) { if(error_out)*error_out=MG_ERR_BUSY; return 0; }
    d=(net_datagram_object_t *)kmalloc(sizeof(*d)); if(!d) return 0;
    *d=(net_datagram_object_t){0}; d->device=net_primary_device(); d->local_port=port;
    if(!udp_register_handler(port, datagram_rx)) { kfree(d); if(error_out)*error_out=MG_ERR_BUSY; return 0; }
    object_init(&d->object, OBJECT_TYPE_NETWORK_DATAGRAM, datagram_destroy); datagram_objects[slot]=d;
    if(error_out)*error_out=MG_OK; return &d->object;
}

kernel_object_t *net_user_stream_connect(const mg_net_endpoint_t *remote, u32 timeout, i64 *error_out)
{
    net_stream_object_t *s; tcp_connection_t *connection; tcp_status_t status;
    (void)timeout;
    if(error_out)*error_out=MG_ERR_NETWORK_UNAVAILABLE;
    if(!remote || !remote->port || !net_primary_device() || !net_network_configured()) return 0;
    if(!tcp_connect(net_primary_device(), kernel_ip(remote->address), remote->port, &connection, &status)) {
        if (error_out) *error_out = status == TCP_STATUS_TIMEOUT ? MG_ERR_TIMEOUT :
            status == TCP_STATUS_BUSY ? MG_ERR_BUSY :
            status == TCP_STATUS_RESET ? MG_ERR_CONNECTION_RESET : MG_ERR_CONNECTION_CLOSED;
        return 0;
    }
    s=(net_stream_object_t *)kmalloc(sizeof(*s)); if(!s) { (void)tcp_close(connection,&status); if(error_out)*error_out=MG_ERR_NO_MEMORY; return 0; }
    *s=(net_stream_object_t){0}; s->connection=connection; object_init(&s->object,OBJECT_TYPE_NETWORK_STREAM,stream_destroy);
    if(error_out)*error_out=MG_OK; return &s->object;
}

i64 net_user_info(mg_net_info_t *info)
{
    const net_config_t *c=net_config(); if(!info)return MG_ERR_BAD_ARGUMENT;
    *info=(mg_net_info_t){0}; info->configured=c->configured; info->mode=(u8)c->mode; info->prefix_length=c->prefix_length; info->address=user_ip(c->address); info->netmask=user_ip(c->netmask); info->gateway=user_ip(c->gateway); info->dns=user_ip(c->dns); return MG_OK;
}

i64 net_user_resolve_a(const char *name, mg_ipv4_addr_t *address, u32 timeout)
{
    net_ipv4_t result; dns_status_t status; bool resolved;
    if(!name||!address||!net_primary_device())return MG_ERR_NETWORK_UNAVAILABLE;
    /* The current resolver has one wire transaction slot.  Keep that limit
     * explicit at the object boundary instead of allowing two processes to
     * overwrite the transaction ID/state. */
    if (__atomic_exchange_n(&resolver_active, true, __ATOMIC_ACQ_REL)) return MG_ERR_BUSY;
    if (timeout == MG_NET_TIMEOUT_IMMEDIATE) {
        __atomic_store_n(&resolver_active, false, __ATOMIC_RELEASE);
        return MG_ERR_WOULD_BLOCK;
    }
    resolved=dns_resolve_a(net_primary_device(),name,&result,&status);
    __atomic_store_n(&resolver_active, false, __ATOMIC_RELEASE);
    if(!resolved)return status==DNS_STATUS_TIMEOUT?MG_ERR_TIMEOUT:MG_ERR_NOT_FOUND;
    *address=user_ip(result); return MG_OK;
}

i64 net_user_icmp_echo(kernel_object_t *object,const mg_ipv4_addr_t *destination,const void *payload,usize length,u32 timeout,mg_icmp_echo_result_t *result)
{
    net_icmp_object_t *i=(net_icmp_object_t *)object; u64 start,received; net_ipv4_t source; usize reply_length;
    if(!i||object->type!=OBJECT_TYPE_NETWORK_ICMP||!destination||!result||(!payload&&length)||length>NET_USER_ICMP_MAX||!net_primary_device())return MG_ERR_BAD_ARGUMENT;
    start=timer_uptime_ms(); i->next_sequence++;
    if(!icmp_echo_request(net_primary_device(),kernel_ip(*destination),i->identifier,i->next_sequence,payload,length))return MG_ERR_NETWORK_UNAVAILABLE;
    while(timer_uptime_ms()-start<timeout_ms(timeout)) { if(icmp_echo_reply_info(i->identifier,i->next_sequence,&source,&reply_length,&received)) { result->source=user_ip(source); result->sequence=i->next_sequence; result->reply_length=(u16)reply_length; result->rtt_ms=received-start; return MG_OK; } wait_tick(); }
    return MG_ERR_TIMEOUT;
}

i64 net_user_datagram_send(kernel_object_t *object,const mg_net_endpoint_t *destination,const void *data,usize length,u32 timeout)
{
    net_datagram_object_t *d=(net_datagram_object_t *)object; const net_config_t *c=net_config(); (void)timeout;
    if(!d||object->type!=OBJECT_TYPE_NETWORK_DATAGRAM||!destination||!destination->port||(!data&&length)||length>NET_USER_DATAGRAM_MAX)return MG_ERR_BAD_ARGUMENT;
    return udp_transmit(d->device,c->address,kernel_ip(destination->address),d->local_port,destination->port,data,length)?(i64)length:MG_ERR_NETWORK_UNAVAILABLE;
}

i64 net_user_datagram_receive(kernel_object_t *object,void *data,usize capacity,u32 timeout,mg_datagram_result_t *result)
{
    net_datagram_object_t *d=(net_datagram_object_t *)object; u64 start=timer_uptime_ms(); u32 head; net_datagram_packet_t *p;
    if(!d||object->type!=OBJECT_TYPE_NETWORK_DATAGRAM||!data||!result)return MG_ERR_BAD_ARGUMENT;
    while(__atomic_load_n(&d->count,__ATOMIC_ACQUIRE)==0) { if(timeout==0||timer_uptime_ms()-start>=timeout_ms(timeout))return MG_ERR_TIMEOUT; wait_tick(); }
    head=__atomic_load_n(&d->head,__ATOMIC_RELAXED); p=&d->packets[head]; if(capacity<p->length)return MG_ERR_BUFFER_TOO_SMALL;
    for(usize i=0;i<p->length;i++)((u8 *)data)[i]=p->data[i]; result->source=p->source; result->length=p->length;
    __atomic_store_n(&d->head,(head+1U)%NET_USER_DATAGRAM_QUEUE,__ATOMIC_RELEASE); __atomic_fetch_sub(&d->count,1U,__ATOMIC_RELEASE); return (i64)p->length;
}

i64 net_user_stream_send(kernel_object_t *object,const void *data,usize length,u32 timeout)
{
    net_stream_object_t *s=(net_stream_object_t *)object; tcp_status_t status; (void)timeout;
    if(!s||object->type!=OBJECT_TYPE_NETWORK_STREAM||(!data&&length))return MG_ERR_BAD_ARGUMENT;
    if(!tcp_send(s->connection,data,length,&status))return status==TCP_STATUS_TIMEOUT?MG_ERR_TIMEOUT:MG_ERR_CONNECTION_CLOSED; return (i64)length;
}

i64 net_user_stream_receive(kernel_object_t *object,void *data,usize capacity,u32 timeout)
{
    net_stream_object_t *s=(net_stream_object_t *)object; u64 start=timer_uptime_ms(); usize received;
    if(!s||object->type!=OBJECT_TYPE_NETWORK_STREAM||!data||!capacity)return MG_ERR_BAD_ARGUMENT;
    while((received=tcp_receive_bytes(s->connection,data,capacity))==0) { tcp_state_t state=tcp_connection_state(s->connection); if(state==TCP_STATE_CLOSE_WAIT||state==TCP_STATE_CLOSED)return MG_ERR_CONNECTION_CLOSED; if(timeout==0||timer_uptime_ms()-start>=timeout_ms(timeout))return MG_ERR_TIMEOUT; wait_tick(); } return (i64)received;
}

i64 net_user_stream_close(kernel_object_t *object)
{
    net_stream_object_t *s=(net_stream_object_t *)object; tcp_status_t status;
    if(!s||object->type!=OBJECT_TYPE_NETWORK_STREAM)return MG_ERR_BAD_ARGUMENT;
    if(!s->connection)return MG_OK;
    /* Closing a handle is definitive even if a peer never completes the
     * handshake.  Release the singleton before the handle is destroyed so a
     * failed close cannot turn the one-stream limit into a permanent leak. */
    if (!tcp_close(s->connection,&status)) tcp_abort(s->connection);
    s->connection=0;
    return MG_OK;
}

i64 net_user_interfaces(mg_net_interface_info_t *entries, usize capacity)
{
    usize count = 0;
    if (!net_fill_interface(entries, capacity, &count)) return MG_ERR_BAD_ARGUMENT;
    return (i64)count;
}

i64 net_user_routes(mg_net_route_info_t *entries, usize capacity)
{
    usize count = 0;
    if (!net_fill_routes(entries, capacity, &count)) return MG_ERR_BAD_ARGUMENT;
    return (i64)count;
}

i64 net_user_neighbors(mg_net_neighbor_info_t *entries, usize capacity)
{
    arp_snapshot_entry_t snapshot[ARP_CACHE_CAPACITY];
    usize count = arp_snapshot(snapshot, ARP_CACHE_CAPACITY);
    if (entries && capacity >= sizeof(*entries)) {
        usize copy = count < capacity / sizeof(*entries) ? count : capacity / sizeof(*entries);
        for (usize i = 0; i < copy; i++) {
            entries[i] = (mg_net_neighbor_info_t){0};
            for (u32 j = 0; j < 4; j++) entries[i].address.octet[j] = snapshot[i].address.octet[j];
            for (u32 j = 0; j < 6; j++) entries[i].mac[j] = snapshot[i].mac[j];
            strncpy(entries[i].interface_name, net_primary_device()->name, MG_NET_NAME_MAX - 1);
            entries[i].state = snapshot[i].valid ? 1 : 0;
        }
    }
    return (i64)count;
}

i64 net_user_connections(mg_net_connection_info_t *entries, usize capacity)
{
    (void)entries; (void)capacity;
    /* Connection objects are currently intentionally short-lived and are
     * removed at close; no stale endpoint table is exposed as active state. */
    return 0;
}

i64 net_user_renew(u32 timeout)
{
    if (net_config()->mode != NET_CONFIG_MODE_DHCP)
        return MG_ERR_BAD_ARGUMENT;
    return net_user_set_automatic(timeout);
}

i64 net_user_set_manual(const mg_net_manual_config_t *configuration)
{
    net_ipv4_t address;
    net_ipv4_t gateway;
    net_ipv4_t dns;

    if (!configuration || !net_primary_device())
        return MG_ERR_NETWORK_UNAVAILABLE;
    address = kernel_ip(configuration->address);
    gateway = kernel_ip(configuration->gateway);
    dns = kernel_ip(configuration->dns);
    clear_runtime_configuration();
    if (!net_config_apply_manual(&address, configuration->prefix_length,
                                 &gateway, &dns)) {
        clear_runtime_configuration();
        return MG_ERR_BAD_ARGUMENT;
    }
    return MG_OK;
}

i64 net_user_set_automatic(u32 timeout)
{
    return apply_dhcp_configuration(timeout, NULL);
}

i64 net_user_reload(void)
{
    net_persistent_config_t persistent;
    const char *reason = NULL;

    if (!net_config_load_persistent(&persistent, &reason))
        return MG_ERR_BAD_ARGUMENT;
    if (persistent.mode == NET_CONFIG_MODE_MANUAL) {
        mg_net_manual_config_t manual = {0};
        manual.address = user_ip(persistent.address);
        manual.prefix_length = persistent.prefix_length;
        manual.gateway = user_ip(persistent.gateway);
        manual.dns = user_ip(persistent.dns);
        return net_user_set_manual(&manual);
    }
    if (persistent.mode == NET_CONFIG_MODE_DHCP)
        return net_user_set_automatic(MG_NET_TIMEOUT_DEFAULT);
    return MG_ERR_BAD_ARGUMENT;
}

i64 net_user_apply_boot_config(const char **reason)
{
    net_persistent_config_t persistent;
    const char *load_reason = NULL;
    i64 result;

    if (reason) *reason = NULL;
    if (!net_config_load_persistent(&persistent, &load_reason)) {
        if (reason) *reason = load_reason ? load_reason :
            "network configuration could not be read";
        return MG_ERR_BAD_ARGUMENT;
    }
    if (persistent.mode == NET_CONFIG_MODE_MANUAL) {
        mg_net_manual_config_t manual = {0};
        manual.address = user_ip(persistent.address);
        manual.prefix_length = persistent.prefix_length;
        manual.gateway = user_ip(persistent.gateway);
        manual.dns = user_ip(persistent.dns);
        result = net_user_set_manual(&manual);
        if (result < 0 && reason)
            *reason = result == MG_ERR_NETWORK_UNAVAILABLE ?
                "no network device to configure" :
                "invalid manual network configuration";
        return result;
    }
    if (persistent.mode != NET_CONFIG_MODE_DHCP) {
        if (reason) *reason = "invalid network configuration mode";
        return MG_ERR_BAD_ARGUMENT;
    }
    result = apply_dhcp_configuration(MG_NET_TIMEOUT_DEFAULT, reason);
    return result;
}
