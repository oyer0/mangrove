/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <syscall.h>
#include <msr.h>
#include <cpu.h>
#include <scheduler.h>
#include <process.h>
#include <heap.h>
#include <pmm.h>
#include <vmm.h>
#include <terminal.h>
#include <mangrove_errors.h>
#include <mg/filesystem.h>
#include <mg/net.h>
#include <mg/network_service.h>
#include <mg/power.h>
#include <mg/service.h>
#include <mg/session_service.h>
#include <mg/process_limits.h>
#include <mg/device_service.h>
#include <mg/volume_service.h>
#include <mg/storage.h>
#include <mg/terminal.h>
#include <mg/time.h>
#include <mg/identity.h>
#include <net/user.h>
#include <string.h>
#include <kprint.h>
#include <timer.h>
#include <timekeeping.h>
#include <csprng.h>
#include <random_syscall.h>
#include <platform_power.h>
#include <identity.h>
#include <pass.h>
#include <session.h>
#include <ipc.h>
#include <service.h>
#include <device.h>
#include <inspection.h>
#include <storage/gpt.h>
#include <storage/format.h>
#include <panic.h>
#include <stdint.h>
#ifdef HEAP_PROFILE_DIAGNOSTICS
#include <heap_profile_diag.h>
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#define MSR_EFER  0xC0000080U
#define MSR_STAR  0xC0000081U
#define MSR_LSTAR 0xC0000082U
#define MSR_FMASK 0xC0000084U
#define EFER_SCE  (1ULL << 0)
#define FILE_REPLACE_CHUNK_BYTES 4096U
typedef struct syscall_frame {
    u64 r15, r14, r13, r12, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rbx, rax;
    u64 rcx, r11;
} syscall_frame_t;

typedef struct syscall_user_lease_entry {
    bool writable;
    vmm_user_range_lease_t lease;
} syscall_user_lease_entry_t;

typedef struct syscall_user_access_context {
    syscall_user_lease_entry_t *leases;
    usize count;
    usize capacity;
    bool allocation_failed;
} syscall_user_access_context_t;

extern void syscall_entry(void);
static void syscall_fail(syscall_frame_t *frame, i64 error);
static void syscall_dispatch_inner(syscall_frame_t *frame);

void syscall_init_cpu(void)
{
    u64 efer = rdmsr(MSR_EFER);

    /* STAR selects kernel CS=0x08 and SYSRET user CS=0x3B/data SS=0x33. */
    wrmsr(MSR_STAR, (0x2BULL << 48) | (0x08ULL << 32));
    wrmsr(MSR_LSTAR, (u64)(uintptr_t)&syscall_entry);
    /* Mask IF, TF and DF while executing the entry path. */
    wrmsr(MSR_FMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10));
    wrmsr(MSR_EFER, efer | EFER_SCE);
}

static bool syscall_user_buffer_accessible(const void *buffer, u64 length,
                                           bool writable)
{
    kernel_thread_t *thread = thread_current();
    syscall_user_access_context_t *context;
    uintptr_t start = (uintptr_t)buffer;
    uintptr_t end;
    uintptr_t first_page;
    uintptr_t last_page;
    if (length == 0) return true;
    if ((u64)(usize)length != length || !thread || !thread->process ||
        !thread->syscall_user_access_context) return false;
    if (!buffer || length > ~(uintptr_t)0 - start ||
        start >= VMM_USER_ADDRESS_LIMIT ||
        length > VMM_USER_ADDRESS_LIMIT - start) return false;
    context = (syscall_user_access_context_t *)
        thread->syscall_user_access_context;
    end = start + (usize)length;
    first_page = start & ~(uintptr_t)(VMM_PAGE_SIZE - 1U);
    last_page = (end - 1U) & ~(uintptr_t)(VMM_PAGE_SIZE - 1U);
    for (usize index = 0; index < context->count; index++) {
        const vmm_user_range_lease_t *lease =
            &context->leases[index].lease;
        uintptr_t lease_end = lease->first_page +
            lease->page_count * VMM_PAGE_SIZE;
        if (lease->active &&
            lease->address_space == thread->process->address_space &&
            lease->first_page <= first_page && lease_end > last_page &&
            (!writable || context->leases[index].writable))
            return true;
    }

    if (context->count == context->capacity) {
        usize new_capacity = context->capacity ? context->capacity * 2U : 8U;
        if (new_capacity < context->capacity ||
            new_capacity > ~(usize)0 / sizeof(*context->leases)) {
            context->allocation_failed = true;
            return false;
        }
        syscall_user_lease_entry_t *leases = kmalloc(
            new_capacity * sizeof(*leases));
        if (!leases) {
            context->allocation_failed = true;
            return false;
        }
        if (context->count)
            memcpy(leases, context->leases,
                   context->count * sizeof(*leases));
        kfree(context->leases);
        context->leases = leases;
        context->capacity = new_capacity;
    }

    vmm_user_range_lease_t lease;
    if (!vmm_user_range_lease(thread->process->address_space, buffer,
                              (usize)length, writable, &lease))
        return false;
    context->leases[context->count].writable = writable;
    context->leases[context->count].lease = lease;
    context->count++;
    return true;
}

static bool syscall_user_buffer_valid(const void *buffer, u64 length)
{
    return syscall_user_buffer_accessible(buffer, length, false);
}

static bool syscall_user_buffer_writable(void *buffer, u64 length)
{
    return syscall_user_buffer_accessible(buffer, length, true);
}

static void syscall_user_lease_release_all(
    syscall_user_access_context_t *context)
{
    if (!context) return;
    for (usize i = 0; i < context->count; i++)
        vmm_user_range_release(&context->leases[i].lease);
    kfree(context->leases);
    context->leases = NULL;
    context->count = 0;
    context->capacity = 0;
}

static bool syscall_copy_from_user_byte(char *destination,
                                        const char *source)
{
    kernel_thread_t *thread = thread_current();
    vmm_user_range_lease_t lease;
    bool valid;
    if (!destination || !source || !thread || !thread->process) return false;
    valid = vmm_user_range_lease(thread->process->address_space, source, 1,
                                 false, &lease);
    if (!valid) return false;
    *destination = *source;
    vmm_user_range_release(&lease);
    return true;
}

static void syscall_random_bytes(syscall_frame_t *frame)
{
    void *buffer = (void *)(uintptr_t)frame->rdi;
    u64 length = frame->rsi;
    mg_result_t result = syscall_random_fill(buffer, length);
    if (result != MG_OK) {
        syscall_fail(frame, result);
        return;
    }
    frame->rax = MG_OK;
}

static void syscall_path_release(char **path)
{
    if (path && *path) {
        kfree(*path);
        *path = NULL;
    }
}

#define SYSCALL_PATH_AUTO __attribute__((cleanup(syscall_path_release)))

static int syscall_copy_path(const char *user_path, char **out_path,
                             usize *out_length)
{
    uintptr_t source = (uintptr_t)user_path;
    usize length;
    char *path;
    if (!out_path || !out_length) return VFS_ERR_INVALID_PARAM;
    *out_path = NULL;
    *out_length = 0;
    if (!user_path) return VFS_ERR_INVALID_PARAM;
    for (length = 0; length <= MG_PATH_MAX_BYTES; length++) {
        if (length > (usize)(~(uintptr_t)0 - source))
            return VFS_ERR_INVALID_PARAM;
        char value;
        const char *byte = (const char *)(source + length);
        if (!syscall_copy_from_user_byte(&value, byte))
            return VFS_ERR_INVALID_PARAM;
        if (!value) {
            if (!length) return VFS_ERR_INVALID_PARAM;
            path = (char *)kmalloc(length + 1U);
            if (!path) return VFS_ERR_NO_MEM;
            for (usize i = 0; i <= length; i++) {
                if (i > (usize)(~(uintptr_t)0 - source) ||
                    !syscall_copy_from_user_byte(&path[i],
                        (const char *)(source + i))) {
                    kfree(path);
                    return VFS_ERR_INVALID_PARAM;
                }
                if ((i < length && !path[i]) ||
                    (i == length && path[i] != '\0')) {
                    kfree(path);
                    return VFS_ERR_INVALID_PARAM;
                }
            }
            *out_path = path;
            *out_length = length;
            return VFS_OK;
        }
    }
    return VFS_ERR_NAME_TOO_LONG;
}

static bool syscall_copy_bounded_text(const char *user_text, char *text,
                                      usize size, bool allow_empty)
{
    usize i;
    if (!user_text || !text || size == 0) return false;
    for (i = 0; i < size; i++) {
        if (!syscall_copy_from_user_byte(&text[i], user_text + i))
            return false;
        if (!text[i]) return allow_empty || i != 0;
    }
    return false;
}

static bool syscall_copy_text(const char *user_text, char *text, usize size)
{
    return syscall_copy_bounded_text(user_text, text, size, false);
}

static bool syscall_is_editable_configuration(const char *path)
{
    /* The canonical path is rendered from a retained directory location, so this
     * boundary is deliberately component-based rather than a substring
     * match (for example, /configuration must not qualify). */
    return path && path[0] == '/' && path[1] == 'c' && path[2] == 'o' &&
           path[3] == 'n' && path[4] == 'f' && path[5] == '/' &&
           path[6] != '\0';
}

static i64 syscall_network_open(process_t *process, kernel_object_t *object)
{
    process_handle_t handle;
    if (!object) return MG_ERR_NO_MEMORY;
    if (!process_handle_install(process, object, OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE,
                                &handle)) {
        object_release(object);
        return MG_ERR_NO_MEMORY;
    }
    object_release(object);
    return (i64)handle;
}

static bool syscall_is_network_service(const process_t *process)
{
    return process && process->system_service &&
           process->service_id == MG_SERVICE_NETWORKD &&
           process->credentials_initialized &&
           identity_credentials_has_privilege(
               &process->credentials, IDENTITY_PRIVILEGE_MANAGE_NETWORK);
}

static bool syscall_is_logind(const process_t *process)
{
    return process && process->system_service &&
           process->service_id == MG_SERVICE_LOGIND &&
           process->credentials_initialized &&
           identity_credentials_has_privilege(
               &process->credentials, IDENTITY_PRIVILEGE_MANAGE_SESSIONS);
}

static bool syscall_is_volumed(const process_t *process)
{
    return process && process->system_service &&
           process->service_id == MG_SERVICE_VOLUMED &&
           process->credentials_initialized &&
           identity_credentials_has_privilege(
               &process->credentials, IDENTITY_PRIVILEGE_MANAGE_DEVICES) &&
           identity_credentials_has_privilege(
               &process->credentials, IDENTITY_PRIVILEGE_MANAGE_STORAGE);
}

static bool syscall_is_diskutil(const process_t *process)
{
    return process && !process->system_service &&
           process->credentials_initialized &&
           !strcmp(process->name, "diskutil") &&
           identity_credentials_has_privilege(
               &process->credentials, IDENTITY_PRIVILEGE_MANAGE_STORAGE);
}

static bool syscall_fixed_text_valid(const char *text, usize capacity,
                                     bool allow_empty)
{
    if (!text || !capacity) return false;
    for (usize index = 0; index < capacity; index++) {
        if (text[index] == '\0') return allow_empty || index != 0U;
    }
    return false;
}

static bool syscall_volume_path_valid(const char *path, char *name,
                                      usize name_capacity)
{
    const char prefix[] = "/vol/";
    usize prefix_length = sizeof(prefix) - 1U;
    usize length;

    if (!path || !name || name_capacity < 2U ||
        !syscall_fixed_text_valid(path, MG_VOLUME_MOUNT_MAX, false))
        return false;
    length = strlen(path);
    if (length <= prefix_length || strncmp(path, prefix, prefix_length) != 0)
        return false;
    if (strlen(path + prefix_length) >= name_capacity ||
        !strcmp(path + prefix_length, ".") ||
        !strcmp(path + prefix_length, "..")) return false;
    for (const char *part = path + prefix_length; *part; part++)
        if (*part == '/') return false;
    memcpy(name, path + prefix_length, strlen(path + prefix_length) + 1U);
    return true;
}

static bool syscall_volume_device_allowed(block_device_t *device,
                                          u64 expected_parent_id)
{
    gpt_partition_info_t partition;
    u64 parent_id;

    if (!device || !block_device_is_live(device)) return false;
    if (device->type == BLOCK_DEVICE_USB) {
        if (expected_parent_id != 0ULL) return false;
        for (u32 index = 0; index < block_device_count(); index++) {
            block_device_t *child = block_get_device(index);
            if (!child || child->type != BLOCK_DEVICE_PARTITION) continue;
            if (!gpt_get_partition_info(child, &partition) ||
                !gpt_partition_parent_matches(&partition, device)) continue;
            if (!strcmp(partition.name, GPT_MANGROVE_ROOT_NAME) ||
                !strcmp(partition.name, GPT_MANGROVE_BOOT_NAME)) return false;
        }
        return true;
    }
    if (device->type != BLOCK_DEVICE_PARTITION ||
        !gpt_get_partition_info(device, &partition) || !partition.parent ||
        !block_device_is_live(partition.parent) ||
        partition.parent->type != BLOCK_DEVICE_USB)
        return false;
    parent_id = BLOCK_DEVICE_ID_BASE | (partition.parent->id + 1ULL);
    if (expected_parent_id != parent_id) return false;
    if (!strcmp(partition.name, GPT_MANGROVE_ROOT_NAME) ||
        !strcmp(partition.name, GPT_MANGROVE_BOOT_NAME)) return false;
    return true;
}

static u32 syscall_volume_collect_children(block_device_t *parent,
                                           block_device_t *devices[],
                                           u32 capacity)
{
    u32 count = 0;

    if (!parent || !devices || capacity == 0U) return 0;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *device = block_get_device(index);
        gpt_partition_info_t partition;
        if (!device || !block_device_is_live(device)) continue;
        if (device != parent) {
            if (device->type != BLOCK_DEVICE_PARTITION ||
                !gpt_get_partition_info(device, &partition) ||
                !gpt_partition_parent_matches(&partition, parent)) continue;
        }
        if (count >= capacity) break;
        devices[count++] = device;
    }
    return count;
}

static bool syscall_storage_device_matches(block_device_t *device,
                                           u64 expected_parent_id)
{
    gpt_partition_info_t partition;

    if (!device || !block_device_is_live(device)) return false;
    if (device->type != BLOCK_DEVICE_PARTITION)
        return expected_parent_id == 0ULL;
    if (!gpt_get_partition_info(device, &partition) || !partition.parent ||
        !block_device_is_live(partition.parent)) return false;
    return expected_parent_id ==
           (BLOCK_DEVICE_ID_BASE | (partition.parent->id + 1ULL));
}

static bool syscall_storage_system_managed(block_device_t *device)
{
    gpt_partition_info_t partition;

    if (!device) return true;
    if (device->type == BLOCK_DEVICE_PARTITION) {
        if (!gpt_get_partition_info(device, &partition)) return false;
        if (!strcmp(partition.name, GPT_MANGROVE_ROOT_NAME) ||
            !strcmp(partition.name, GPT_MANGROVE_BOOT_NAME)) return true;
        device = partition.parent;
    }
    if (!device) return true;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *child = block_get_device(index);
        if (!child || child->type != BLOCK_DEVICE_PARTITION ||
            !gpt_get_partition_info(child, &partition) ||
            !gpt_partition_parent_matches(&partition, device)) continue;
        if (!strcmp(partition.name, GPT_MANGROVE_ROOT_NAME) ||
            !strcmp(partition.name, GPT_MANGROVE_BOOT_NAME)) return true;
    }
    return false;
}

static i64 syscall_authorize_volume_request(process_t *process,
                                            process_handle_t request_handle,
                                            u32 operation)
{
    const char *description;

    if (!syscall_is_volumed(process)) return MG_ERR_PRIVILEGE_REQUIRED;
    switch (operation) {
        case MG_VOLUME_OP_MOUNT:
            description = "Mount a removable volume.";
            break;
        case MG_VOLUME_OP_UNMOUNT:
            description = "Unmount a removable volume.";
            break;
        case MG_VOLUME_OP_EJECT:
            description = "Eject removable media.";
            break;
        default:
            return MG_ERR_BAD_ARGUMENT;
    }
    return pass_authorize_request(process, request_handle,
                                  IDENTITY_PRIVILEGE_MANAGE_STORAGE,
                                  description);
}

static void syscall_copy_identity(const user_identity_t *source,
                                  mg_identity_t *destination)
{
    memset(destination, 0, sizeof(*destination));
    destination->uid = source->uid;
    destination->role = source->role;
    strncpy(destination->username, source->username,
            sizeof(destination->username) - 1U);
    strncpy(destination->home, source->home,
            sizeof(destination->home) - 1U);
}

static i64 syscall_authorize_network_request(process_t *process,
                                             process_handle_t request_handle,
                                             u32 operation)
{
    const char *description;

    if (!syscall_is_network_service(process))
        return MG_ERR_PRIVILEGE_REQUIRED;
    switch (operation) {
        case MG_NETWORK_OP_SET_AUTOMATIC:
            description = "Configure the network automatically.";
            break;
        case MG_NETWORK_OP_SET_MANUAL:
            description = "Configure the network manually.";
            break;
        case MG_NETWORK_OP_ENABLE:
            description = "Enable the network interface.";
            break;
        case MG_NETWORK_OP_DISABLE:
            description = "Disable the network interface.";
            break;
        case MG_NETWORK_OP_RENEW:
            description = "Renew the DHCP lease.";
            break;
        case MG_NETWORK_OP_RELOAD:
            description = "Reload the network configuration.";
            break;
        default:
            return MG_ERR_BAD_ARGUMENT;
    }
    return pass_authorize_request(
        process, request_handle, IDENTITY_PRIVILEGE_MANAGE_NETWORK,
        description);
}

static void syscall_network(process_t *process, syscall_frame_t *frame)
{
    mg_net_request_t request;
    kernel_object_t *object;
    i64 result;

    if (!process || !frame || !frame->rdi ||
        !syscall_user_buffer_valid((const void *)(uintptr_t)frame->rdi,
                                   sizeof(request))) {
        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
        return;
    }
    memcpy(&request, (const void *)(uintptr_t)frame->rdi, sizeof(request));
    switch (request.operation) {
        case MG_NET_OP_INFO:
            if (!request.result || request.result_capacity < sizeof(mg_net_info_t) ||
                !syscall_user_buffer_writable(request.result, sizeof(mg_net_info_t))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_info((mg_net_info_t *)request.result); return;
        case MG_NET_OP_RESOLVE_A: {
            char hostname[256];
            if (!request.buffer || !request.result || request.result_capacity < sizeof(mg_ipv4_addr_t) ||
                !syscall_copy_text((const char *)request.buffer, hostname, sizeof(hostname)) ||
                !syscall_user_buffer_writable(request.result, sizeof(mg_ipv4_addr_t))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_resolve_a(hostname, (mg_ipv4_addr_t *)request.result,
                                                 request.timeout_ms);
            return;
        }
        case MG_NET_OP_INTERFACES:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_writable(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_interfaces((mg_net_interface_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_ROUTES:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_writable(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_routes((mg_net_route_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_NEIGHBORS:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_writable(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_neighbors((mg_net_neighbor_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_CONNECTIONS:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_writable(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_connections((mg_net_connection_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_RENEW:
            if (!syscall_is_network_service(process)) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            if (request.flags > 1U) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            frame->rax = (u64)net_user_dhcp_renew(
                request.timeout_ms, request.flags != 0U); return;
        case MG_NET_OP_SET_MANUAL: {
            mg_net_manual_config_t configuration;
            if (!syscall_is_network_service(process)) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            if (!request.buffer || request.buffer_length < sizeof(configuration) ||
                !syscall_user_buffer_valid(request.buffer, sizeof(configuration))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return;
            }
            memcpy(&configuration, request.buffer, sizeof(configuration));
            frame->rax = (u64)net_user_set_manual(&configuration); return;
        }
        case MG_NET_OP_SET_AUTOMATIC: {
            if (!syscall_is_network_service(process)) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            frame->rax = (u64)net_user_set_automatic(request.timeout_ms); return;
        }
        case MG_NET_OP_RELOAD: {
            if (!syscall_is_network_service(process)) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            frame->rax = (u64)net_user_reload(); return;
        }
        case MG_NET_OP_SERVICE_SET_ENABLED:
            if (!syscall_is_network_service(process)) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            if (request.flags > 1U) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            frame->rax = (u64)net_user_set_enabled(request.flags != 0U);
            return;
        case MG_NET_OP_SERVICE_CLEAR:
            if (!syscall_is_network_service(process)) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            frame->rax = (u64)net_user_clear_runtime();
            return;
        case MG_NET_OP_ICMP_OPEN:
            object = net_user_icmp_create();
            frame->rax = (u64)(object ? syscall_network_open(process, object) : MG_ERR_BUSY);
            return;
        case MG_NET_OP_DATAGRAM_OPEN:
            object = net_user_datagram_create(request.endpoint.port, &result);
            frame->rax = (u64)(object ? syscall_network_open(process, object) : result);
            return;
        case MG_NET_OP_STREAM_CONNECT:
            object = net_user_stream_connect(&request.endpoint, request.timeout_ms, &result);
            frame->rax = (u64)(object ? syscall_network_open(process, object) : result);
            return;
        case MG_NET_OP_ICMP_ECHO:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_ICMP,
                                           OBJECT_RIGHT_WRITE);
            if (!object || !request.result || request.result_capacity < sizeof(mg_icmp_echo_result_t) ||
                (!request.buffer && request.buffer_length) ||
                !syscall_user_buffer_writable(request.result, sizeof(mg_icmp_echo_result_t)) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                if (object) object_release(object);
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            result = net_user_icmp_echo(object, &request.endpoint.address,
                                        request.buffer, request.buffer_length,
                                        request.timeout_ms,
                                        (mg_icmp_echo_result_t *)request.result);
            object_release(object);
            frame->rax = (u64)result;
            return;
        case MG_NET_OP_DATAGRAM_SEND:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_DATAGRAM,
                                           OBJECT_RIGHT_WRITE);
            if (!object || (!request.buffer && request.buffer_length) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                if (object) object_release(object);
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            result = net_user_datagram_send(object, &request.endpoint,
                                            request.buffer, request.buffer_length,
                                            request.timeout_ms);
            object_release(object);
            frame->rax = (u64)result;
            return;
        case MG_NET_OP_DATAGRAM_RECEIVE:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_DATAGRAM,
                                           OBJECT_RIGHT_READ);
            if (!object || !request.buffer || !request.result ||
                request.result_capacity < sizeof(mg_datagram_result_t) ||
                !syscall_user_buffer_writable((void *)request.buffer,
                                              request.buffer_length) ||
                !syscall_user_buffer_writable(request.result, sizeof(mg_datagram_result_t))) {
                if (object) object_release(object);
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            result = net_user_datagram_receive(object, (void *)request.buffer,
                                               request.buffer_length, request.timeout_ms,
                                               (mg_datagram_result_t *)request.result);
            object_release(object);
            frame->rax = (u64)result;
            return;
        case MG_NET_OP_STREAM_SEND:
            object = process_handle_lookup(
                process, request.handle, OBJECT_TYPE_NETWORK_STREAM,
                OBJECT_RIGHT_WRITE);
            if (!object || (!request.buffer && request.buffer_length) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                if (object) object_release(object);
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            result = net_user_stream_send(object, request.buffer,
                                          request.buffer_length,
                                          request.timeout_ms);
            object_release(object);
            frame->rax = (u64)result;
            return;
        case MG_NET_OP_STREAM_RECEIVE:
            object = process_handle_lookup(
                process, request.handle, OBJECT_TYPE_NETWORK_STREAM,
                OBJECT_RIGHT_READ);
            if (!object || !request.buffer || !request.buffer_length ||
                !syscall_user_buffer_writable((void *)request.buffer,
                                              request.buffer_length)) {
                if (object) object_release(object);
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            result = net_user_stream_receive(object, (void *)request.buffer,
                                             request.buffer_length,
                                             request.timeout_ms);
            object_release(object);
            frame->rax = (u64)result;
            return;
        case MG_NET_OP_STREAM_CLOSE:
            object = process_handle_lookup(
                process, request.handle, OBJECT_TYPE_NETWORK_STREAM,
                OBJECT_RIGHT_WRITE);
            if (!object) { syscall_fail(frame, MG_ERR_INVALID_HANDLE); return; }
            result = net_user_stream_close(object);
            /* A close request consumes the stream handle even when graceful
             * FIN processing times out or is interrupted. The stream helper
             * aborts the transport on those paths before returning. A busy
             * result leaves the handle available for a later close attempt. */
            if (result != MG_ERR_BUSY &&
                !process_handle_close(process, request.handle) && result == MG_OK)
                result = MG_ERR_INVALID_HANDLE;
            object_release(object);
            frame->rax = (u64)result;
            return;
        case MG_NET_OP_ICMP_CLOSE: {
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_ICMP,
                                           OBJECT_RIGHT_WRITE);
            if (!object) { syscall_fail(frame, MG_ERR_INVALID_HANDLE); return; }
            bool closed = process_handle_close(process, request.handle);
            object_release(object);
            frame->rax = closed ? MG_OK : MG_ERR_INVALID_HANDLE;
            return;
        }
        case MG_NET_OP_DATAGRAM_CLOSE: {
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_DATAGRAM,
                                           OBJECT_RIGHT_WRITE);
            if (!object) { syscall_fail(frame, MG_ERR_INVALID_HANDLE); return; }
            bool closed = process_handle_close(process, request.handle);
            object_release(object);
            frame->rax = closed ? MG_OK : MG_ERR_INVALID_HANDLE;
            return;
        }
        default:
            syscall_fail(frame, MG_ERR_UNSUPPORTED); return;
    }
}

static void syscall_fail(syscall_frame_t *frame, i64 error)
{
    frame->rax = (u64)error;
}

static i64 syscall_vfs_error(int result)
{
    switch (result) {
        case VFS_OK: return MG_OK;
        case VFS_ERR_NOT_FOUND: return MG_ERR_NOT_FOUND;
        case VFS_ERR_NO_MEM: return MG_ERR_NO_MEMORY;
        case VFS_ERR_UNSUPPORTED: return MG_ERR_UNSUPPORTED;
        case VFS_ERR_NOT_EMPTY: return MG_ERR_NOT_EMPTY;
        case VFS_ERR_IO: return MG_ERR_IO;
        case VFS_ERR_UNCERTAIN: return MG_ERR_UNCERTAIN;
        case VFS_ERR_BUFFER_TOO_SMALL: return MG_ERR_BUFFER_TOO_SMALL;
        case VFS_ERR_BAD_FORMAT: return MG_ERR_IO;
        case VFS_ERR_ACCESS_DENIED: return MG_ERR_ACCESS_DENIED;
        case VFS_ERR_NOT_DIRECTORY: return MG_ERR_NOT_DIRECTORY;
        case VFS_ERR_BUSY: return MG_ERR_BUSY;
        case VFS_ERR_DEVICE_GONE: return MG_ERR_DEVICE_GONE;
        case VFS_ERR_NO_SPACE: return MG_ERR_NO_SPACE;
        case VFS_ERR_ALREADY_EXISTS: return MG_ERR_ALREADY_EXISTS;
        case VFS_ERR_STALE_GENERATION: return MG_ERR_STALE_GENERATION;
        case VFS_ERR_NAME_TOO_LONG: return MG_ERR_NAME_TOO_LONG;
        case VFS_ERR_CANCELLED: return MG_ERR_CANCELLED;
        case VFS_ERR_TIME_UNAVAILABLE: return MG_ERR_TIME_UNAVAILABLE;
        default: return MG_ERR_BAD_ARGUMENT;
    }
}

static i64 syscall_object_io_result(bool file_write, i64 result)
{
    if (result >= 0 || result == MG_ERR_DEVICE_GONE ||
        result == MG_ERR_UNCERTAIN) return result;
    if (result == MG_ERR_IO || result == MG_ERR_ACCESS_DENIED ||
        result == MG_ERR_BAD_ARGUMENT || result == MG_ERR_NO_MEMORY ||
        result == MG_ERR_NOT_FOUND || result == MG_ERR_CANCELLED ||
        result == MG_ERR_WOULD_BLOCK) return result;
    if (file_write &&
        (result == MG_ERR_NO_SPACE ||
         result == MG_ERR_ACCESS_DENIED || result == MG_ERR_IO ||
         result == MG_ERR_TIME_UNAVAILABLE))
        return result;
    return MG_ERR_UNSUPPORTED;
}

typedef struct {
    u8 **chunks;
    usize length;
    usize count;
} syscall_replace_input_t;

static void syscall_replace_input_release(syscall_replace_input_t *input)
{
    if (!input) return;
    for (usize i = 0; i < input->count; i++) kfree(input->chunks[i]);
    kfree(input->chunks);
    input->chunks = NULL;
    input->count = 0;
}

static bool syscall_replace_input_copy(syscall_replace_input_t *input,
                                       const void *user_data, usize length)
{
    usize slots;
    if (!input || length > MG_FILE_CONTENT_ADDRESS_MAX ||
        !syscall_user_buffer_valid(user_data, length)) return false;
    input->length = length;
    slots = length / FILE_REPLACE_CHUNK_BYTES +
        (length % FILE_REPLACE_CHUNK_BYTES != 0);
    if (!slots) return true;
    if (slots > (usize)-1 / sizeof(*input->chunks)) return false;
    input->chunks = kmalloc(slots * sizeof(*input->chunks));
    if (!input->chunks) return false;
    memset(input->chunks, 0, slots * sizeof(*input->chunks));
    for (usize i = 0; i < slots; i++) {
        usize offset = i * FILE_REPLACE_CHUNK_BYTES;
        usize bytes = length - offset;
        if (bytes > FILE_REPLACE_CHUNK_BYTES) bytes = FILE_REPLACE_CHUNK_BYTES;
        input->chunks[i] = kmalloc(FILE_REPLACE_CHUNK_BYTES);
        if (!input->chunks[i]) return false;
        input->count++;
        memcpy(input->chunks[i], (const u8 *)user_data + offset, bytes);
    }
    return true;
}

static int syscall_snapshot_copy_chunk(void *context, u64 offset,
                                       const u8 *data, usize length)
{
    mg_file_snapshot_request_t *request = context;
    if (!request || !data || offset > request->capacity ||
        length > request->capacity - offset)
        return VFS_ERR_INVALID_PARAM;
    memcpy(request->data + offset, data, length);
    return VFS_OK;
}

static int syscall_snapshot_copy_state(void *context,
                                       const mg_file_lines_state_t *state)
{
    mg_file_snapshot_request_t *request = context;
    if (!request || !state)
        return VFS_ERR_INVALID_PARAM;
    memcpy(request->state, state, sizeof(*state));
    return VFS_OK;
}

static i64 syscall_file_snapshot(process_t *process, process_handle_t handle,
                                 const mg_file_snapshot_request_t *user_request)
{
    mg_file_snapshot_request_t request;
    kernel_object_t *object;
    int result;
    if (!process || !user_request ||
        !syscall_user_buffer_valid(user_request, sizeof(request)))
        return MG_ERR_BAD_ARGUMENT;
    memcpy(&request, user_request, sizeof(request));
    if (!request.capacity || request.capacity > MG_FILE_CONTENT_ADDRESS_MAX ||
        !syscall_user_buffer_writable(request.data, request.capacity) ||
        !syscall_user_buffer_writable(request.state, sizeof(*request.state)) ||
        ((uintptr_t)request.data <
             (uintptr_t)request.state + sizeof(*request.state) &&
         (uintptr_t)request.state <
             (uintptr_t)request.data + request.capacity))
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, handle, OBJECT_TYPE_FILE,
                                          OBJECT_RIGHT_READ);
    if (!object) return MG_ERR_INVALID_HANDLE;
    result = object_file_snapshot_stream(
        object, request.capacity, syscall_snapshot_copy_chunk,
        syscall_snapshot_copy_state, &request);
    object_release(object);
    return syscall_vfs_error(result);
}

static u32 syscall_replace_source(void *context, u64 offset, u8 *buffer,
                                  u32 requested)
{
    syscall_replace_input_t *input = (syscall_replace_input_t *)context;
    usize cursor, copied = 0;

    if (!input || !buffer || offset > input->length ||
        requested > input->length - offset) return 0;
    cursor = (usize)offset;
    while (copied < requested) {
        usize index = cursor / FILE_REPLACE_CHUNK_BYTES;
        usize within = cursor % FILE_REPLACE_CHUNK_BYTES;
        usize count = FILE_REPLACE_CHUNK_BYTES - within;
        if (index >= input->count || !input->chunks[index]) return 0;
        if (count > requested - copied) count = requested - copied;
        memcpy(buffer + copied, input->chunks[index] + within, count);
        cursor += count;
        copied += count;
    }
    return requested;
}

static i64 syscall_file_replace(process_t *process, process_handle_t handle,
                                const void *user_data, usize length)
{
    syscall_replace_input_t input = {0};
    kernel_object_t *object;
    i64 result;

    if (length > MG_FILE_CONTENT_ADDRESS_MAX)
        return MG_ERR_REPLACEMENT_TOO_LARGE;
    if (!syscall_user_buffer_valid(user_data, length))
        return MG_ERR_BAD_ARGUMENT;
    object = process_handle_lookup(process, handle, OBJECT_TYPE_FILE,
                                          OBJECT_RIGHT_WRITE);
    if (!object) return MG_ERR_INVALID_HANDLE;

    if (!syscall_replace_input_copy(&input, user_data, length)) {
        result = MG_ERR_NO_MEMORY;
        goto cleanup;
    }
    result = syscall_vfs_error(object_file_replace(
        object, length, syscall_replace_source, &input));

cleanup:
    syscall_replace_input_release(&input);
    object_release(object);
    return result;
}

static int syscall_lookup_location(process_t *process, const char *user_path,
                                     vfs_location_t **out)
{
    SYSCALL_PATH_AUTO char *path = NULL;
    usize path_length;
    int result = syscall_copy_path(user_path, &path, &path_length);
    if (result != VFS_OK) return result;
    return process_lookup_path_n(process, path, path_length, out);
}

static int syscall_lookup_parent(process_t *process, const char *user_path,
                                  vfs_location_t **out, char *name, usize capacity,
                                  bool *out_trailing_separator)
{
    SYSCALL_PATH_AUTO char *path = NULL;
    usize path_length;
    int result = syscall_copy_path(user_path, &path, &path_length);
    if (result != VFS_OK) return result;
    return process_lookup_parent_n(process, path, path_length, out, name,
                                    capacity, out_trailing_separator);
}

static i64 syscall_file_create_contents_common(
    process_t *process, const void *user_request, bool stream_request)
{
    mg_file_create_contents_request_t request;
    syscall_replace_input_t input = {0};
    char name[VFS_COMPONENT_MAX_BYTES + 1U];
    VFS_LOCATION_AUTO vfs_location_t *parent = NULL;
    kernel_object_t *object;
    vfs_file_handle_t *file_handle = NULL;
    process_handle_t reserved;
    bool trailing_separator = false;
    i64 result;
    u32 flags = VFS_OPEN_RDWR;

    if (!process || !user_request ||
        !syscall_user_buffer_valid(user_request, stream_request
            ? sizeof(mg_file_create_stream_request_t) : sizeof(request)))
        return MG_ERR_BAD_ARGUMENT;
    if (stream_request) {
        mg_file_create_stream_request_t stream;
        memcpy(&stream, user_request, sizeof(stream));
        if (stream.reserved ||
            (stream.flags & ~(VFS_OPEN_RDWR | VFS_OPEN_APPEND)) ||
            !(stream.flags & VFS_OPEN_RDWR) ||
            ((stream.flags & VFS_OPEN_APPEND) &&
             !(stream.flags & VFS_OPEN_WRITE)))
            return MG_ERR_BAD_ARGUMENT;
        request.path = stream.path;
        request.data = stream.data;
        request.length = stream.length;
        request.out_handle = stream.out_handle;
        flags = stream.flags;
    } else {
        memcpy(&request, user_request, sizeof(request));
    }
    if (request.length > MG_FILE_CONTENT_ADDRESS_MAX)
        return MG_ERR_REPLACEMENT_TOO_LARGE;
    if (!request.out_handle ||
        !syscall_user_buffer_writable(request.out_handle,
                                 sizeof(*request.out_handle)) ||
        !syscall_user_buffer_valid(request.data, request.length))
        return MG_ERR_BAD_ARGUMENT;

    int lookup = syscall_lookup_parent(process, request.path, &parent, name,
                                       sizeof(name), &trailing_separator);
    if (lookup != VFS_OK) return syscall_vfs_error(lookup);
    if (trailing_separator) return MG_ERR_NOT_DIRECTORY;

    object = object_file_prepare();
    if (!object) return MG_ERR_NO_MEMORY;
    u32 rights = 0;
    if (flags & VFS_OPEN_READ) rights |= OBJECT_RIGHT_READ;
    if (flags & VFS_OPEN_WRITE) rights |= OBJECT_RIGHT_WRITE;
    if (!process_handle_reserve(process, object, rights,
                                &reserved)) {
        object_release(object);
        return MG_ERR_NO_MEMORY;
    }
    if (!syscall_replace_input_copy(&input, request.data, request.length)) {
        result = MG_ERR_NO_MEMORY;
        goto failure;
    }

    result = syscall_vfs_error(vfs_file_create_exclusive_at_flags(
        vfs_location_node(parent), name, request.length, syscall_replace_source, &input,
        flags, &file_handle));
    if (result != MG_OK) goto failure;

    /* The output page lease and the reserved handle remain owned by this
     * admitted syscall across the VFS operation. */
    object_file_bind_prepared(object, file_handle);
    if (!process_handle_activate(process, reserved))
        panic("file_create_contents: reserved handle lost");
    *request.out_handle = reserved;
    syscall_replace_input_release(&input);
    return MG_OK;

failure:
    syscall_replace_input_release(&input);
    (void)process_handle_cancel(process, reserved);
    return result;
}

static i64 syscall_file_create_contents(
    process_t *process, const mg_file_create_contents_request_t *user_request)
{
    return syscall_file_create_contents_common(process, user_request, false);
}

static i64 syscall_file_create_stream(
    process_t *process, const mg_file_create_stream_request_t *user_request)
{
    return syscall_file_create_contents_common(process, user_request, true);
}

static i64 syscall_create_path(process_t *process, const char *user_path,
                               bool directory)
{
    VFS_LOCATION_AUTO vfs_location_t *location = NULL;
    char name[VFS_COMPONENT_MAX_BYTES + 1U];
    vfs_node_t *parent = NULL;
    vfs_node_t *created = NULL;
    bool trailing_separator = false;
    int result;
    int lookup_result;

    lookup_result = syscall_lookup_parent(process, user_path, &location, name,
                                           sizeof(name), &trailing_separator);
    parent = vfs_location_node(location);
    if (lookup_result != VFS_OK || !parent) {
        return syscall_vfs_error(lookup_result);
    }
    if (trailing_separator && !directory) return MG_ERR_NOT_DIRECTORY;
    if (parent->type != VFS_TYPE_DIRECTORY) return MG_ERR_NOT_DIRECTORY;
    if (!vfs_check_access(parent, VFS_ACCESS_WRITE)) {
        return MG_ERR_ACCESS_DENIED;
    }
    vfs_node_t *existing = NULL;
    lookup_result = vfs_finddir_checked(parent, name, &existing);
    if (lookup_result == VFS_OK) return MG_ERR_ALREADY_EXISTS;
    if (lookup_result != VFS_ERR_NOT_FOUND) return syscall_vfs_error(lookup_result);
    result = directory ? vfs_mkdir(parent, name, &created) :
                         vfs_create(parent, name, &created);
    return syscall_vfs_error(result);
}

static bool syscall_admin_credentials(process_t *process,
                                      process_credentials_t *credentials)
{
    return process && process == process_current() && credentials &&
           process_get_credentials(process, credentials) &&
           identity_credentials_is_admin(credentials);
}

static bool syscall_admin_can_traverse(const vfs_node_t *node,
                                       const process_credentials_t *credentials)
{
    return node && credentials && vfs_node_is_live(node) &&
           (vfs_check_access(node, VFS_ACCESS_READ) ||
            vfs_administrator_override_allowed(node, credentials));
}

static bool syscall_admin_same_node(const vfs_node_t *left,
                                    const vfs_node_t *right)
{
    return left && right && left->super == right->super &&
           left->inode == right->inode;
}

static int syscall_admin_authorize(process_t *process,
                                   const char *description,
                                   process_credentials_t *credentials)
{
    int result;

    if (!syscall_admin_credentials(process, credentials))
        return MG_ERR_PRIVILEGE_REQUIRED;
    result = pass_authorize_current(IDENTITY_PRIVILEGE_MANAGE_USER_DATA,
                                    description);
    if (result != MG_OK) return result;
    if (!syscall_admin_credentials(process, credentials))
        return MG_ERR_ACCESS_DENIED;
    return MG_OK;
}

static bool syscall_admin_parent_allowed(
    vfs_node_t *parent, const process_credentials_t *credentials)
{
    return parent && parent->type == VFS_TYPE_DIRECTORY && credentials &&
           (vfs_check_access(parent, VFS_ACCESS_WRITE) ||
            vfs_administrator_override_allowed(parent, credentials));
}

static bool syscall_admin_user_parent_allowed(
    vfs_node_t *parent, const process_credentials_t *credentials)
{
    return parent && parent->type == VFS_TYPE_DIRECTORY && credentials &&
           vfs_administrator_override_allowed(parent, credentials);
}

static bool syscall_admin_access(const vfs_node_t *node, void *context)
{
    return syscall_admin_can_traverse(node, context);
}

static int syscall_admin_parent(process_t *process, const char *user_path,
                                 const process_credentials_t *credentials,
                                 vfs_location_t **out, char *name, usize capacity,
                                 bool *out_trailing_separator)
{
    SYSCALL_PATH_AUTO char *path = NULL;
    usize path_length;
    int result = syscall_copy_path(user_path, &path, &path_length);
    if (result != VFS_OK) return result;
    return process_lookup_parent_n_access(process, path, path_length,
        syscall_admin_access, (void *)credentials, out, name, capacity,
        out_trailing_separator);
}

static bool syscall_admin_revalidate(vfs_location_t *location,
                                      process_credentials_t *credentials)
{
    return vfs_location_check_access(location, syscall_admin_access, credentials) == VFS_OK &&
        syscall_admin_parent_allowed(vfs_location_node(location), credentials);
}

static int syscall_admin_create_path(process_t *process, const char *user_path,
                                     bool directory)
{
    VFS_LOCATION_AUTO vfs_location_t *location = NULL;
    char name[VFS_COMPONENT_MAX_BYTES + 1U];
    process_credentials_t credentials;
    bool trailing_separator = false;
    vfs_node_t *created = NULL;
    if (!syscall_admin_credentials(process, &credentials)) return MG_ERR_ACCESS_DENIED;
    int result = syscall_admin_parent(process, user_path, &credentials,
        &location, name, sizeof(name), &trailing_separator);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (trailing_separator && !directory) return MG_ERR_NOT_DIRECTORY;
    vfs_node_t *parent = vfs_location_node(location);
    if (!syscall_admin_user_parent_allowed(parent, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *existing = NULL;
    result = vfs_finddir_trusted_checked(parent, name, &existing);
    if (result == VFS_OK) return MG_ERR_ALREADY_EXISTS;
    if (result != VFS_ERR_NOT_FOUND) return syscall_vfs_error(result);
    result = syscall_admin_authorize(process,
        directory ? "Create a directory in regular-user data."
                  : "Create a file in regular-user data.", &credentials);
    if (result != MG_OK) return result;
    if (!syscall_admin_revalidate(location, &credentials) ||
        !syscall_admin_user_parent_allowed(parent, &credentials)) return MG_ERR_ACCESS_DENIED;
    result = directory
        ? vfs_mkdir_owned(parent, name, parent->owner_uid,
                            VFS_DEFAULT_DIRECTORY_PERMISSIONS, &created)
        : vfs_create_owned(parent, name, parent->owner_uid,
                             VFS_DEFAULT_FILE_PERMISSIONS, &created);
    return syscall_vfs_error(result);
}

static int syscall_admin_open(process_t *process, const char *user_path, u32 flags)
{
    VFS_LOCATION_AUTO vfs_location_t *location = NULL;
    char name[VFS_COMPONENT_MAX_BYTES + 1U];
    process_credentials_t credentials;
    process_handle_t handle;
    bool trailing_separator = false;
    if (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE && flags != VFS_OPEN_RDWR)
        return MG_ERR_BAD_ARGUMENT;
    if (!syscall_admin_credentials(process, &credentials)) return MG_ERR_ACCESS_DENIED;
    int result = syscall_admin_parent(process, user_path, &credentials,
        &location, name, sizeof(name), &trailing_separator);
    if (result != VFS_OK) return syscall_vfs_error(result);
    vfs_node_t *parent = vfs_location_node(location);
    if (!syscall_admin_parent_allowed(parent, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *node = NULL;
    result = vfs_finddir_trusted_checked(parent, name, &node);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (trailing_separator && node->type != VFS_TYPE_DIRECTORY)
        return MG_ERR_NOT_DIRECTORY;
    if (node->type != VFS_TYPE_FILE || node->super != parent->super ||
        !vfs_administrator_override_allowed(node, &credentials)) return MG_ERR_ACCESS_DENIED;
    result = syscall_admin_authorize(process,
        "Open regular-user data with administrator access.", &credentials);
    if (result != MG_OK) return result;
    if (!syscall_admin_revalidate(location, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *current = NULL;
    result = vfs_finddir_trusted_checked(parent, name, &current);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (!syscall_admin_same_node(node, current) ||
        !vfs_administrator_override_allowed(current, &credentials)) return MG_ERR_ACCESS_DENIED;
    kernel_object_t *object = object_file_create_node_user_authorized(current, flags);
    if (!object) return MG_ERR_IO;
    u32 rights = ((flags & VFS_OPEN_READ) ? OBJECT_RIGHT_READ : 0) |
                 ((flags & VFS_OPEN_WRITE) ? OBJECT_RIGHT_WRITE : 0);
    bool installed = process_handle_install(process, object, rights, &handle);
    object_release(object);
    return installed ? (int)handle : MG_ERR_NO_MEMORY;
}

static int syscall_admin_remove(process_t *process, const char *user_path)
{
    VFS_LOCATION_AUTO vfs_location_t *location = NULL;
    char name[VFS_COMPONENT_MAX_BYTES + 1U];
    process_credentials_t credentials;
    bool trailing_separator = false;
    if (!syscall_admin_credentials(process, &credentials)) return MG_ERR_ACCESS_DENIED;
    int result = syscall_admin_parent(process, user_path, &credentials,
        &location, name, sizeof(name), &trailing_separator);
    if (result != VFS_OK) return syscall_vfs_error(result);
    vfs_node_t *parent = vfs_location_node(location);
    if (!syscall_admin_parent_allowed(parent, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *node = NULL;
    result = vfs_finddir_trusted_checked(parent, name, &node);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (trailing_separator && node->type != VFS_TYPE_DIRECTORY)
        return MG_ERR_NOT_DIRECTORY;
    if (node->super != parent->super ||
        !vfs_administrator_override_allowed(node, &credentials)) return MG_ERR_ACCESS_DENIED;
    result = syscall_admin_authorize(process, "Remove regular-user data.", &credentials);
    if (result != MG_OK) return result;
    if (!syscall_admin_revalidate(location, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *current = NULL;
    result = vfs_finddir_trusted_checked(parent, name, &current);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (!syscall_admin_same_node(node, current) ||
        !vfs_administrator_override_allowed(current, &credentials)) return MG_ERR_ACCESS_DENIED;
    result = node->type == VFS_TYPE_DIRECTORY
        ? vfs_rmdir_expected_authorized(parent, name, node->super, node->inode)
        : vfs_unlink_expected_authorized(parent, name, node->super, node->inode);
    return syscall_vfs_error(result);
}

static int syscall_admin_move(process_t *process, const char *source,
                              const char *destination)
{
    VFS_LOCATION_AUTO vfs_location_t *src = NULL;
    VFS_LOCATION_AUTO vfs_location_t *dst = NULL;
    char source_name[VFS_COMPONENT_MAX_BYTES + 1U];
    char destination_name[VFS_COMPONENT_MAX_BYTES + 1U];
    process_credentials_t credentials;
    bool source_trailing_separator = false;
    bool destination_trailing_separator = false;
    if (!syscall_admin_credentials(process, &credentials)) return MG_ERR_ACCESS_DENIED;
    int result = syscall_admin_parent(process, source, &credentials,
        &src, source_name, sizeof(source_name), &source_trailing_separator);
    if (result != VFS_OK) return syscall_vfs_error(result);
    result = syscall_admin_parent(process, destination, &credentials,
        &dst, destination_name, sizeof(destination_name),
        &destination_trailing_separator);
    if (result != VFS_OK) return syscall_vfs_error(result);
    vfs_node_t *sp = vfs_location_node(src), *dp = vfs_location_node(dst);
    if (sp->super != dp->super) return MG_ERR_UNSUPPORTED;
    if (!syscall_admin_parent_allowed(sp, &credentials) ||
        !syscall_admin_parent_allowed(dp, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *node = NULL;
    result = vfs_finddir_trusted_checked(sp, source_name, &node);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (source_trailing_separator && node->type != VFS_TYPE_DIRECTORY)
        return MG_ERR_NOT_DIRECTORY;
    if (node->super != sp->super ||
        !vfs_administrator_override_allowed(node, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *existing = NULL;
    result = vfs_finddir_trusted_checked(dp, destination_name, &existing);
    if (result != VFS_OK && result != VFS_ERR_NOT_FOUND) return syscall_vfs_error(result);
    if (destination_trailing_separator && !existing) return MG_ERR_NOT_FOUND;
    if (destination_trailing_separator &&
        existing->type != VFS_TYPE_DIRECTORY) return MG_ERR_NOT_DIRECTORY;
    if (existing && !syscall_admin_same_node(node, existing)) return MG_ERR_ALREADY_EXISTS;
    result = syscall_admin_authorize(process, "Move regular-user data.", &credentials);
    if (result != MG_OK) return result;
    if (!syscall_admin_revalidate(src, &credentials) ||
        !syscall_admin_revalidate(dst, &credentials)) return MG_ERR_ACCESS_DENIED;
    vfs_node_t *current = NULL;
    result = vfs_finddir_trusted_checked(sp, source_name, &current);
    if (result != VFS_OK) return syscall_vfs_error(result);
    if (!syscall_admin_same_node(node, current) ||
        !vfs_administrator_override_allowed(current, &credentials)) return MG_ERR_ACCESS_DENIED;
    result = vfs_finddir_trusted_checked(dp, destination_name, &existing);
    if (result != VFS_OK && result != VFS_ERR_NOT_FOUND) return syscall_vfs_error(result);
    if (existing && !syscall_admin_same_node(current, existing)) return MG_ERR_ACCESS_DENIED;
    return syscall_vfs_error(vfs_rename_expected_authorized(sp, source_name,
                              node->super, node->inode, dp, destination_name));
}

/* Read-only diagnostics require storage-inspection authority. No PASS is
 * consumed: this interface cannot authorize, finish or discard an owner. */
static i64 syscall_recovery_owners(process_t *process,
    const mg_recovery_owner_snapshot_request_t *user_request)
{
    process_credentials_t credentials;
    if (!process || !process_get_credentials(process, &credentials) ||
        !identity_credentials_has_privilege(&credentials, IDENTITY_PRIVILEGE_MANAGE_STORAGE))
        return MG_ERR_ACCESS_DENIED;
    mg_recovery_owner_snapshot_request_t request;
    if (!user_request || !syscall_user_buffer_valid(user_request, sizeof(request)))
        return MG_ERR_BAD_ARGUMENT;
    memcpy(&request, user_request, sizeof(request));
    if (!request.out_count || request.result_capacity > MG_RECOVERY_OWNER_MAX ||
        !syscall_user_buffer_writable(request.out_count, sizeof(*request.out_count)) ||
        (request.result_capacity && (!request.result ||
         !syscall_user_buffer_writable(request.result,
             (u64)request.result_capacity * sizeof(*request.result)))))
        return MG_ERR_BAD_ARGUMENT;
    uintptr_t entries_address = (uintptr_t)request.result;
    uintptr_t count_address = (uintptr_t)request.out_count;
    usize entries_bytes = (usize)request.result_capacity * sizeof(*request.result);
    if (entries_bytes && (count_address >= entries_address
            ? count_address - entries_address < entries_bytes
            : entries_address - count_address < sizeof(*request.out_count)))
        return MG_ERR_BAD_ARGUMENT; /* Both output contracts require disjoint ranges. */
    vfs_recovery_pin_info_t owners[VFS_MAX_RECOVERY_PINS];
    u32 count = 0;
    int result = vfs_recovery_pins_snapshot(owners, VFS_MAX_RECOVERY_PINS, &count);
    if (result != VFS_OK) return syscall_vfs_error(result);
    *request.out_count = count;
    if (count > request.result_capacity) return MG_ERR_BUFFER_TOO_SMALL;
    for (u32 i = 0; i < count; i++) {
        mg_recovery_owner_info_t info = {
            .owner_id = owners[i].id,
            .state = owners[i].state == VFS_RECOVERY_PIN_UNRESOLVED
                ? MG_RECOVERY_OWNER_UNRESOLVED : MG_RECOVERY_OWNER_ACTIVE,
            .flags = owners[i].flags & (MG_RECOVERY_DEVICE_ONLINE |
                                       MG_RECOVERY_PHYSICAL_ONLINE | MG_RECOVERY_PRIVATE),
        };
        if (owners[i].flags & VFS_RECOVERY_HAS_DEVICE) {
            info.instance_id = BLOCK_DEVICE_ID_BASE | (owners[i].device_id + 1U);
            info.physical_instance_id = BLOCK_DEVICE_ID_BASE | (owners[i].physical_id + 1U);
            if (!(owners[i].flags & VFS_RECOVERY_DEVICE_ONLINE) ||
                !(owners[i].flags & VFS_RECOVERY_PHYSICAL_ONLINE))
                info.flags |= MG_RECOVERY_OFFLINE_RETAINED;
        }
        request.result[i] = info;
    }
    return MG_OK;
}

static i64 syscall_spawn_error(int result)
{
    if (result == VFS_ERR_IO) return MG_ERR_IO;
    if (result == VFS_ERR_CANCELLED) return MG_ERR_CANCELLED;
    if (result == VFS_ERR_DEVICE_GONE) return MG_ERR_DEVICE_GONE;
    if (result == VFS_ERR_UNCERTAIN) return MG_ERR_UNCERTAIN;
    if (result == VFS_ERR_NO_MEM) return MG_ERR_NO_MEMORY;
    return MG_ERR_INVALID_EXEC;
}

static void syscall_dispatch_inner(syscall_frame_t *frame)
{
    if (!frame) return;

    switch (frame->rax) {
        case SYSCALL_OPEN: {
            SYSCALL_PATH_AUTO char *path = NULL;
            SYSCALL_PATH_AUTO char *resolved = NULL;
            usize path_length;
            VFS_LOCATION_AUTO vfs_location_t *location = NULL;
            vfs_node_t *node = NULL;
            int lookup_result;
            u32 flags = (u32)frame->rsi;
            u32 rights;
            bool authorized_configuration_write;
            process_handle_t handle;
            kernel_object_t *object;
            int copy_result = syscall_copy_path(
                (const char *)(uintptr_t)frame->rdi, &path, &path_length);
            if (copy_result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(copy_result));
                return;
            }
            u32 access_flags = flags & VFS_OPEN_RDWR;
            if ((flags & ~(VFS_OPEN_RDWR | VFS_OPEN_APPEND)) ||
                !access_flags ||
                ((flags & VFS_OPEN_APPEND) &&
                 !(access_flags & VFS_OPEN_WRITE))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            lookup_result = process_lookup_path_n(process_current(), path,
                                                   path_length, &location);
            node = vfs_location_node(location);
            if (lookup_result != VFS_OK || !node) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            if (node->type != VFS_TYPE_FILE) {
                syscall_fail(frame, MG_ERR_NOT_FOUND);
                return;
            }
            char path_probe[1];
            usize resolved_size = 0;
            if (vfs_location_path(location, path_probe, sizeof(path_probe),
                                  &resolved_size) == VFS_ERR_BUFFER_TOO_SMALL &&
                resolved_size <= VFS_PATH_MAX_BYTES + 1U) {
                resolved = (char *)kmalloc(resolved_size);
                if (resolved && vfs_location_path(location, resolved,
                        resolved_size, NULL) != VFS_OK) {
                    kfree(resolved);
                    resolved = NULL;
                }
            }
            authorized_configuration_write =
                (flags & VFS_OPEN_WRITE) != 0U &&
                syscall_is_editable_configuration(resolved ? resolved : "");
            if (((flags & VFS_OPEN_READ) &&
                 !vfs_check_access(node, VFS_ACCESS_READ)) ||
                ((flags & VFS_OPEN_WRITE) && !authorized_configuration_write &&
                 !vfs_check_access(node, VFS_ACCESS_WRITE))) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            if (authorized_configuration_write) {
                int authorization_result = pass_authorize_current(
                    IDENTITY_PRIVILEGE_MANAGE_CONFIGURATION,
                    "Modify administrator configuration.");
                if (authorization_result != MG_OK) {
                    syscall_fail(frame, authorization_result);
                    return;
                }
            }
            object = authorized_configuration_write
                ? object_file_create_node_authorized(node, flags)
                : object_file_create_node(node, flags);
            if (!object) {
                syscall_fail(frame, MG_ERR_IO);
                return;
            }
            rights = 0;
            if (flags & VFS_OPEN_READ) rights |= OBJECT_RIGHT_READ;
            if (flags & VFS_OPEN_WRITE) rights |= OBJECT_RIGHT_WRITE;
            if (!process_handle_install(process_current(), object, rights,
                                        &handle)) {
                object_release(object);
                syscall_fail(frame, MG_ERR_NO_MEMORY);
                return;
            }
            object_release(object);
            frame->rax = handle;
            return;
        }
        case SYSCALL_READ:
        case SYSCALL_WRITE:
        case SYSCALL_WRITE_LITERAL: {
            process_handle_t handle = (process_handle_t)frame->rdi;
            void *buffer = (void *)(uintptr_t)frame->rsi;
            u64 length = frame->rdx;
            process_t *process = process_current();
            kernel_object_t *object = process_handle_lookup(
                process, handle, OBJECT_TYPE_INVALID,
                frame->rax == SYSCALL_READ ? OBJECT_RIGHT_READ : OBJECT_RIGHT_WRITE);
            i64 result;
            bool file_write;
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            if (!(frame->rax == SYSCALL_READ
                    ? syscall_user_buffer_writable(buffer, length)
                    : syscall_user_buffer_valid(buffer, length))) {
                object_release(object);
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = frame->rax == SYSCALL_READ
                ? object_read(object, buffer, length)
                : frame->rax == SYSCALL_WRITE_LITERAL
                    ? object_write_literal(object, buffer, length)
                    : object_write(object, buffer, length);
            file_write = frame->rax != SYSCALL_READ &&
                         object->type == OBJECT_TYPE_FILE;
            object_release(object);
            frame->rax = (u64)syscall_object_io_result(file_write, result);
            return;
        }
        case SYSCALL_CLOSE:
            frame->rax = process_handle_close(
                             process_current(),
                             (process_handle_t)frame->rdi)
                ? (u64)MG_OK : (u64)MG_ERR_INVALID_HANDLE;
            return;
        case SYSCALL_SPAWN: {
            char cmdline[256];
            process_handle_t handle;
            if (!syscall_copy_text((const char *)(uintptr_t)frame->rdi,
                                   cmdline, sizeof(cmdline))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            /* The spawn argument is a command line, not a filesystem path.
             * Resolving the complete string would normalize slashes inside
             * arguments (for example, "http://" became "http:/").
             * process_spawn() parses the copied command line and resolves
             * only argv[0] against the parent's working directory. */
            int spawn_result = process_spawn_result(process_current(), cmdline, &handle);
            if (spawn_result != VFS_OK) {
                syscall_fail(frame, syscall_spawn_error(spawn_result));
                return;
            }
            frame->rax = handle;
            return;
        }
        case SYSCALL_SPAWN_ARGV:
        case SYSCALL_SPAWN_ARGV_WITH_OUTPUT: {
            bool with_output = frame->rax == SYSCALL_SPAWN_ARGV_WITH_OUTPUT;
            process_arguments_t args;
            process_handle_t handle;
            if (with_output && (!frame->rdx || frame->rdx > UINT32_MAX)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            mg_result_t copied = process_arguments_collect(&args,
                (uintptr_t)frame->rdi, (usize)frame->rsi,
                syscall_user_buffer_valid);
            if (copied != MG_OK) {
                syscall_fail(frame, copied);
                return;
            }
            int result = process_spawn_arguments_result(process_current(), &args,
                !with_output, with_output ? (process_handle_t)frame->rdx : 0,
                &handle);
            process_arguments_destroy(&args);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_spawn_error(result));
                return;
            }
            frame->rax = handle;
            return;
        }
        case SYSCALL_SPAWN_WITH_OUTPUT: {
            char cmdline[256];
            process_handle_t handle;
            if (!syscall_copy_text((const char *)(uintptr_t)frame->rdi,
                                   cmdline, sizeof(cmdline))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            int spawn_result = process_spawn_with_output_result(
                    process_current(), cmdline, (process_handle_t)frame->rsi, &handle);
            if (spawn_result != VFS_OK) {
                syscall_fail(frame, syscall_spawn_error(spawn_result));
                return;
            }
            frame->rax = handle;
            return;
        }
        case SYSCALL_REDIRECT_OUTPUT: {
            process_handle_t saved;
            process_handle_t *user_saved =
                (process_handle_t *)(uintptr_t)frame->rsi;
            if (!user_saved || !syscall_user_buffer_writable(user_saved,
                                                          sizeof(*user_saved)) ||
                !process_redirect_output(process_current(),
                                         (process_handle_t)frame->rdi,
                                         &saved)) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            *user_saved = saved;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_RESTORE_OUTPUT:
            frame->rax = process_restore_output(
                process_current(), (process_handle_t)frame->rdi)
                ? (u64)MG_OK : (u64)MG_ERR_INVALID_HANDLE;
            return;
        case SYSCALL_WAIT: {
            process_handle_t handle = (process_handle_t)frame->rdi;
            i32 status;
            int result;
            i32 *user_status = (i32 *)(uintptr_t)frame->rsi;
            if (!syscall_user_buffer_writable(user_status, sizeof(*user_status))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = process_wait(process_current(), handle, &status);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            *user_status = status;
            frame->rax = 0;
            return;
        }
        case SYSCALL_CHDIR: {
            SYSCALL_PATH_AUTO char *path = NULL;
            usize path_length;
            int result;
            result = syscall_copy_path((const char *)(uintptr_t)frame->rdi,
                                       &path, &path_length);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            result = process_chdir_result_n(process_current(), path, path_length);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = 0;
            return;
        }
        case SYSCALL_MEMORY_MAP: {
            uintptr_t address;
            uintptr_t *user_address = (uintptr_t *)(uintptr_t)frame->rsi;
            i64 result;

            if (!syscall_user_buffer_writable(user_address,
                                           sizeof(*user_address))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = process_memory_map(process_current(), (usize)frame->rdi,
                                        &address);
            if (result < 0) {
                syscall_fail(frame, result);
                return;
            }
            *user_address = address;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_MEMORY_UNMAP:
            frame->rax = (u64)process_memory_unmap(
                process_current(), (uintptr_t)frame->rdi);
            return;
        case SYSCALL_GETCWD: {
            process_t *process = process_current();
            char *buffer = (char *)(uintptr_t)frame->rdi;
            usize capacity = (usize)frame->rsi;
            usize *user_size = (usize *)(uintptr_t)frame->rdx;
            usize required = 0;

            if (!process || !buffer || capacity == 0 || !user_size ||
                !syscall_user_buffer_writable(buffer, capacity) ||
                !syscall_user_buffer_writable(user_size, sizeof(*user_size))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            int result = process_cwd_path(process, buffer, capacity, &required);
            *user_size = required;
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = required - 1;
            return;
        }
        case SYSCALL_PATH_INFO: {
            VFS_LOCATION_AUTO vfs_location_t *location = NULL;
            vfs_node_t *node = NULL;
            mg_path_info_t *info = (mg_path_info_t *)(uintptr_t)frame->rsi;
            vfs_node_metadata_t metadata;
            int lookup_result;

            if (!info || !syscall_user_buffer_writable(info, sizeof(*info))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            lookup_result = syscall_lookup_location(process_current(),
                (const char *)(uintptr_t)frame->rdi, &location);
            node = vfs_location_node(location);
            if (lookup_result != VFS_OK || !node) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            lookup_result = vfs_node_metadata_snapshot(node, &metadata);
            if (lookup_result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            mg_path_info_t result = {0};
            result.type = metadata.type == VFS_TYPE_DIRECTORY
                ? MG_PATH_TYPE_DIRECTORY : MG_PATH_TYPE_FILE;
            result.permissions = metadata.permissions;
            result.size = metadata.size;
            result.identifier = metadata.inode;
            result.owner_uid = metadata.owner_uid;
            memcpy(info, &result, sizeof(result));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_DIRECTORY_OPEN: {
            VFS_LOCATION_AUTO vfs_location_t *location = NULL;
            vfs_node_t *node = NULL;
            int lookup_result;
            process_handle_t handle;
            kernel_object_t *object;

            lookup_result = syscall_lookup_location(process_current(),
                (const char *)(uintptr_t)frame->rdi, &location);
            node = vfs_location_node(location);
            if (lookup_result != VFS_OK || !node) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            if (node->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, MG_ERR_NOT_DIRECTORY);
                return;
            }
            if (!vfs_check_access(node, VFS_ACCESS_READ)) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            object = object_directory_create_node(node);
            if (!object) {
                syscall_fail(frame, MG_ERR_IO);
                return;
            }
            if (!process_handle_install(process_current(), object,
                                        OBJECT_RIGHT_READ, &handle)) {
                object_release(object);
                syscall_fail(frame, MG_ERR_NO_MEMORY);
                return;
            }
            object_release(object);
            frame->rax = handle;
            return;
        }
        case SYSCALL_DIRECTORY_READ: {
            mg_directory_entry_t *user_entry =
                (mg_directory_entry_t *)(uintptr_t)frame->rsi;
            kernel_object_t *object;
            vfs_dirent_t entry;
            i64 result;

            if (!user_entry ||
                !syscall_user_buffer_writable(user_entry, sizeof(*user_entry))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            object = process_handle_lookup(process_current(),
                                           (process_handle_t)frame->rdi,
                                           OBJECT_TYPE_DIRECTORY,
                                           OBJECT_RIGHT_READ);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            result = object_directory_read(object, &entry);
            object_release(object);
            if (result < 0) {
                syscall_fail(frame, result);
                return;
            }
            if (result == 0) {
                syscall_fail(frame, MG_ERR_END_OF_FILE);
                return;
            }
            usize name_length = 0;
            while (name_length < sizeof(entry.name) && entry.name[name_length])
                name_length++;
            if (name_length >= sizeof(user_entry->name)) {
                syscall_fail(frame, MG_ERR_NAME_TOO_LONG);
                return;
            }
            memcpy(user_entry->name, entry.name, sizeof(user_entry->name));
            user_entry->type = entry.type == VFS_TYPE_DIRECTORY
                ? MG_PATH_TYPE_DIRECTORY : MG_PATH_TYPE_FILE;
            user_entry->reserved = 0;
            user_entry->identifier = entry.inode;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_DIRECTORY_READ_BATCH: {
            mg_directory_entry_t *user_entries =
                (mg_directory_entry_t *)(uintptr_t)frame->rsi;
            kernel_object_t *object;
            vfs_dirent_t *entries;
            u64 capacity = frame->rdx;
            i64 result;

            if (!user_entries || capacity == 0 ||
                capacity > VFS_DIRECTORY_BATCH_MAX ||
                !syscall_user_buffer_writable(user_entries,
                    (usize)capacity * sizeof(*user_entries))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            object = process_handle_lookup(process_current(),
                                           (process_handle_t)frame->rdi,
                                           OBJECT_TYPE_DIRECTORY,
                                           OBJECT_RIGHT_READ);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            entries = (vfs_dirent_t *)kmalloc(
                (usize)capacity * sizeof(*entries));
            if (!entries) {
                object_release(object);
                syscall_fail(frame, MG_ERR_NO_MEMORY);
                return;
            }
            result = object_directory_read_batch(object, entries,
                (u32)capacity, sizeof(((mg_directory_entry_t *)0)->name));
            object_release(object);
            if (result < 0) {
                kfree(entries);
                syscall_fail(frame, result);
                return;
            }
            for (i64 i = 0; i < result; i++) {
                memcpy(user_entries[i].name, entries[i].name,
                       sizeof(user_entries[i].name));
                user_entries[i].type = entries[i].type == VFS_TYPE_DIRECTORY
                    ? MG_PATH_TYPE_DIRECTORY : MG_PATH_TYPE_FILE;
                user_entries[i].reserved = 0;
                user_entries[i].identifier = entries[i].inode;
            }
            kfree(entries);
            if (result == 0) {
                syscall_fail(frame, MG_ERR_END_OF_FILE);
                return;
            }
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_DIRECTORY_READ_EX: {
            mg_directory_entry_ex_t *user_entry =
                (mg_directory_entry_ex_t *)(uintptr_t)frame->rsi;
            kernel_object_t *object;
            vfs_dirent_t entry;
            i64 result;

            if (!user_entry || !syscall_user_buffer_writable(user_entry,
                    sizeof(*user_entry))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            object = process_handle_lookup(process_current(),
                (process_handle_t)frame->rdi, OBJECT_TYPE_DIRECTORY,
                OBJECT_RIGHT_READ);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            result = object_directory_read(object, &entry);
            object_release(object);
            if (result < 0) {
                syscall_fail(frame, result);
                return;
            }
            if (result == 0) {
                syscall_fail(frame, MG_ERR_END_OF_FILE);
                return;
            }
            memcpy(user_entry->name, entry.name, sizeof(user_entry->name));
            user_entry->type = entry.type == VFS_TYPE_DIRECTORY
                ? MG_PATH_TYPE_DIRECTORY : MG_PATH_TYPE_FILE;
            user_entry->reserved = 0;
            user_entry->identifier = entry.inode;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_DIRECTORY_READ_BATCH_EX: {
            mg_directory_entry_ex_t *user_entries =
                (mg_directory_entry_ex_t *)(uintptr_t)frame->rsi;
            kernel_object_t *object;
            vfs_dirent_t *entries;
            u64 capacity = frame->rdx;
            i64 result;

            if (!user_entries || capacity == 0 ||
                capacity > VFS_DIRECTORY_BATCH_MAX ||
                !syscall_user_buffer_writable(user_entries,
                    (usize)capacity * sizeof(*user_entries))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            object = process_handle_lookup(process_current(),
                (process_handle_t)frame->rdi, OBJECT_TYPE_DIRECTORY,
                OBJECT_RIGHT_READ);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            entries = (vfs_dirent_t *)kmalloc(
                (usize)capacity * sizeof(*entries));
            if (!entries) {
                object_release(object);
                syscall_fail(frame, MG_ERR_NO_MEMORY);
                return;
            }
            result = object_directory_read_batch(object, entries,
                (u32)capacity, sizeof(((mg_directory_entry_ex_t *)0)->name));
            object_release(object);
            if (result < 0) {
                kfree(entries);
                syscall_fail(frame, result);
                return;
            }
            for (i64 i = 0; i < result; i++) {
                memcpy(user_entries[i].name, entries[i].name,
                       sizeof(user_entries[i].name));
                user_entries[i].type = entries[i].type == VFS_TYPE_DIRECTORY
                    ? MG_PATH_TYPE_DIRECTORY : MG_PATH_TYPE_FILE;
                user_entries[i].reserved = 0;
                user_entries[i].identifier = entries[i].inode;
            }
            kfree(entries);
            if (result == 0) {
                syscall_fail(frame, MG_ERR_END_OF_FILE);
                return;
            }
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_FILE_CREATE:
            frame->rax = (u64)syscall_create_path(
                process_current(), (const char *)(uintptr_t)frame->rdi, false);
            return;
        case SYSCALL_DIRECTORY_CREATE:
            frame->rax = (u64)syscall_create_path(
                process_current(), (const char *)(uintptr_t)frame->rdi, true);
            return;
        case SYSCALL_PATH_MOVE: {
            VFS_LOCATION_AUTO vfs_location_t *source_location = NULL;
            VFS_LOCATION_AUTO vfs_location_t *destination_location = NULL;
            char source_name[VFS_COMPONENT_MAX_BYTES + 1U];
            char destination_name[VFS_COMPONENT_MAX_BYTES + 1U];
            vfs_node_t *source_parent = NULL, *destination_parent = NULL;
            vfs_node_t *source_node;
            vfs_super_t *source_super;
            u64 source_inode;
            bool source_trailing_separator = false;
            bool destination_trailing_separator = false;
            int result;

            int source_lookup = syscall_lookup_parent(process_current(),
                (const char *)(uintptr_t)frame->rdi, &source_location,
                source_name, sizeof(source_name), &source_trailing_separator);
            int destination_lookup = syscall_lookup_parent(process_current(),
                (const char *)(uintptr_t)frame->rsi, &destination_location,
                destination_name, sizeof(destination_name),
                &destination_trailing_separator);
            source_parent = vfs_location_node(source_location);
            destination_parent = vfs_location_node(destination_location);
            if (source_lookup != VFS_OK ||
                !source_parent || destination_lookup != VFS_OK ||
                !destination_parent) {
                syscall_fail(frame, syscall_vfs_error(source_lookup != VFS_OK
                    ? source_lookup : destination_lookup));
                return;
            }
            if (source_parent->type != VFS_TYPE_DIRECTORY ||
                destination_parent->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, MG_ERR_NOT_DIRECTORY);
                return;
            }
            if (!vfs_check_access(source_parent, VFS_ACCESS_WRITE) ||
                !vfs_check_access(destination_parent, VFS_ACCESS_WRITE)) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            result = vfs_finddir_checked(source_parent, source_name, &source_node);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            if (source_trailing_separator &&
                source_node->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, MG_ERR_NOT_DIRECTORY);
                return;
            }
            vfs_node_t *destination_node = NULL;
            result = vfs_finddir_checked(destination_parent, destination_name, &destination_node);
            if (result != VFS_OK && result != VFS_ERR_NOT_FOUND) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            if (destination_trailing_separator && !destination_node) {
                syscall_fail(frame, MG_ERR_NOT_FOUND);
                return;
            }
            if (destination_trailing_separator &&
                destination_node->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, MG_ERR_NOT_DIRECTORY);
                return;
            }
            if (destination_node && (destination_node->super != source_node->super ||
                                     destination_node->inode != source_node->inode)) {
                syscall_fail(frame, MG_ERR_ALREADY_EXISTS);
                return;
            }
            if (source_parent->super != destination_parent->super) {
                syscall_fail(frame, MG_ERR_UNSUPPORTED);
                return;
            }
            source_super = source_node->super;
            source_inode = source_node->inode;
            result = vfs_rename_expected(source_parent, source_name,
                                         source_super, source_inode,
                                         destination_parent, destination_name);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }
        case SYSCALL_PATH_REMOVE: {
            char name[VFS_COMPONENT_MAX_BYTES + 1U];
            VFS_LOCATION_AUTO vfs_location_t *location = NULL;
            vfs_node_t *parent = NULL, *node;
            bool trailing_separator = false;
            int result;
            int lookup_result;

            lookup_result = syscall_lookup_parent(process_current(),
                (const char *)(uintptr_t)frame->rdi, &location, name,
                sizeof(name), &trailing_separator);
            parent = vfs_location_node(location);
            if (lookup_result != VFS_OK || !parent ||
                parent->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, lookup_result != VFS_OK
                    ? syscall_vfs_error(lookup_result) : MG_ERR_NOT_DIRECTORY);
                return;
            }
            if (!vfs_check_access(parent, VFS_ACCESS_WRITE)) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            result = vfs_finddir_checked(parent, name, &node);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            if (trailing_separator && node->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, MG_ERR_NOT_DIRECTORY);
                return;
            }
            result = node->type == VFS_TYPE_DIRECTORY
                ? vfs_rmdir_expected(parent, name, node->super, node->inode)
                : vfs_unlink_expected(parent, name, node->super, node->inode);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }
        case SYSCALL_FILESYSTEM_ADMIN: {
            mg_filesystem_admin_request_t request;
            mg_filesystem_admin_request_t *user_request =
                (mg_filesystem_admin_request_t *)(uintptr_t)frame->rdi;

            if (!user_request || !syscall_user_buffer_valid(user_request,
                                                              sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            switch (request.operation) {
                case MG_FILESYSTEM_ADMIN_OPEN:
                    if (!request.source || request.destination ||
                        (request.flags & ~(VFS_OPEN_RDWR | VFS_OPEN_APPEND)) ||
                        !(request.flags & VFS_OPEN_RDWR) ||
                        ((request.flags & VFS_OPEN_APPEND) &&
                         !(request.flags & VFS_OPEN_WRITE))) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    frame->rax = (u64)syscall_admin_open(
                        process_current(), request.source, request.flags);
                    return;
                case MG_FILESYSTEM_ADMIN_CREATE_FILE:
                case MG_FILESYSTEM_ADMIN_CREATE_DIRECTORY:
                    if (!request.source || request.destination || request.flags) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    frame->rax = (u64)syscall_admin_create_path(
                        process_current(), request.source,
                        request.operation == MG_FILESYSTEM_ADMIN_CREATE_DIRECTORY);
                    return;
                case MG_FILESYSTEM_ADMIN_MOVE:
                    if (!request.source || !request.destination || request.flags) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    frame->rax = (u64)syscall_admin_move(
                        process_current(), request.source,
                        request.destination);
                    return;
                case MG_FILESYSTEM_ADMIN_REMOVE:
                    if (!request.source || request.destination || request.flags) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    frame->rax = (u64)syscall_admin_remove(
                        process_current(), request.source);
                    return;
                default:
                    syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                    return;
            }
        }
        case SYSCALL_FILE_APPEND_LINES: {
            const mg_file_append_lines_request_t *user_request =
                (const mg_file_append_lines_request_t *)(uintptr_t)frame->rsi;
            mg_file_append_lines_request_t request;
            kernel_object_t *object;
            char *data = NULL;
            mg_file_lines_state_t state = {0};
            int result;

            if (!syscall_user_buffer_valid(user_request, sizeof(*user_request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.max_lines || request.max_lines > MG_FILE_LINES_MAX ||
                (request.flags & ~MG_FILE_LINES_RESET) ||
                (request.state && !syscall_user_buffer_writable(request.state, sizeof(state))) ||
                request.length > MG_FILE_LINES_MAX * MG_FILE_LINE_CAPACITY ||
                (request.length && !syscall_user_buffer_valid(request.data,
                                                               request.length))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            object = process_handle_lookup(process_current(),
                (process_handle_t)frame->rdi, OBJECT_TYPE_FILE,
                OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            if (request.length) {
                data = kmalloc(request.length);
                if (!data) {
                    object_release(object);
                    syscall_fail(frame, MG_ERR_NO_MEMORY);
                    return;
                }
                memcpy(data, request.data, request.length);
            }
            result = object_file_append_lines(object, data, request.length,
                                              request.max_lines, request.flags, &state);
            object_release(object);
            if (result == VFS_OK && request.state)
                memcpy(request.state, &state, sizeof(state));
            kfree(data);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }

        case SYSCALL_FILE_SNAPSHOT: {
            frame->rax = (u64)syscall_file_snapshot(
                process_current(), (process_handle_t)frame->rdi,
                (const mg_file_snapshot_request_t *)(uintptr_t)frame->rsi);
            return;
        }
        case SYSCALL_FILE_REPLACE:
            frame->rax = (u64)syscall_file_replace(
                process_current(), (process_handle_t)frame->rdi,
                (const void *)(uintptr_t)frame->rsi, (usize)frame->rdx);
            return;
        case SYSCALL_FILE_CREATE_CONTENTS:
            frame->rax = (u64)syscall_file_create_contents(
                process_current(),
                (const mg_file_create_contents_request_t *)(uintptr_t)frame->rdi);
            return;
        case SYSCALL_FILE_CREATE_STREAM:
            frame->rax = (u64)syscall_file_create_stream(
                process_current(),
                (const mg_file_create_stream_request_t *)(uintptr_t)frame->rdi);
            return;
        case SYSCALL_FILE_TRUNCATE: {
            kernel_object_t *object = process_handle_lookup(
                process_current(), (process_handle_t)frame->rdi,
                OBJECT_TYPE_FILE, OBJECT_RIGHT_WRITE);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            int result = object_file_truncate(object);
            object_release(object);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }
        case SYSCALL_FILE_SEEK: {
            kernel_object_t *object = process_handle_lookup(
                process_current(), (process_handle_t)frame->rdi,
                OBJECT_TYPE_FILE, 0);
            if (!object || frame->rdx > MG_SEEK_END) {
                if (object) object_release(object);
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE :
                             MG_ERR_BAD_ARGUMENT);
                return;
            }
            int result = object_file_seek(object, (i64)frame->rsi,
                                          (u32)frame->rdx);
            object_release(object);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }
        case SYSCALL_FILE_POSITION: {
            kernel_object_t *object = process_handle_lookup(
                process_current(), (process_handle_t)frame->rdi,
                OBJECT_TYPE_FILE, 0);
            u64 position = 0;
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            int result = object_file_position(object, &position);
            object_release(object);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            if (position > INT64_MAX) {
                syscall_fail(frame, MG_ERR_BUFFER_TOO_SMALL);
                return;
            }
            frame->rax = position;
            return;
        }
        case SYSCALL_CONSOLE_TRANSACTION: {
            u64 op = frame->rdi;
            process_t *process = process_current();
            if (op > 1U) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!process || (op == 1U
                    ? !terminal_begin_batch_for_process(process->pid)
                    : !terminal_end_batch_for_process(process->pid))) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_CONSOLE_INPUT_MODE:
            if (frame->rdi > 1U || !process_current()) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!terminal_process_controls(process_current()->pid)) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            if (frame->rdi)
                terminal_cursor_disable();
            else
                terminal_cursor_enable();
            frame->rax = MG_OK;
            return;
        case SYSCALL_TERMINAL_CONTROL: {
            process_t *process = process_current();
            u32 operation = (u32)frame->rdi;
            i64 result = MG_OK;

            if (!process ||
                operation < MG_TERMINAL_OP_RESERVED_ALTERNATE_ENTER ||
                operation > MG_TERMINAL_OP_SESSION_HISTORY_RESET) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }

            switch (operation) {
                case MG_TERMINAL_OP_RESERVED_ALTERNATE_ENTER:
                case MG_TERMINAL_OP_RESERVED_ALTERNATE_LEAVE:
                case MG_TERMINAL_OP_RESERVED_CURSOR_MOVE:
                case MG_TERMINAL_OP_RESERVED_CURSOR_VISIBILITY:
                case MG_TERMINAL_OP_RESERVED_CLEAR:
                case MG_TERMINAL_OP_RESERVED_STYLED_WRITE:
                case MG_TERMINAL_OP_RESERVED_SEMANTIC_WRITE:
                    result = MG_ERR_UNSUPPORTED;
                    break;
                case MG_TERMINAL_OP_SIZE: {
                    mg_terminal_size_t size;
                    if (frame->rsi || !frame->rdx ||
                        !syscall_user_buffer_writable(
                            (void *)(uintptr_t)frame->rdx, sizeof(size))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    size.version = MG_TERMINAL_API_VERSION;
                    u32 rows;
                    u32 columns;
                    terminal_get_dimensions(&rows, &columns);
                    size.rows = rows;
                    size.columns = columns;
                    memcpy((void *)(uintptr_t)frame->rdx, &size,
                           sizeof(size));
                    break;
                }
                case MG_TERMINAL_OP_CAPABILITIES: {
                    mg_terminal_capabilities_t capabilities;
                    process_t *process = process_current();
                    kernel_object_t *output;
                    u32 capability_mask;
                    if (frame->rsi || !frame->rdx ||
                        !syscall_user_buffer_writable(
                            (void *)(uintptr_t)frame->rdx,
                            sizeof(capabilities))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    capabilities.version = MG_TERMINAL_API_VERSION;
                    terminal_get_capability_mask(&capability_mask);
                    output = process_handle_lookup(
                        process, PROCESS_INITIAL_CONSOLE_HANDLE,
                        OBJECT_TYPE_INVALID, OBJECT_RIGHT_WRITE);
                    if (output) {
                        if (output->type == OBJECT_TYPE_CONSOLE)
                            capability_mask |=
                                TERMINAL_CAP_INTERACTIVE_OUTPUT;
                        object_release(output);
                    }
                    capabilities.capabilities = capability_mask;
                    memcpy((void *)(uintptr_t)frame->rdx, &capabilities,
                           sizeof(capabilities));
                    break;
                }
                case MG_TERMINAL_OP_OVERLAY_SET: {
                    mg_terminal_overlay_request_t request;
                    const terminal_overlay_cell_t *cells =
                        (const terminal_overlay_cell_t *)(uintptr_t)frame->rdx;
                    u64 cell_bytes;

                    if (!frame->rsi || !frame->rdx ||
                        !syscall_user_buffer_valid(
                            (const void *)(uintptr_t)frame->rsi,
                            sizeof(request))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    memcpy(&request, (const void *)(uintptr_t)frame->rsi,
                           sizeof(request));
                    if (request.version != MG_TERMINAL_API_VERSION ||
                        request.reserved || !request.rows ||
                        !request.columns || request.rows > 128U ||
                        request.columns > 256U ||
                        request.rows > (~(u64)0 / request.columns) ||
                        request.cell_count !=
                            (u64)request.rows * request.columns ||
                        request.cell_count > (~(u64)0 / sizeof(*cells))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    cell_bytes = request.cell_count * sizeof(*cells);
                    if (!syscall_user_buffer_valid(cells, cell_bytes) ||
                        !terminal_overlay_set_for_process(
                            process->pid, request.rows, request.columns,
                            cells))
                        result = MG_ERR_BUSY;
                    break;
                }
                case MG_TERMINAL_OP_OVERLAY_CLEAR:
                    if (frame->rsi || frame->rdx ||
                        !terminal_overlay_clear_for_process(process->pid))
                        result = MG_ERR_ACCESS_DENIED;
                    break;
                case MG_TERMINAL_OP_INPUT_CURSOR: {
                    mg_terminal_input_cursor_request_t request;
                    if (frame->rdx || !syscall_user_buffer_valid(
                        (void *)(uintptr_t)frame->rsi, sizeof(request))) {
                        result = MG_ERR_BAD_ARGUMENT; break;
                    }
                    memcpy(&request, (void *)(uintptr_t)frame->rsi, sizeof(request));
                    if (!terminal_input_cursor_for_process(process->pid, request.offset, request.extent))
                        result = MG_ERR_BAD_ARGUMENT;
                    break;
                }
                case MG_TERMINAL_OP_SESSION_HISTORY_RESET:
                    if (frame->rsi || frame->rdx) {
                        result = MG_ERR_BAD_ARGUMENT;
                    } else if (!syscall_is_logind(process)) {
                        result = MG_ERR_PRIVILEGE_REQUIRED;
                    } else if (!terminal_reset_session_history_authorized()) {
                        result = MG_ERR_BUSY;
                    }
                    break;
                case MG_TERMINAL_OP_PREPARE_INPUT_LINE:
                    if (frame->rsi || frame->rdx ||
                        !terminal_prepare_input_line_for_process(process->pid))
                        result = MG_ERR_ACCESS_DENIED;
                    break;
                case MG_TERMINAL_OP_READ_KEY: {
                    mg_terminal_key_request_t request;
                    mg_terminal_key_result_t key_result;
                    u32 key;
                    if (!frame->rsi || !frame->rdx ||
                        !syscall_user_buffer_valid(
                            (const void *)(uintptr_t)frame->rsi,
                            sizeof(request)) ||
                        !syscall_user_buffer_writable(
                            (void *)(uintptr_t)frame->rdx,
                            sizeof(key_result))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    memcpy(&request, (const void *)(uintptr_t)frame->rsi,
                           sizeof(request));
                    if (request.version != MG_TERMINAL_API_VERSION) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    scheduler_syscall_enter();
                    result = terminal_read_key_for_process(process->pid,
                                               request.timeout_ms,
                                               &key);
                    scheduler_syscall_leave();
                    if (result == MG_OK) {
                        key_result.version = MG_TERMINAL_API_VERSION;
                        key_result.key = key;
                        memcpy((void *)(uintptr_t)frame->rdx, &key_result,
                               sizeof(key_result));
                    }
                    break;
                }
                case MG_TERMINAL_OP_READ_KEY_EVENT: {
                    mg_terminal_key_request_t request;
                    terminal_key_event_t event;
                    if (!frame->rsi || !frame->rdx ||
                        !syscall_user_buffer_valid(
                            (const void *)(uintptr_t)frame->rsi,
                            sizeof(request)) ||
                        !syscall_user_buffer_writable(
                            (void *)(uintptr_t)frame->rdx, sizeof(event))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    memcpy(&request, (const void *)(uintptr_t)frame->rsi,
                           sizeof(request));
                    if (request.version != MG_TERMINAL_API_VERSION) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    scheduler_syscall_enter();
                    result = terminal_read_key_event_for_process(
                        process->pid, request.timeout_ms, &event);
                    scheduler_syscall_leave();
                    if (result == MG_OK)
                        memcpy((void *)(uintptr_t)frame->rdx, &event,
                               sizeof(event));
                    break;
                }
                case MG_TERMINAL_OP_UPDATE_BEGIN:
                    if (frame->rsi || frame->rdx ||
                        !terminal_begin_batch_for_process(process->pid))
                        result = MG_ERR_ACCESS_DENIED;
                    break;
                case MG_TERMINAL_OP_UPDATE_END:
                    if (frame->rsi || frame->rdx ||
                        !terminal_end_batch_for_process(process->pid))
                        result = MG_ERR_ACCESS_DENIED;
                    break;
                case MG_TERMINAL_OP_INPUT_MODE: {
                    mg_terminal_input_mode_request_t request;
                    if (!frame->rsi || frame->rdx ||
                        !syscall_user_buffer_valid(
                            (const void *)(uintptr_t)frame->rsi,
                            sizeof(request))) {
                        result = MG_ERR_BAD_ARGUMENT;
                        break;
                    }
                    memcpy(&request, (const void *)(uintptr_t)frame->rsi,
                           sizeof(request));
                    if (request.version != MG_TERMINAL_API_VERSION ||
                        request.enabled > 1U)
                        result = MG_ERR_BAD_ARGUMENT;
                    else
                        result = terminal_set_raw_input_for_process(
                            process->pid, request.enabled != 0U);
                    break;
                }
                default:
                    result = MG_ERR_UNSUPPORTED;
                    break;
            }
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_UPTIME_MS:
            frame->rax = timer_uptime_ms();
            return;
        case SYSCALL_CLOCK_MONOTONIC: {
            mg_monotonic_time_t value;
            mg_monotonic_time_t *user_value =
                (mg_monotonic_time_t *)(uintptr_t)frame->rdi;

            if (!user_value || !syscall_user_buffer_writable(user_value,
                                                            sizeof(value))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            value.milliseconds = timekeeping_monotonic_ms();
            memcpy(user_value, &value, sizeof(value));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_CLOCK_MONOTONIC_INFO: {
            mg_monotonic_info_t value;
            mg_monotonic_info_t *user_value =
                (mg_monotonic_info_t *)(uintptr_t)frame->rdi;

            if (!user_value || !syscall_user_buffer_writable(user_value,
                                                            sizeof(value))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            timer_monotonic_info(&value);
            memcpy(user_value, &value, sizeof(value));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_RANDOM_STATUS: {
            mg_result_t result = csprng_status();
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_RANDOM_BYTES:
            syscall_random_bytes(frame);
            return;
        case SYSCALL_CLOCK_REALTIME: {
            mg_mangrove_time_t value;
            mg_mangrove_time_t *user_value =
                (mg_mangrove_time_t *)(uintptr_t)frame->rdi;

            if (!user_value || !syscall_user_buffer_writable(user_value,
                                                            sizeof(value))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!timekeeping_realtime(&value)) {
                syscall_fail(frame, MG_ERR_TIME_UNAVAILABLE);
                return;
            }
            memcpy(user_value, &value, sizeof(value));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_CLOCK_REALTIME_INFO: {
            mg_realtime_info_t value;
            mg_realtime_info_t *user_value =
                (mg_realtime_info_t *)(uintptr_t)frame->rdi;

            if (!user_value || !syscall_user_buffer_writable(user_value,
                                                            sizeof(value))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!timekeeping_realtime_info(&value)) {
                syscall_fail(frame, MG_ERR_TIME_UNAVAILABLE);
                return;
            }
            memcpy(user_value, &value, sizeof(value));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_CLOCK_BOOT_ID:
            frame->rax = timekeeping_boot_id();
            return;
        case SYSCALL_CLOCK_PROCESS_CPU: {
            process_t *process = process_current();
            mg_process_cpu_time_t value;
            mg_process_cpu_time_t *user_value =
                (mg_process_cpu_time_t *)(uintptr_t)frame->rdi;

            if (!process || !user_value ||
                !syscall_user_buffer_writable(user_value, sizeof(value))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            value.ticks = __atomic_load_n(&process->cpu_time_ticks,
                                          __ATOMIC_RELAXED);
            memcpy(user_value, &value, sizeof(value));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_SLEEP_MS: {
            process_t *process = process_current();
            bool slept;

            if (process_take_interrupt(process)) {
                syscall_fail(frame, MG_ERR_INTERRUPTED);
                return;
            }
            scheduler_syscall_enter();
            slept = scheduler_sleep(frame->rdi);
            scheduler_syscall_leave();
            if (process_take_interrupt(process)) {
                syscall_fail(frame, MG_ERR_INTERRUPTED);
                return;
            }
            if (!slept) {
                syscall_fail(frame, MG_ERR_BUSY);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_NETWORK:
            scheduler_syscall_enter();
            syscall_network(process_current(), frame);
            scheduler_syscall_leave();
            return;
        case SYSCALL_POWER_OFF:
            frame->rax = (u64)platform_poweroff();
            return;
        case SYSCALL_REBOOT:
            frame->rax = (u64)platform_reboot();
            return;
        case SYSCALL_POWER_STATUS: {
            mg_power_status_t *status =
                (mg_power_status_t *)(uintptr_t)frame->rdi;
            if (!status || !syscall_user_buffer_writable(status, sizeof(*status))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            frame->rax = (u64)platform_power_status(status);
            return;
        }
        case SYSCALL_GET_IDENTITY: {
            process_credentials_t credentials;
            mg_identity_t account;
            mg_identity_t *identity =
                (mg_identity_t *)(uintptr_t)frame->rdi;

            if (!identity || !syscall_user_buffer_writable(identity,
                                                         sizeof(*identity))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!process_get_credentials(process_current(), &credentials)) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            if (!identity_query_credentials(&credentials, &account)) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            *identity = account;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_ACCOUNT: {
            mg_account_request_t request;
            char username[MG_IDENTITY_USERNAME_CAPACITY];
            char password[IDENTITY_PASSWORD_MAX_LENGTH + 1U] = {0};
            mg_account_info_t *result_buffer;
            usize *out_count;
            mg_result_t result;

            if (!syscall_user_buffer_valid(
                    (const void *)(uintptr_t)frame->rdi, sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, (const void *)(uintptr_t)frame->rdi,
                   sizeof(request));
            if (request.operation != MG_ACCOUNT_OP_LIST &&
                (!request.username || !syscall_copy_text(request.username,
                                                         username,
                                                         sizeof(username)))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result_buffer = request.result;
            out_count = request.out_count;
            switch (request.operation) {
                case MG_ACCOUNT_OP_LIST:
                    if (request.result_capacity > MG_ACCOUNT_MAX_RECORDS ||
                        !out_count || !syscall_user_buffer_writable(
                            out_count, sizeof(*out_count)) ||
                        (request.result_capacity && (!result_buffer ||
                            !syscall_user_buffer_writable(result_buffer,
                                request.result_capacity * sizeof(*result_buffer))))) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    result = identity_account_list(result_buffer,
                                                   request.result_capacity,
                                                   out_count);
                    frame->rax = (u64)result;
                    return;
                case MG_ACCOUNT_OP_SHOW:
                    if (!result_buffer || request.result_capacity != 1U ||
                        !syscall_user_buffer_writable(result_buffer,
                                                   sizeof(*result_buffer))) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    result = identity_account_show(username, result_buffer);
                    frame->rax = (u64)result;
                    return;
                case MG_ACCOUNT_OP_CREATE:
                    if (request.flags || request.role || request.result ||
                        request.out_count || !request.password ||
                        !syscall_copy_text(request.password, password,
                                           sizeof(password))) {
                        password_secure_clear(password, sizeof(password));
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    result = identity_account_create(username, password);
                    password_secure_clear(password, sizeof(password));
                    frame->rax = (u64)result;
                    return;
                case MG_ACCOUNT_OP_REMOVE:
                    if (request.flags & ~MG_ACCOUNT_REMOVE_PURGE ||
                        request.role || request.password || request.result ||
                        request.out_count) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    frame->rax = (u64)identity_account_remove(
                        username,
                        (request.flags & MG_ACCOUNT_REMOVE_PURGE) != 0U);
                    return;
                case MG_ACCOUNT_OP_SET_ROLE:
                    if (request.flags || request.role > MG_IDENTITY_ROLE_ADMIN ||
                        request.password || request.result || request.out_count) {
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    frame->rax = (u64)identity_account_set_role(
                        username, request.role);
                    return;
                case MG_ACCOUNT_OP_SET_PASSWORD:
                    if (request.flags || request.role || request.result ||
                        request.out_count || !request.password ||
                        !syscall_copy_text(request.password, password,
                                           sizeof(password))) {
                        password_secure_clear(password, sizeof(password));
                        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                        return;
                    }
                    result = identity_account_set_password(username, password);
                    password_secure_clear(password, sizeof(password));
                    frame->rax = (u64)result;
                    return;
                default:
                    syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                    return;
            }
        }
        case SYSCALL_PASS_AUTHENTICATE: {
            char username[MG_IDENTITY_USERNAME_CAPACITY];
            char password[IDENTITY_PASSWORD_MAX_LENGTH + 1U];
            user_identity_t identity;
            mg_identity_t *user_identity =
                (mg_identity_t *)(uintptr_t)frame->rdx;
            int result;

            if (!syscall_is_logind(process_current()) || !user_identity ||
                !syscall_user_buffer_writable(user_identity, sizeof(*user_identity)) ||
                !syscall_copy_text((const char *)(uintptr_t)frame->rdi,
                                   username, sizeof(username)) ||
                !syscall_copy_text((const char *)(uintptr_t)frame->rsi,
                                   password, sizeof(password))) {
                password_secure_clear(password, sizeof(password));
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = pass_authenticate_account(username, password, &identity);
            password_secure_clear(password, sizeof(password));
            if (result == MG_OK) syscall_copy_identity(&identity, user_identity);
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_SESSION_AUTOLOGIN_IDENTITY: {
            mg_identity_t identity;
            mg_identity_t *user_identity =
                (mg_identity_t *)(uintptr_t)frame->rdi;
            int result;

            if (!user_identity || !syscall_user_buffer_writable(
                    user_identity, sizeof(*user_identity))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = session_autologin_identity_process(process_current(),
                                                        &identity);
            if (result == MG_OK) *user_identity = identity;
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_SESSION_CREATE: {
            char username[MG_IDENTITY_USERNAME_CAPACITY];
            mg_session_info_t session;
            mg_session_info_t *user_session =
                (mg_session_info_t *)(uintptr_t)frame->rdx;
            int result;

            if (!user_session || !syscall_user_buffer_writable(
                    user_session, sizeof(*user_session)) ||
                !syscall_copy_text((const char *)(uintptr_t)frame->rsi,
                                   username, sizeof(username))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = session_create_process(process_current(),
                                            (process_handle_t)frame->rdi,
                                            username, &session);
            if (result == MG_OK) *user_session = session;
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_SESSION_LAUNCH: {
            mg_handle_t *user_shell =
                (mg_handle_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_shell || !syscall_user_buffer_writable(user_shell,
                                                            sizeof(*user_shell))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = session_launch_process(process_current(),
                                            (mg_session_id_t)frame->rdi,
                                            user_shell);
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_SESSION_QUERY: {
            mg_session_status_t *status =
                (mg_session_status_t *)(uintptr_t)frame->rsi;
            int result;

            if (!status || !syscall_user_buffer_writable(status, sizeof(*status))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = session_query_process(process_current(),
                                           (mg_session_id_t)frame->rdi,
                                           status);
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_SESSION_LIST: {
            mg_session_list_request_t request;
            mg_session_list_request_t *user_request =
                (mg_session_list_request_t *)(uintptr_t)frame->rdi;
            u32 count = 0;
            u32 total = 0;
            int result;

            if (!user_request || !syscall_user_buffer_valid(user_request,
                                                              sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.result || !request.out_count || !request.out_total ||
                request.capacity == 0U || request.capacity > 8U ||
                !syscall_user_buffer_writable(request.result,
                    (u64)request.capacity * sizeof(*request.result)) ||
                !syscall_user_buffer_writable(request.out_count,
                                            sizeof(*request.out_count)) ||
                !syscall_user_buffer_writable(request.out_total,
                                            sizeof(*request.out_total))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = session_list_process(process_current(), request.offset,
                                          request.result, request.capacity,
                                          &count, &total);
            if (result == MG_OK) {
                *request.out_count = count;
                *request.out_total = total;
            }
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_SESSION_END:
            frame->rax = (u64)session_end_process(
                process_current(), (mg_session_id_t)frame->rdi);
            return;
        case SYSCALL_SERVICE_START: {
            mg_handle_t handle;
            mg_handle_t *user_handle = (mg_handle_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_handle || !syscall_user_buffer_writable(user_handle,
                                                            sizeof(*user_handle))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = process_spawn_service_result(process_current(),
                                                  (u32)frame->rdi, &handle);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            *user_handle = handle;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_SERVICE_REGISTER: {
            char name[MG_IPC_SERVICE_NAME_MAX];
            mg_handle_t handle;
            mg_handle_t *user_handle = (mg_handle_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_handle || !syscall_user_buffer_writable(user_handle,
                                                            sizeof(*user_handle)) ||
                !syscall_copy_text((const char *)(uintptr_t)frame->rdi,
                                   name, sizeof(name))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = ipc_service_register(process_current(), name, &handle);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            *user_handle = handle;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_SERVICE_LOOKUP: {
            char name[MG_IPC_SERVICE_NAME_MAX];
            mg_handle_t handle;
            mg_handle_t *user_handle = (mg_handle_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_handle || !syscall_user_buffer_writable(user_handle,
                                                            sizeof(*user_handle)) ||
                !syscall_copy_text((const char *)(uintptr_t)frame->rdi,
                                   name, sizeof(name))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = ipc_service_lookup(process_current(), name, &handle);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            *user_handle = handle;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_IPC_REQUEST: {
            mg_ipc_message_t request;
            mg_ipc_message_t reply;
            const mg_ipc_message_t *user_request =
                (const mg_ipc_message_t *)(uintptr_t)frame->rsi;
            mg_ipc_message_t *user_reply =
                (mg_ipc_message_t *)(uintptr_t)frame->rdx;
            int result;

            if (!user_request || !user_reply ||
                !syscall_user_buffer_valid(user_request, sizeof(request)) ||
                !syscall_user_buffer_writable(user_reply, sizeof(reply))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            scheduler_syscall_enter();
            result = ipc_kernel_request(process_current(),
                                        (process_handle_t)frame->rdi,
                                        &request, &reply);
            scheduler_syscall_leave();
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            memcpy(user_reply, &reply, sizeof(reply));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_IPC_RECEIVE: {
            mg_ipc_received_t received;
            mg_ipc_received_t *user_received =
                (mg_ipc_received_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_received || !syscall_user_buffer_writable(user_received,
                                                              sizeof(received))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            scheduler_syscall_enter();
            result = ipc_kernel_receive(process_current(),
                                        (process_handle_t)frame->rdi,
                                        &received);
            scheduler_syscall_leave();
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            memcpy(user_received, &received, sizeof(received));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_IPC_RECEIVE_TIMED: {
            mg_ipc_received_t received;
            mg_ipc_received_t *user_received =
                (mg_ipc_received_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_received || !syscall_user_buffer_writable(user_received,
                                                              sizeof(received))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            scheduler_syscall_enter();
            result = ipc_kernel_receive_timed(
                process_current(), (process_handle_t)frame->rdi,
                &received, (u32)frame->rdx);
            scheduler_syscall_leave();
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            memcpy(user_received, &received, sizeof(received));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_IPC_REPLY: {
            mg_ipc_message_t message;
            const mg_ipc_message_t *user_message =
                (const mg_ipc_message_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_message || !syscall_user_buffer_valid(user_message,
                                                             sizeof(message))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&message, user_message, sizeof(message));
            scheduler_syscall_enter();
            result = ipc_kernel_reply(process_current(),
                                      (process_handle_t)frame->rdi, &message);
            scheduler_syscall_leave();
            frame->rax = (u64)result;
            return;
        }
        case SYSCALL_IPC_TRY_RECEIVE: {
            mg_ipc_received_t received;
            mg_ipc_received_t *user_received =
                (mg_ipc_received_t *)(uintptr_t)frame->rsi;
            int result;

            if (!user_received || !syscall_user_buffer_writable(user_received,
                                                              sizeof(received))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = ipc_kernel_try_receive(process_current(),
                                            (process_handle_t)frame->rdi,
                                            &received);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            memcpy(user_received, &received, sizeof(received));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_IPC_EVENT_SUBSCRIBE: {
            int result = ipc_kernel_event_subscribe(
                process_current(), (process_handle_t)frame->rdi,
                (u32)frame->rsi);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_IPC_EVENT_UNSUBSCRIBE: {
            int result = ipc_kernel_event_unsubscribe(
                process_current(), (process_handle_t)frame->rdi);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_PROCESS_POLL: {
            i32 status;
            i32 *user_status = (i32 *)(uintptr_t)frame->rsi;
            int result;

            if (!user_status || !syscall_user_buffer_writable(user_status,
                                                            sizeof(*user_status))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = process_poll(process_current(),
                                  (process_handle_t)frame->rdi, &status);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            *user_status = status;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_SERVICE_STOP: {
            process_t *process = process_current();
            int result;

            if (!process || !process->system_service ||
                process->service_id != MG_SERVICE_SPROUT) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            result = process_terminate_child(
                process, (process_handle_t)frame->rdi, (i32)frame->rsi);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_PROCESS_HANDLE_PID: {
            u64 pid = process_handle_pid(
                process_current(), (process_handle_t)frame->rdi);
            if (!pid) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            frame->rax = pid;
            return;
        }
        case SYSCALL_SERVICE_AUTHORIZE: {
            int result = service_authorize_control(
                process_current(), (process_handle_t)frame->rdi,
                (u32)frame->rsi, (u32)frame->rdx);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_NETWORK_AUTHORIZE: {
            int result = syscall_authorize_network_request(
                process_current(), (process_handle_t)frame->rdi,
                (u32)frame->rsi);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_DEVICE_SNAPSHOT: {
            mg_device_snapshot_request_t request;
            mg_device_snapshot_request_t *user_request =
                (mg_device_snapshot_request_t *)(uintptr_t)frame->rdi;
            u32 total = 0;
            u64 generation = 0;
            mg_result_t result;

            if (!user_request || !syscall_user_buffer_valid(user_request,
                                                              sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (request.reserved || request.result_capacity == 0U ||
                request.result_capacity > MG_DEVICE_RESPONSE_MAX ||
                !request.result || !request.out_total ||
                !syscall_user_buffer_writable(request.result,
                    (u64)request.result_capacity * sizeof(*request.result)) ||
                !syscall_user_buffer_writable(request.out_total,
                                            sizeof(*request.out_total)) ||
                (request.out_snapshot_generation &&
                 !syscall_user_buffer_writable(request.out_snapshot_generation,
                                            sizeof(*request.out_snapshot_generation))) ||
                (request.snapshot_generation &&
                 !request.out_snapshot_generation)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = device_snapshot_read(request.filter, request.device_id,
                                          request.offset,
                                          request.snapshot_generation,
                                          request.result,
                                          request.result_capacity, &total,
                                          &generation);
            memcpy(request.out_total, &total, sizeof(total));
            if (request.out_snapshot_generation)
                memcpy(request.out_snapshot_generation, &generation,
                       sizeof(generation));
            if (result < 0) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = result;
            return;
        }
        case SYSCALL_VOLUME_MOUNT: {
            mg_volume_mount_request_t request;
            mg_volume_mount_request_t *user_request =
                (mg_volume_mount_request_t *)(uintptr_t)frame->rdi;
            block_device_t *device;
            vfs_node_t *parent = NULL;
            vfs_node_t *existing = NULL;
            char name[VFS_MOUNT_PATH_MAX];
            const char *filesystem;
            int lookup_result;
            int mount_result;

            if (!syscall_is_volumed(process_current()) || !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved ||
                (request.flags & ~MG_VOLUME_MOUNT_FLAG_READ_ONLY) ||
                !syscall_fixed_text_valid(request.filesystem,
                                          sizeof(request.filesystem), false) ||
                !syscall_volume_path_valid(request.mount_point, name,
                                            sizeof(name)) ||
                (strcmp(request.filesystem, "mgfs") != 0 &&
                 strcmp(request.filesystem, "fat32") != 0 &&
                 strcmp(request.filesystem, "exfat") != 0)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!syscall_volume_device_allowed(device,
                                               request.parent_instance_id)) {
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            filesystem = vfs_probe_filesystem(device);
            if (!filesystem || strcmp(filesystem, request.filesystem) != 0) {
                syscall_fail(frame, MG_ERR_IO);
                return;
            }
            if (vfs_mount_point_for_device(device, name, sizeof(name), NULL)) {
                syscall_fail(frame, MG_ERR_ALREADY_EXISTS);
                return;
            }
            lookup_result = vfs_lookup_trusted(request.mount_point, &existing);
            if (lookup_result == VFS_OK && existing) {
                syscall_fail(frame, MG_ERR_ALREADY_EXISTS);
                return;
            }
            if (lookup_result != VFS_ERR_NOT_FOUND) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            lookup_result = vfs_lookup_trusted("/vol", &parent);
            if (lookup_result != VFS_OK || !parent ||
                parent->type != VFS_TYPE_DIRECTORY) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            mount_result = vfs_mount_path(
                parent, request.mount_point + 5U, request.mount_point,
                request.filesystem, device, VFS_MOUNT_ROLE_VOLUME,
                (request.flags & MG_VOLUME_MOUNT_FLAG_READ_ONLY) != 0U);
            if (mount_result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(mount_result));
                return;
            }
            (void)block_device_set_automount_suppressed(device->id, false);
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_VOLUME_UNMOUNT: {
            mg_volume_unmount_request_t request;
            mg_volume_unmount_request_t *user_request =
                (mg_volume_unmount_request_t *)(uintptr_t)frame->rdi;
            block_device_t *device;
            block_device_t *devices[VFS_MAX_MOUNTS];
            u32 device_count = 0;
            int result;

            if (!syscall_is_volumed(process_current()) || !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved ||
                (request.flags & ~(MG_VOLUME_UNMOUNT_FLAG_ALL_CHILDREN |
                                   MG_VOLUME_UNMOUNT_FLAG_PREFLIGHT))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!syscall_volume_device_allowed(device,
                                               request.parent_instance_id)) {
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            if (request.flags & MG_VOLUME_UNMOUNT_FLAG_ALL_CHILDREN) {
                if (device->type != BLOCK_DEVICE_USB ||
                    request.parent_instance_id != 0ULL) {
                    syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                    return;
                }
                device_count = syscall_volume_collect_children(
                    device, devices, VFS_MAX_MOUNTS);
                if (!device_count) {
                    syscall_fail(frame, MG_ERR_NOT_FOUND);
                    return;
                }
                result = (request.flags & MG_VOLUME_UNMOUNT_FLAG_PREFLIGHT)
                    ? vfs_unmount_devices_preflight(devices, device_count)
                    : vfs_unmount_devices(devices, device_count);
            } else if (request.flags & MG_VOLUME_UNMOUNT_FLAG_PREFLIGHT) {
                result = vfs_unmount_device_preflight(device);
            } else {
                result = vfs_unmount_device(device);
            }
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_VOLUME_SUPPRESS: {
            mg_volume_suppression_request_t request;
            mg_volume_suppression_request_t *user_request =
                (mg_volume_suppression_request_t *)(uintptr_t)frame->rdi;
            block_device_t *device;

            if (!syscall_is_volumed(process_current()) || !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved ||
                request.suppressed > 1U) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!syscall_volume_device_allowed(device,
                                               request.parent_instance_id)) {
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            if (!block_device_set_automount_suppressed(
                    device->id, request.suppressed != 0U)) {
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_VOLUME_AUTHORIZE: {
            i64 result = syscall_authorize_volume_request(
                process_current(), (process_handle_t)frame->rdi,
                (u32)frame->rsi);
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_AUTHORIZE: {
            process_t *requester = process_current();
            u32 operation = (u32)frame->rdi;
            if (!syscall_is_diskutil(requester) ||
                !requester->storage_management_session ||
                (operation != MG_STORAGE_OP_FORMAT &&
                 operation != MG_STORAGE_OP_LABEL &&
                 operation != MG_STORAGE_OP_GPT_INITIALIZE &&
                 operation != MG_STORAGE_OP_GPT_CREATE &&
                 operation != MG_STORAGE_OP_GPT_DELETE)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            requester->storage_authorized_operation = operation;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_AUTHORIZE_CANCEL: {
            process_t *requester = process_current();
            if (!requester) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            requester->storage_authorized_operation = 0U;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_SESSION_AUTHORIZE: {
            process_t *requester = process_current();
            i64 result;

            if (!syscall_is_diskutil(requester) ||
                requester->storage_management_session) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            result = pass_authorize_current(
                IDENTITY_PRIVILEGE_MANAGE_STORAGE,
                "Open disk administration.");
            if (result != MG_OK) {
                syscall_fail(frame, result);
                return;
            }
            requester->storage_management_session = true;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_FORMAT: {
            mg_storage_format_request_t request;
            mg_storage_format_request_t *user_request =
                (mg_storage_format_request_t *)(uintptr_t)frame->rdi;
            process_t *requester = process_current();
            block_device_t *device;
            const char *filesystem;
            int result;

            if (!syscall_is_diskutil(requester) ||
                !requester->storage_management_session ||
                requester->storage_authorized_operation != MG_STORAGE_OP_FORMAT ||
                !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved ||
                (request.filesystem != MG_STORAGE_FILESYSTEM_FAT32 &&
                 request.filesystem != MG_STORAGE_FILESYSTEM_MGFS &&
                 request.filesystem != MG_STORAGE_FILESYSTEM_EXFAT)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!syscall_storage_device_matches(device,
                                                request.parent_instance_id)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            if (syscall_storage_system_managed(device)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            filesystem = request.filesystem == MG_STORAGE_FILESYSTEM_FAT32
                ? "fat32" : request.filesystem == MG_STORAGE_FILESYSTEM_MGFS
                ? "mgfs" : "exfat";
            requester->storage_authorized_operation = 0U;
            result = storage_format_filesystem_for_id(device,
                request.instance_id - BLOCK_DEVICE_ID_BASE - 1ULL, filesystem);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            /* Formatting intentionally leaves the exact current instance
             * suppressed.  It must stay unmounted until the user explicitly
             * mounts it or physically reinserts it as a fresh instance. */
            (void)block_device_set_automount_suppressed(
                request.instance_id - BLOCK_DEVICE_ID_BASE - 1ULL, true);
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_LABEL: {
            mg_storage_label_request_t request;
            mg_storage_label_request_t *user_request =
                (mg_storage_label_request_t *)(uintptr_t)frame->rdi;
            process_t *requester = process_current();
            block_device_t *device;
            int result;

            if (!syscall_is_diskutil(requester) ||
                !requester->storage_management_session ||
                requester->storage_authorized_operation != MG_STORAGE_OP_LABEL ||
                !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved || request.reserved2 ||
                !syscall_fixed_text_valid(request.filesystem,
                                          sizeof(request.filesystem), false) ||
                !syscall_fixed_text_valid(request.label, sizeof(request.label),
                                          true) ||
                (strcmp(request.filesystem, "fat32") != 0 &&
                 strcmp(request.filesystem, "mgfs") != 0 &&
                 strcmp(request.filesystem, "exfat") != 0)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!syscall_storage_device_matches(device,
                                                request.parent_instance_id)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            if (syscall_storage_system_managed(device)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            requester->storage_authorized_operation = 0U;
            result = storage_set_filesystem_label_for_id(
                device, request.instance_id - BLOCK_DEVICE_ID_BASE - 1ULL,
                request.filesystem, request.label);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_GPT_INFO: {
            mg_storage_gpt_info_request_t request;
            mg_storage_gpt_info_t *user_info =
                (mg_storage_gpt_info_t *)(uintptr_t)frame->rsi;
            mg_storage_gpt_info_request_t *user_request =
                (mg_storage_gpt_info_request_t *)(uintptr_t)frame->rdi;
            block_device_t *device;
            gpt_table_info_t table;
            int result;

            if (!user_request || !user_info ||
                !syscall_user_buffer_valid(user_request, sizeof(request)) ||
                !syscall_user_buffer_writable(user_info, sizeof(*user_info))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved || request.reserved2) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!device || device->type == BLOCK_DEVICE_PARTITION ||
                !block_device_is_live(device)) {
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            result = gpt_get_table_info(device, &table);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            memset(user_info, 0, sizeof(*user_info));
            user_info->table_type = (u32)table.type;
            user_info->partition_count = table.partition_count;
            user_info->entry_count = table.entry_count;
            user_info->entry_size = table.entry_size;
            user_info->first_usable_lba = table.first_usable_lba;
            user_info->last_usable_lba = table.last_usable_lba;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_GPT_PLAN: {
            mg_storage_gpt_plan_request_t request;
            mg_storage_gpt_plan_t *user_plan =
                (mg_storage_gpt_plan_t *)(uintptr_t)frame->rsi;
            mg_storage_gpt_plan_request_t *user_request =
                (mg_storage_gpt_plan_request_t *)(uintptr_t)frame->rdi;
            block_device_t *device;
            gpt_partition_plan_t plan;
            int result;

            if (!user_request || !user_plan ||
                !syscall_user_buffer_valid(user_request, sizeof(request)) ||
                !syscall_user_buffer_writable(user_plan, sizeof(*user_plan))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved ||
                (request.flags & ~MG_STORAGE_GPT_PLAN_FLAG_REST) ||
                ((request.flags & MG_STORAGE_GPT_PLAN_FLAG_REST) &&
                 request.requested_bytes) ||
                (!(request.flags & MG_STORAGE_GPT_PLAN_FLAG_REST) &&
                 !request.requested_bytes)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!device || device->type == BLOCK_DEVICE_PARTITION ||
                !block_device_is_live(device)) {
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            result = gpt_plan_partition(
                device, request.requested_bytes,
                (request.flags & MG_STORAGE_GPT_PLAN_FLAG_REST) != 0U,
                &plan);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            user_plan->first_lba = plan.first_lba;
            user_plan->last_lba = plan.last_lba;
            user_plan->size_bytes = plan.size_bytes;
            user_plan->partition_number = plan.partition_number;
            user_plan->reserved = 0;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_GPT_INITIALIZE: {
            process_t *requester = process_current();
            block_device_t *device;
            int result;

            if (!syscall_is_diskutil(requester) ||
                !requester->storage_management_session ||
                requester->storage_authorized_operation !=
                    MG_STORAGE_OP_GPT_INITIALIZE) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            device = block_get_device_by_public_id(frame->rdi);
            if (!device || device->type == BLOCK_DEVICE_PARTITION) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (syscall_storage_system_managed(device)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            requester->storage_authorized_operation = 0U;
            result = gpt_initialize_device_for_id(device,
                frame->rdi - BLOCK_DEVICE_ID_BASE - 1ULL);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_GPT_CREATE: {
            mg_storage_gpt_create_request_t request;
            mg_storage_gpt_create_request_t *user_request =
                (mg_storage_gpt_create_request_t *)(uintptr_t)frame->rdi;
            process_t *requester = process_current();
            block_device_t *device;
            int result;

            if (!syscall_is_diskutil(requester) ||
                !requester->storage_management_session ||
                requester->storage_authorized_operation !=
                    MG_STORAGE_OP_GPT_CREATE || !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || request.reserved || !request.partition_number ||
                request.partition_number > GPT_MAX_PARTITION_ENTRIES ||
                request.first_lba > request.last_lba) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            if (!device || device->type == BLOCK_DEVICE_PARTITION) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (syscall_storage_system_managed(device)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            requester->storage_authorized_operation = 0U;
            result = gpt_create_partition_for_id(device,
                request.instance_id - BLOCK_DEVICE_ID_BASE - 1ULL, request.partition_number,
                                          request.first_lba, request.last_lba);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_STORAGE_GPT_DELETE: {
            mg_storage_gpt_delete_request_t request;
            mg_storage_gpt_delete_request_t *user_request =
                (mg_storage_gpt_delete_request_t *)(uintptr_t)frame->rdi;
            process_t *requester = process_current();
            block_device_t *device;
            block_device_t *partition;
            int result;

            if (!syscall_is_diskutil(requester) ||
                !requester->storage_management_session ||
                requester->storage_authorized_operation != MG_STORAGE_OP_GPT_DELETE ||
                !user_request ||
                !syscall_user_buffer_valid(user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_PRIVILEGE_REQUIRED);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.instance_id || !request.partition_instance_id ||
                request.reserved || !request.partition_number ||
                request.partition_number > GPT_MAX_PARTITION_ENTRIES) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            device = block_get_device_by_public_id(request.instance_id);
            partition = block_get_device_by_public_id(request.partition_instance_id);
            if (!device || device->type == BLOCK_DEVICE_PARTITION || !partition ||
                partition->type != BLOCK_DEVICE_PARTITION) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_DEVICE_GONE);
                return;
            }
            if (syscall_storage_system_managed(device) ||
                syscall_storage_system_managed(partition)) {
                requester->storage_authorized_operation = 0U;
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            requester->storage_authorized_operation = 0U;
            result = gpt_delete_partition_for_id(device,
                request.instance_id - BLOCK_DEVICE_ID_BASE - 1ULL, partition,
                request.partition_instance_id - BLOCK_DEVICE_ID_BASE - 1ULL,
                request.partition_number);
            if (result != VFS_OK) {
                syscall_fail(frame, syscall_vfs_error(result));
                return;
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_RECOVERY_OWNERS:
            frame->rax = (u64)syscall_recovery_owners(process_current(),
                (const mg_recovery_owner_snapshot_request_t *)(uintptr_t)frame->rdi);
            return;
        case SYSCALL_SYSTEM_SHUTDOWN_PENDING: {
            bool *user_pending = (bool *)(uintptr_t)frame->rdi;
            bool pending;
            if (!user_pending || !syscall_user_buffer_writable(
                    user_pending, sizeof(*user_pending))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            pending = process_shutdown_pending();
            memcpy(user_pending, &pending, sizeof(pending));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_PROCESS_SNAPSHOT: {
            mg_process_snapshot_request_t request;
            mg_process_snapshot_request_t *user_request =
                (mg_process_snapshot_request_t *)(uintptr_t)frame->rdi;
            u32 total = 0;
            u32 copied;

            if (!user_request || !syscall_user_buffer_valid(user_request,
                                                              sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.result || !request.out_count || !request.out_total ||
                request.result_capacity == 0U ||
                request.result_capacity > MG_PROCESS_SNAPSHOT_PAGE_MAX ||
                !syscall_user_buffer_writable(request.result,
                    (u64)request.result_capacity * sizeof(*request.result)) ||
                !syscall_user_buffer_writable(request.out_count,
                                            sizeof(*request.out_count)) ||
                !syscall_user_buffer_writable(request.out_total,
                                            sizeof(*request.out_total))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            copied = process_snapshot_read(request.offset, request.result,
                                           request.result_capacity, &total);
            memcpy(request.out_count, &copied, sizeof(copied));
            memcpy(request.out_total, &total, sizeof(total));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_MEMORY_INFO: {
            mg_system_memory_info_t *info =
                (mg_system_memory_info_t *)(uintptr_t)frame->rdi;

            if (!info || !syscall_user_buffer_writable(info, sizeof(*info))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            info->physical_total_bytes = pmm_get_total_memory();
            info->physical_used_bytes = pmm_get_used_memory();
            info->physical_free_bytes = pmm_get_free_memory();
            info->kernel_heap_total_bytes = heap_get_total_size();
            info->kernel_heap_used_bytes = heap_get_used_size();
            info->kernel_heap_free_bytes = heap_get_free_size();
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_CPU_SNAPSHOT: {
            mg_cpu_snapshot_request_t request;
            mg_cpu_snapshot_request_t *user_request =
                (mg_cpu_snapshot_request_t *)(uintptr_t)frame->rdi;
            u32 total = 0;
            u32 copied;

            if (!user_request || !syscall_user_buffer_valid(user_request,
                                                              sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (!request.result || !request.out_count || !request.out_total ||
                request.result_capacity == 0U ||
                request.result_capacity > MG_CPU_SNAPSHOT_PAGE_MAX ||
                !syscall_user_buffer_writable(request.result,
                    (u64)request.result_capacity * sizeof(*request.result)) ||
                !syscall_user_buffer_writable(request.out_count,
                                            sizeof(*request.out_count)) ||
                !syscall_user_buffer_writable(request.out_total,
                                            sizeof(*request.out_total))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            copied = cpu_snapshot_read(request.offset, request.result,
                                       request.result_capacity, &total);
            memcpy(request.out_count, &copied, sizeof(copied));
            memcpy(request.out_total, &total, sizeof(total));
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_SYSTEM_INFO: {
            mg_system_info_t *info =
                (mg_system_info_t *)(uintptr_t)frame->rdi;

            if (!info || !syscall_user_buffer_writable(info, sizeof(*info))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            system_info_read(info);
            frame->rax = MG_OK;
            return;
        }
#ifdef HEAP_PROFILE_DIAGNOSTICS
        case SYSCALL_HEAP_PROFILE_DIAG: {
            heap_profile_diag_request_t request;
            const heap_profile_diag_request_t *user_request =
                (const heap_profile_diag_request_t *)(uintptr_t)frame->rdi;

            if (!user_request || !syscall_user_buffer_valid(
                    user_request, sizeof(request))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            memcpy(&request, user_request, sizeof(request));
            if (request.magic != HEAP_PROFILE_DIAG_MAGIC) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (request.operation == HEAP_PROFILE_OP_MARK_PHASE) {
                if (request.phase > 0xffffffffULL ||
                    request.snapshot_address || request.snapshot_bytes ||
                    !heap_profile_mark_phase((u32)request.phase)) {
                    syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                    return;
                }
                frame->rax = MG_OK;
                return;
            }
            if (request.operation == HEAP_PROFILE_OP_FREEZE) {
                if (request.phase || request.snapshot_address ||
                    request.snapshot_bytes) {
                    syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                    return;
                }
                heap_profile_freeze();
                frame->rax = MG_OK;
                return;
            }
            if (request.operation == HEAP_PROFILE_OP_READ) {
                void *destination =
                    (void *)(uintptr_t)request.snapshot_address;
                if (request.snapshot_bytes == 0U ||
                    request.snapshot_bytes >
                        HEAP_PROFILE_EXPORT_CHUNK_BYTES ||
                    request.phase >= heap_profile_total_bytes() ||
                    request.snapshot_bytes >
                        heap_profile_total_bytes() -
                            (usize)request.phase || !destination ||
                    !syscall_user_buffer_writable(destination,
                        (usize)request.snapshot_bytes) ||
                    !heap_profile_read(destination, (usize)request.phase,
                        (usize)request.snapshot_bytes)) {
                    syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                    return;
                }
                frame->rax = MG_OK;
                return;
            }
            syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
            return;
        }
#endif
        case SYSCALL_PROCESS_SESSION_ID: {
            mg_session_id_t *user_session_id =
                (mg_session_id_t *)(uintptr_t)frame->rdi;
            process_t *current = process_current();

            if (!user_session_id || !current ||
                !syscall_user_buffer_writable(user_session_id,
                                            sizeof(*user_session_id))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            *user_session_id = current->session_id;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_PROCESS_CURRENT_PID: {
            u64 pid = process_current_pid();

            if (!pid) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            frame->rax = pid;
            return;
        }
        case SYSCALL_YIELD:
            frame->rax = scheduler_yield() ? (u64)MG_OK : (u64)MG_ERR_BUSY;
            return;
        case SYSCALL_EXIT:
            /* The current process has one userspace thread in this phase.
             * Mark it terminated, then hand execution to the scheduler.
             * Force-end any active terminal batch so output is not
             * permanently suppressed if the process dies mid-batch. */
            terminal_force_end_batch();
            if (!process_exit(process_current(), (i32)frame->rdi)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            frame->rax = scheduler_terminate() ? (u64)MG_OK :
                                                  (u64)MG_ERR_BUSY;
            return;
        default:
            syscall_fail(frame, MG_ERR_UNSUPPORTED);
            return;
    }
}

void syscall_dispatch(void *raw_frame)
{
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;
    kernel_thread_t *thread = thread_current();
    process_t *process;
    syscall_user_access_context_t context = {0};
    void *previous_context;
    bool tracked;

    if (!frame) return;
    if (!thread || !thread->process) {
        syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
        return;
    }
    process = thread->process;
    tracked = frame->rax != SYSCALL_EXIT;
    if (tracked && !process_syscall_enter(process)) {
        syscall_fail(frame, MG_ERR_BUSY);
        return;
    }
    previous_context = thread->syscall_user_access_context;
    thread->syscall_user_access_context = &context;
    if (tracked) scheduler_syscall_enter();
    syscall_dispatch_inner(frame);
    syscall_user_lease_release_all(&context);
    thread->syscall_user_access_context = previous_context;
    if (tracked) scheduler_syscall_leave();
    if (tracked) process_syscall_leave(process);
    if (context.allocation_failed && (i64)frame->rax == MG_ERR_BAD_ARGUMENT)
        syscall_fail(frame, MG_ERR_NO_MEMORY);
}
