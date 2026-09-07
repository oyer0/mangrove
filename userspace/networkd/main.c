/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/ipc.h>
#include <mg/log_service.h>
#include <mg/net.h>
#include <mg/network_service.h>
#include <mangrove.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_CONFIG_PATH "/conf/network/config"
#define NETWORK_CONFIG_MAX_BYTES 1024U

static bool requester_is_sprout(const mg_ipc_received_t *received)
{
    return received && received->requester.system_service &&
           received->requester.service_id == MG_SERVICE_SPROUT;
}

static bool interface_name_valid(const char *name)
{
    usize length;
    usize prefix_length;

    if (!name || !name[0]) return false;
    if (!strncmp(name, "eth", 3)) prefix_length = 3;
    else if (!strncmp(name, "wifi", 4)) prefix_length = 4;
    else return false;
    length = strlen(name);
    if (length <= prefix_length || length >= MG_NET_NAME_MAX) return false;
    for (usize index = prefix_length; index < length; index++)
        if (name[index] < '0' || name[index] > '9') return false;
    return true;
}

static mg_result_t log_network_result(const mg_network_request_t *request,
                                      mg_result_t result)
{
    char message[MG_LOG_MESSAGE_MAX];
    const char *name = request && request->interface_name[0]
        ? request->interface_name : "network";
    const char *action = "network operation";

    if (!request) return result;
    switch (request->operation) {
        case MG_NETWORK_OP_SET_AUTOMATIC:
            action = result == MG_OK ? "DHCP lease acquired" :
                "DHCP acquisition failed";
            break;
        case MG_NETWORK_OP_SET_MANUAL:
            action = "network configuration applied (manual)";
            break;
        case MG_NETWORK_OP_ENABLE:
            action = "network interface enabled";
            break;
        case MG_NETWORK_OP_DISABLE:
            action = "network interface disabled";
            break;
        case MG_NETWORK_OP_RENEW:
            action = result == MG_OK ? "DHCP lease renewed" :
                "DHCP renewal failed";
            break;
        case MG_NETWORK_OP_RELOAD:
            action = "network configuration reloaded";
            break;
        default:
            break;
    }
    if (result == MG_OK)
        snprintf(message, sizeof(message), "%s for %s", action, name);
    else
        snprintf(message, sizeof(message), "%s for %s: %s", action, name,
                 error_string(result));
    (void)mg_log_submit(result == MG_OK ? MG_LOG_INFO : MG_LOG_ERROR,
                        message);
    return result;
}

static void log_network_event(const mg_event_t *event, mg_result_t result)
{
    char message[MG_LOG_MESSAGE_MAX];
    const char *name;
    const char *action;

    if (!event) return;
    name = event->name[0] ? event->name : "interface";
    switch (event->type) {
        case MG_EVENT_NETWORK_LINK_UP: action = "link up"; break;
        case MG_EVENT_NETWORK_LINK_DOWN: action = "link down"; break;
        case MG_EVENT_NETWORK_INTERFACE_ADDED: action = "interface added"; break;
        case MG_EVENT_NETWORK_INTERFACE_REMOVED: action = "interface removed"; break;
        case MG_EVENT_QUEUE_OVERFLOW:
            action = "network snapshot rebuilt after event overflow";
            break;
        default: return;
    }
    if (result == MG_OK)
        snprintf(message, sizeof(message), "%s %s", name, action);
    else
        snprintf(message, sizeof(message), "%s %s: %s", name, action,
                 error_string(result));
    (void)mg_log_submit(result == MG_OK ? MG_LOG_INFO : MG_LOG_ERROR,
                        message);
}

static void log_dhcp_event(u8 severity, const char *message)
{
    if (message) (void)mg_log_submit(severity, message);
}

static bool network_link_available(void)
{
    mg_net_interface_info_t interfaces[MG_NETWORK_MAX_INTERFACES];
    mg_net_info_t info;
    mg_result_t count = mg_net_interfaces(interfaces, sizeof(interfaces));

    if (count <= 0 || count > MG_NETWORK_MAX_INTERFACES) return false;
    if (mg_net_info(&info) != MG_OK)
        memset(&info, 0, sizeof(info));
    for (mg_result_t index = 0; index < count; index++)
        if (interfaces[index].enabled && interfaces[index].link_up &&
            (!info.interface_name[0] ||
             !strcmp(info.interface_name, interfaces[index].name)))
            return true;
    return false;
}

static bool event_is_configured_interface(const mg_event_t *event)
{
    mg_net_info_t info;

    if (!event || mg_net_info(&info) != MG_OK || !info.configured ||
        !info.interface_name[0]) return false;
    return strcmp(event->name, info.interface_name) == 0;
}

static bool network_has_configured_interface(void)
{
    mg_net_info_t info;

    return mg_net_info(&info) == MG_OK && info.configured &&
           info.interface_name[0];
}

static mg_result_t restart_dhcp_if_link_up(void)
{
    if (!network_link_available()) return MG_ERR_NETWORK_UNAVAILABLE;
    return mg_net_set_automatic();
}

static u32 network_wait_timeout(void)
{
    mg_net_info_t info;
    u64 timeout;
    bool link_up;

    if (mg_net_info(&info) != MG_OK || !info.configured ||
        info.mode != MG_NET_MODE_DHCP ||
        (info.dhcp_state != MG_NET_DHCP_STATE_BOUND &&
         info.dhcp_state != MG_NET_DHCP_STATE_RENEWING &&
         info.dhcp_state != MG_NET_DHCP_STATE_REBINDING))
        return MG_NET_TIMEOUT_DEFAULT;
    /* A link-down interface cannot make progress through DHCP.  Leave the
     * lease in place while it remains valid, but retain an expiry wakeup so
     * an expired address is not left configured indefinitely. */
    link_up = network_link_available();
    if (!link_up) timeout = info.lease_remaining_ms;
    else timeout = info.dhcp_next_action_ms;
    if (!timeout) return 1U;
    if (timeout >= (u64)MG_NET_TIMEOUT_DEFAULT)
        return MG_NET_TIMEOUT_DEFAULT - 1U;
    return (u32)timeout;
}

static void handle_dhcp_deadline(void)
{
    mg_net_info_t info;
    mg_result_t result;
    bool rebinding;

    if (mg_net_info(&info) != MG_OK || !info.configured ||
        info.mode != MG_NET_MODE_DHCP || info.dhcp_next_action_ms != 0)
        return;
    if (info.lease_remaining_ms == 0) {
        log_dhcp_event(MG_LOG_WARNING, "DHCP lease expired");
        result = mg_net_clear_runtime();
        if (result != MG_OK) {
            log_dhcp_event(MG_LOG_ERROR, "DHCP lease cleanup failed");
        } else if (network_link_available()) {
            result = restart_dhcp_if_link_up();
            log_dhcp_event(result == MG_OK ? MG_LOG_INFO : MG_LOG_ERROR,
                           "DHCP acquisition restarted");
        }
        return;
    }
    if (!network_link_available()) return;
    rebinding = info.dhcp_state == MG_NET_DHCP_STATE_REBINDING ||
        (info.dhcp_state == MG_NET_DHCP_STATE_RENEWING &&
         info.rebind_in_ms == 0);
    if (info.dhcp_state == MG_NET_DHCP_STATE_BOUND)
        log_dhcp_event(MG_LOG_INFO, "DHCP renewal started");
    else if (rebinding && info.dhcp_state != MG_NET_DHCP_STATE_REBINDING)
        log_dhcp_event(MG_LOG_WARNING, "DHCP rebinding started");
    result = mg_net_dhcp_renew(rebinding);
    if (result == MG_OK) {
        log_dhcp_event(MG_LOG_INFO, "DHCP lease renewed");
        return;
    }
    /* A NAK clears the old lease in the kernel.  Begin a fresh acquisition;
     * ordinary timeouts retain the address and use the bounded retry timer. */
    if (mg_net_info(&info) == MG_OK && info.mode != MG_NET_MODE_DHCP) {
        result = restart_dhcp_if_link_up();
        log_dhcp_event(result == MG_OK ? MG_LOG_INFO : MG_LOG_ERROR,
                       "DHCP acquisition restarted");
    }
}

static mg_result_t find_interface(const char *name,
                                  mg_net_interface_info_t *output)
{
    mg_net_interface_info_t interfaces[MG_NETWORK_MAX_INTERFACES];
    mg_result_t count;

    if (!interface_name_valid(name)) return MG_ERR_BAD_ARGUMENT;
    count = mg_net_interfaces(interfaces, sizeof(interfaces));
    if (count < 0) return count;
    for (mg_result_t index = 0; index < count &&
         index < MG_NETWORK_MAX_INTERFACES; index++) {
        if (!strcmp(interfaces[index].name, name)) {
            if (output) *output = interfaces[index];
            return MG_OK;
        }
    }
    return MG_ERR_NOT_FOUND;
}

static mg_result_t read_network_config(char *buffer, usize capacity,
                                       usize *length)
{
    mg_path_info_t info;
    mg_result_t result;
    mg_handle_t file;
    usize offset = 0;

    if (!buffer || !length || capacity == 0) return MG_ERR_BAD_ARGUMENT;
    result = path_info(NETWORK_CONFIG_PATH, &info);
    if (result != MG_OK) return result;
    if (info.type != MG_PATH_TYPE_FILE || info.size >= capacity)
        return MG_ERR_BAD_ARGUMENT;
    result = file_open(NETWORK_CONFIG_PATH, MG_OPEN_READ);
    if (result < 0) return result;
    file = (mg_handle_t)result;
    while (offset < (usize)info.size) {
        result = object_read(file, buffer + offset,
                             (usize)info.size - offset);
        if (result < 0 || result == 0) {
            (void)handle_close(file);
            return result < 0 ? result : MG_ERR_IO;
        }
        offset += (usize)result;
    }
    buffer[offset] = '\0';
    *length = offset;
    (void)handle_close(file);
    return MG_OK;
}

static mg_result_t write_network_config_text(const char *text, usize length)
{
    mg_handle_t file;
    mg_result_t result;

    if (!text || length >= NETWORK_CONFIG_MAX_BYTES)
        return MG_ERR_BAD_ARGUMENT;
    result = file_open(NETWORK_CONFIG_PATH, MG_OPEN_WRITE);
    if (result < 0) return result;
    file = (mg_handle_t)result;
    result = file_truncate(file);
    if (result == MG_OK)
        result = object_write_all(file, text, length) == (mg_result_t)length
            ? MG_OK : MG_ERR_IO;
    (void)handle_close(file);
    return result;
}

static mg_result_t build_network_config(const mg_network_request_t *request,
                                        char *text, usize capacity,
                                        usize *length)
{
    char address[16];
    char gateway[16];
    char dns[16];
    int written;

    if (!request || !text || !length || capacity == 0 ||
        !interface_name_valid(request->interface_name))
        return MG_ERR_BAD_ARGUMENT;
    if (request->operation == MG_NETWORK_OP_SET_AUTOMATIC) {
        written = snprintf(text, capacity, "interface=%s\nmode=dhcp\n",
                           request->interface_name);
    } else {
        if (!mg_ipv4_format(&request->manual.address, address,
                            sizeof(address)) ||
            !mg_ipv4_format(&request->manual.gateway, gateway,
                            sizeof(gateway)) ||
            !mg_ipv4_format(&request->manual.dns, dns, sizeof(dns)) ||
            request->manual.prefix_length > 32U) return MG_ERR_BAD_ARGUMENT;
        written = snprintf(text, capacity,
                           "interface=%s\nmode=manual\naddress=%s/%u\n"
                           "gateway=%s\ndns=%s\n", request->interface_name,
                           address, (u32)request->manual.prefix_length, gateway,
                           dns);
    }
    if (written < 0 || (usize)written >= capacity) return MG_ERR_BAD_ARGUMENT;
    *length = (usize)written;
    return MG_OK;
}

static mg_result_t send_status_all(mg_network_response_t *response)
{
    mg_network_status_t status;
    mg_result_t count;

    memset(&status, 0, sizeof(status));
    if (mg_net_info(&status.info) < 0)
        return MG_ERR_NETWORK_UNAVAILABLE;
    count = mg_net_interfaces(status.interfaces, sizeof(status.interfaces));
    if (count < 0) return count;
    if ((usize)count > MG_NETWORK_MAX_INTERFACES)
        count = MG_NETWORK_MAX_INTERFACES;
    response->count = (u32)count;
    memcpy(response->data, &status, sizeof(status));
    return MG_OK;
}

static mg_result_t send_snapshot(u16 operation,
                                 mg_network_response_t *response)
{
    mg_result_t count;
    usize capacity;
    void *output = response->data;

    switch (operation) {
        case MG_NETWORK_OP_INTERFACES:
            capacity = MG_NETWORK_MAX_INTERFACES *
                       sizeof(mg_net_interface_info_t);
            count = mg_net_interfaces(output, capacity);
            break;
        case MG_NETWORK_OP_ROUTES:
            capacity = 4U * sizeof(mg_net_route_info_t);
            count = mg_net_routes(output, capacity);
            break;
        case MG_NETWORK_OP_NEIGHBORS:
            capacity = 16U * sizeof(mg_net_neighbor_info_t);
            count = mg_net_neighbors(output, capacity);
            break;
        case MG_NETWORK_OP_CONNECTIONS:
            capacity = 8U * sizeof(mg_net_connection_info_t);
            count = mg_net_connections(output, capacity);
            break;
        default:
            return MG_ERR_BAD_ARGUMENT;
    }
    if (count < 0) return count;
    response->count = (u32)count;
    return MG_OK;
}

static mg_result_t status_interface(const char *name,
                                    mg_network_response_t *response)
{
    mg_net_interface_info_t interface;
    mg_network_status_t status;
    mg_result_t result = find_interface(name, &interface);

    if (result != MG_OK) return result;
    memset(&status, 0, sizeof(status));
    result = mg_net_info(&status.info);
    if (result != MG_OK) return result;
    response->count = 1;
    status.interfaces[0] = interface;
    memcpy(response->data, &status, sizeof(status));
    return MG_OK;
}

static mg_result_t apply_request(const mg_ipc_received_t *received,
                                 const mg_network_request_t *request)
{
    char old_config[NETWORK_CONFIG_MAX_BYTES];
    char new_config[NETWORK_CONFIG_MAX_BYTES];
    usize old_length = 0;
    usize new_length = 0;
    mg_result_t result;
    mg_net_info_t info;

    if (!request)
        return MG_ERR_BAD_ARGUMENT;
    if (request->operation == MG_NETWORK_OP_RENEW) {
        result = mg_net_info(&info);
        if (result != MG_OK) return result;
        if (info.mode != MG_NET_MODE_DHCP) return MG_ERR_BAD_ARGUMENT;
    } else {
        if (!interface_name_valid(request->interface_name))
            return MG_ERR_BAD_ARGUMENT;
        result = find_interface(request->interface_name, NULL);
        if (result != MG_OK) return result;
    }

    if (request->operation == MG_NETWORK_OP_SET_AUTOMATIC ||
        request->operation == MG_NETWORK_OP_SET_MANUAL) {
        result = build_network_config(request, new_config,
                                       sizeof(new_config), &new_length);
        if (result != MG_OK) return result;
        result = read_network_config(old_config, sizeof(old_config),
                                     &old_length);
        if (result != MG_OK) return result;
    }

    result = pass_authorize_network_request(received->request,
                                            request->operation);
    if (result != MG_OK) return result;

    switch (request->operation) {
        case MG_NETWORK_OP_SET_AUTOMATIC:
            result = write_network_config_text(new_config, new_length);
            if (result == MG_OK) result = mg_net_set_automatic();
            if (result != MG_OK) {
                (void)write_network_config_text(old_config, old_length);
                (void)mg_net_reload();
            }
            return log_network_result(request, result);
        case MG_NETWORK_OP_SET_MANUAL:
            result = write_network_config_text(new_config, new_length);
            if (result == MG_OK) result = mg_net_set_manual(&request->manual);
            if (result != MG_OK) {
                (void)write_network_config_text(old_config, old_length);
                (void)mg_net_reload();
            }
            return log_network_result(request, result);
        case MG_NETWORK_OP_ENABLE:
            result = mg_net_set_enabled(true);
            if (result == MG_OK) result = mg_net_reload();
            return log_network_result(request, result);
        case MG_NETWORK_OP_DISABLE:
            result = mg_net_clear_runtime();
            if (result == MG_OK) result = mg_net_set_enabled(false);
            return log_network_result(request, result);
        case MG_NETWORK_OP_RENEW:
            result = mg_net_dhcp_renew(false);
            return log_network_result(request, result);
        default:
            return MG_ERR_BAD_ARGUMENT;
    }
}

static mg_result_t reload_request(const mg_ipc_received_t *received)
{
    mg_result_t result;

    if (!received) return MG_ERR_BAD_ARGUMENT;
    if (!requester_is_sprout(received))
        result = pass_authorize_network_request(received->request,
                                                MG_NETWORK_OP_RELOAD);
    else
        result = mg_net_reload();
    {
        mg_network_request_t request = {0};
        request.operation = MG_NETWORK_OP_RELOAD;
        result = log_network_result(&request, result);
    }
    return result;
}

static bool send_response(const mg_ipc_received_t *received,
                          const mg_network_response_t *response)
{
    mg_ipc_message_t message = {0};

    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_NETWORK_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static bool handle_request(const mg_ipc_received_t *received)
{
    mg_network_request_t request;
    mg_network_response_t response;

    memset(&response, 0, sizeof(response));
    if (!received || received->message.type != MG_NETWORK_REQUEST ||
        received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_response(received, &response);
        return true;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    if (request.version != MG_NETWORK_PROTOCOL_VERSION || request.flags != 0U)
        response.result = MG_ERR_PROTOCOL;
    else {
        switch (request.operation) {
            case MG_NETWORK_OP_STATUS:
                response.result = send_status_all(&response);
                break;
            case MG_NETWORK_OP_STATUS_INTERFACE:
                response.result = status_interface(request.interface_name,
                                                   &response);
                break;
            case MG_NETWORK_OP_INTERFACES:
            case MG_NETWORK_OP_ROUTES:
            case MG_NETWORK_OP_NEIGHBORS:
            case MG_NETWORK_OP_CONNECTIONS:
                response.result = send_snapshot(request.operation, &response);
                break;
            case MG_NETWORK_OP_RELOAD:
                response.result = reload_request(received);
                break;
            case MG_NETWORK_OP_SET_AUTOMATIC:
            case MG_NETWORK_OP_SET_MANUAL:
            case MG_NETWORK_OP_ENABLE:
            case MG_NETWORK_OP_DISABLE:
            case MG_NETWORK_OP_RENEW:
                response.result = apply_request(received, &request);
                break;
            default:
                response.result = MG_ERR_UNSUPPORTED;
                break;
        }
    }
    (void)send_response(received, &response);
    return true;
}

static void handle_event(const mg_ipc_received_t *received)
{
    mg_event_t event;
    mg_result_t result = MG_OK;

    if (!received || (received->delivery_kind != MG_IPC_DELIVERY_EVENT &&
                      received->delivery_kind !=
                          MG_IPC_DELIVERY_EVENT_OVERFLOW))
        return;
    if (received->message.payload_length != sizeof(event)) return;
    memcpy(&event, received->message.payload, sizeof(event));
    if (event.version != MG_EVENT_PROTOCOL_VERSION) return;
    /* Link events are autonomous maintenance.  The existing network policy
     * path remains the authority and does not ask a human to reauthorize it. */
    if (event.type == MG_EVENT_NETWORK_LINK_DOWN) {
        mg_net_info_t info;
        if (!event_is_configured_interface(&event)) {
            result = MG_OK;
        } else if (mg_net_info(&info) == MG_OK &&
                   info.mode == MG_NET_MODE_DHCP)
            result = MG_OK;
        else
            result = mg_net_clear_runtime();
    } else if (event.type == MG_EVENT_NETWORK_LINK_UP) {
        mg_net_info_t info;
        if (!network_has_configured_interface()) {
            result = network_link_available() ? mg_net_reload() : MG_OK;
        } else if (!event_is_configured_interface(&event)) {
            result = MG_OK;
        } else if (mg_net_info(&info) == MG_OK &&
                   info.mode == MG_NET_MODE_DHCP &&
            info.configured) {
            if (info.lease_remaining_ms == 0) {
                result = mg_net_clear_runtime();
                if (result == MG_OK) result = restart_dhcp_if_link_up();
            } else if (info.dhcp_next_action_ms == 0) {
                result = mg_net_dhcp_renew(
                    info.dhcp_state == MG_NET_DHCP_STATE_REBINDING ||
                    (info.dhcp_state == MG_NET_DHCP_STATE_RENEWING &&
                     info.rebind_in_ms == 0));
            }
        } else {
            result = mg_net_reload();
        }
    } else if (event.type == MG_EVENT_NETWORK_INTERFACE_REMOVED) {
        /* The kernel clears runtime state before publishing removal of the
         * primary interface.  Reconcile from persistent policy when the
         * removal leaves another usable interface, rather than treating the
         * cleared state as an intentional disable. */
        if (event_is_configured_interface(&event))
            result = mg_net_clear_runtime();
        else if (!network_has_configured_interface())
            result = network_link_available() ? mg_net_reload() :
                mg_net_clear_runtime();
        else
            result = MG_OK;
    } else if (event.type == MG_EVENT_NETWORK_INTERFACE_ADDED ||
               event.type == MG_EVENT_QUEUE_OVERFLOW) {
        /* An added interface is also the normal trigger for first
         * configuration.  If policy names another interface, reload will
         * return a bounded NOT_FOUND instead of configuring the wrong NIC. */
        if (event.type == MG_EVENT_QUEUE_OVERFLOW ||
            (!network_has_configured_interface() && network_link_available()))
            result = mg_net_reload();
        else
            result = MG_OK;
    }
    log_network_event(&event, result);
}

int main(void)
{
    mg_handle_t endpoint = 0;
    mg_ipc_received_t received;
    mg_result_t result;

    result = service_register("network", &endpoint);
    if (result < 0) {
        printf("Networkd: endpoint registration failed: %s\n",
               error_string(result));
        process_exit(1);
    }

    result = ipc_event_subscribe(endpoint, MG_EVENT_CLASS_NETWORK);
    if (result != MG_OK) {
        (void)handle_close(endpoint);
        process_exit(1);
    }
    /* No NIC is a normal state.  Re-enable the administrative device on a
     * daemon restart, then let the common kernel config path apply DHCP or
     * manual policy when a device is present. */
    (void)mg_net_set_enabled(true);
    result = mg_net_reload();
    if (result != MG_OK && result != MG_ERR_NETWORK_UNAVAILABLE)
        printf("Networkd: configuration not applied: %s\n",
               error_string(result));
    if (result == MG_OK) {
        mg_network_request_t request = {0};
        request.operation = MG_NETWORK_OP_RELOAD;
        (void)log_network_result(&request, result);
    }

    for (;;) {
        u32 timeout = network_wait_timeout();
        if (timeout == MG_NET_TIMEOUT_DEFAULT)
            result = ipc_receive(endpoint, &received);
        else
            result = ipc_receive_timed(endpoint, &received, timeout);
        if (result == MG_ERR_TIMEOUT) {
            handle_dhcp_deadline();
            continue;
        }
        if (result != MG_OK) {
            (void)handle_close(endpoint);
            process_exit(1);
        }
        if (received.delivery_kind == MG_IPC_DELIVERY_EVENT ||
            received.delivery_kind == MG_IPC_DELIVERY_EVENT_OVERFLOW)
            handle_event(&received);
        else {
            (void)handle_request(&received);
            (void)handle_close(received.request);
        }
    }
}
