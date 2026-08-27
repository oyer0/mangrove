#pragma once

#include <object.h>
#include <mg/net.h>

kernel_object_t *net_user_icmp_create(void);
kernel_object_t *net_user_datagram_create(u16 local_port, i64 *error_out);
kernel_object_t *net_user_stream_connect(const mg_net_endpoint_t *remote,
                                         u32 timeout_ms, i64 *error_out);
i64 net_user_info(mg_net_info_t *info);
i64 net_user_resolve_a(const char *hostname, mg_ipv4_addr_t *address,
                       u32 timeout_ms);
i64 net_user_icmp_echo(kernel_object_t *object, const mg_ipv4_addr_t *destination,
                        const void *payload, usize length, u32 timeout_ms,
                        mg_icmp_echo_result_t *result);
i64 net_user_datagram_send(kernel_object_t *object, const mg_net_endpoint_t *destination,
                           const void *data, usize length, u32 timeout_ms);
i64 net_user_datagram_receive(kernel_object_t *object, void *data, usize capacity,
                              u32 timeout_ms, mg_datagram_result_t *result);
i64 net_user_stream_send(kernel_object_t *object, const void *data, usize length,
                         u32 timeout_ms);
i64 net_user_stream_receive(kernel_object_t *object, void *data, usize capacity,
                            u32 timeout_ms);
i64 net_user_stream_close(kernel_object_t *object);
i64 net_user_interfaces(mg_net_interface_info_t *entries, usize capacity);
i64 net_user_routes(mg_net_route_info_t *entries, usize capacity);
i64 net_user_neighbors(mg_net_neighbor_info_t *entries, usize capacity);
i64 net_user_connections(mg_net_connection_info_t *entries, usize capacity);
i64 net_user_renew(u32 timeout_ms);
i64 net_user_set_manual(const mg_net_manual_config_t *configuration);
i64 net_user_set_automatic(u32 timeout_ms);
i64 net_user_reload(void);
i64 net_user_apply_boot_config(const char **reason);
