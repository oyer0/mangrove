#include <syscall.h>
#include <msr.h>
#include <scheduler.h>
#include <process.h>
#include <heap.h>
#include <vmm.h>
#include <terminal.h>
#include <mangrove_errors.h>
#include <mg/filesystem.h>
#include <mg/net.h>
#include <mg/power.h>
#include <mg/identity.h>
#include <net/user.h>
#include <string.h>
#include <kprint.h>
#include <timer.h>
#include <platform_power.h>
#include <identity.h>
#include <authorization.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define MSR_EFER  0xC0000080U
#define MSR_STAR  0xC0000081U
#define MSR_LSTAR 0xC0000082U
#define MSR_FMASK 0xC0000084U
#define EFER_SCE  (1ULL << 0)

typedef struct syscall_frame {
    u64 r15, r14, r13, r12, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rbx, rax;
    u64 rcx, r11;
} syscall_frame_t;

extern void syscall_entry(void);
static void syscall_fail(syscall_frame_t *frame, i64 error);

void syscall_init(void)
{
    u64 efer = rdmsr(MSR_EFER);

    /* STAR selects kernel CS=0x08 and SYSRET user CS=0x3B/data SS=0x33. */
    wrmsr(MSR_STAR, (0x2BULL << 48) | (0x08ULL << 32));
    wrmsr(MSR_LSTAR, (u64)(uintptr_t)&syscall_entry);
    /* Mask IF, TF and DF while executing the entry path. */
    wrmsr(MSR_FMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10));
    wrmsr(MSR_EFER, efer | EFER_SCE);
}

static bool syscall_user_buffer_valid(const void *buffer, u64 length)
{
    uintptr_t start = (uintptr_t)buffer;
    if (length == 0) return true;
    return vmm_user_range_valid((const void *)start, (usize)length);
}

static bool syscall_copy_path(const char *user_path, char *path, usize size)
{
    usize i;
    if (!user_path || !path || size < 2) return false;
    for (i = 0; i < size; i++) {
        if (!vmm_user_range_valid(user_path + i, 1)) return false;
        path[i] = user_path[i];
        if (path[i] == '\0') return i != 0;
    }
    return false;
}

static bool syscall_copy_text(const char *user_text, char *text, usize size)
{
    usize i;
    if (!user_text || !text || size < 2) return false;
    for (i = 0; i < size; i++) {
        if (!vmm_user_range_valid(user_text + i, 1)) return false;
        text[i] = user_text[i];
        if (!text[i]) return i != 0;
    }
    return false;
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

static i64 syscall_confirm_network_change(const char *description)
{
    return authorization_confirm_current(IDENTITY_PRIVILEGE_MANAGE_NETWORK,
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
                !syscall_user_buffer_valid(request.result, sizeof(mg_net_info_t))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_info((mg_net_info_t *)request.result); return;
        case MG_NET_OP_RESOLVE_A: {
            char hostname[256];
            if (!request.buffer || !request.result || request.result_capacity < sizeof(mg_ipv4_addr_t) ||
                !syscall_copy_text((const char *)request.buffer, hostname, sizeof(hostname)) ||
                !syscall_user_buffer_valid(request.result, sizeof(mg_ipv4_addr_t))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_resolve_a(hostname, (mg_ipv4_addr_t *)request.result,
                                                 request.timeout_ms);
            return;
        }
        case MG_NET_OP_INTERFACES:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_valid(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_interfaces((mg_net_interface_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_ROUTES:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_valid(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_routes((mg_net_route_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_NEIGHBORS:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_valid(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_neighbors((mg_net_neighbor_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_CONNECTIONS:
            if (request.result && request.result_capacity &&
                !syscall_user_buffer_valid(request.result, request.result_capacity)) { syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return; }
            frame->rax = (u64)net_user_connections((mg_net_connection_info_t *)request.result, request.result_capacity); return;
        case MG_NET_OP_RENEW:
            frame->rax = (u64)net_user_renew(request.timeout_ms); return;
        case MG_NET_OP_SET_MANUAL: {
            mg_net_manual_config_t configuration;
            i64 authorization_result;
            if (!request.buffer || request.buffer_length < sizeof(configuration) ||
                !syscall_user_buffer_valid(request.buffer, sizeof(configuration))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT); return;
            }
            memcpy(&configuration, request.buffer, sizeof(configuration));
            authorization_result = syscall_confirm_network_change(
                "Change the active network configuration.");
            if (authorization_result != MG_OK) {
                frame->rax = (u64)authorization_result;
                return;
            }
            frame->rax = (u64)net_user_set_manual(&configuration); return;
        }
        case MG_NET_OP_SET_AUTOMATIC: {
            i64 authorization_result = syscall_confirm_network_change(
                "Return the active network configuration to DHCP.");
            if (authorization_result != MG_OK) {
                frame->rax = (u64)authorization_result;
                return;
            }
            frame->rax = (u64)net_user_set_automatic(request.timeout_ms); return;
        }
        case MG_NET_OP_RELOAD: {
            i64 authorization_result = syscall_confirm_network_change(
                "Reload the machine network configuration.");
            if (authorization_result != MG_OK) {
                frame->rax = (u64)authorization_result;
                return;
            }
            frame->rax = (u64)net_user_reload(); return;
        }
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
                !syscall_user_buffer_valid(request.result, sizeof(mg_icmp_echo_result_t)) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_icmp_echo(object, &request.endpoint.address,
                                                  request.buffer, request.buffer_length,
                                                  request.timeout_ms,
                                                  (mg_icmp_echo_result_t *)request.result);
            return;
        case MG_NET_OP_DATAGRAM_SEND:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_DATAGRAM,
                                           OBJECT_RIGHT_WRITE);
            if (!object || (!request.buffer && request.buffer_length) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_datagram_send(object, &request.endpoint,
                                                      request.buffer, request.buffer_length,
                                                      request.timeout_ms); return;
        case MG_NET_OP_DATAGRAM_RECEIVE:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_DATAGRAM,
                                           OBJECT_RIGHT_READ);
            if (!object || !request.buffer || !request.result ||
                request.result_capacity < sizeof(mg_datagram_result_t) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length) ||
                !syscall_user_buffer_valid(request.result, sizeof(mg_datagram_result_t))) {
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_datagram_receive(object, (void *)request.buffer,
                                                         request.buffer_length, request.timeout_ms,
                                                         (mg_datagram_result_t *)request.result); return;
        case MG_NET_OP_STREAM_SEND:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_STREAM,
                                           OBJECT_RIGHT_WRITE);
            if (!object || (!request.buffer && request.buffer_length) ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_stream_send(object, request.buffer,
                                                    request.buffer_length, request.timeout_ms); return;
        case MG_NET_OP_STREAM_RECEIVE:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_STREAM,
                                           OBJECT_RIGHT_READ);
            if (!object || !request.buffer || !request.buffer_length ||
                !syscall_user_buffer_valid(request.buffer, request.buffer_length)) {
                syscall_fail(frame, !object ? MG_ERR_INVALID_HANDLE : MG_ERR_BAD_ARGUMENT); return;
            }
            frame->rax = (u64)net_user_stream_receive(object, (void *)request.buffer,
                                                       request.buffer_length, request.timeout_ms); return;
        case MG_NET_OP_STREAM_CLOSE:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_STREAM,
                                           OBJECT_RIGHT_WRITE);
            if (!object) { syscall_fail(frame, MG_ERR_INVALID_HANDLE); return; }
            result = net_user_stream_close(object);
            if (result == MG_OK) (void)process_handle_close(process, request.handle);
            frame->rax = (u64)result; return;
        case MG_NET_OP_ICMP_CLOSE:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_ICMP,
                                           OBJECT_RIGHT_WRITE);
            if (!object) { syscall_fail(frame, MG_ERR_INVALID_HANDLE); return; }
            frame->rax = process_handle_close(process, request.handle) ? MG_OK : MG_ERR_INVALID_HANDLE;
            return;
        case MG_NET_OP_DATAGRAM_CLOSE:
            object = process_handle_lookup(process, request.handle, OBJECT_TYPE_NETWORK_DATAGRAM,
                                           OBJECT_RIGHT_WRITE);
            if (!object) { syscall_fail(frame, MG_ERR_INVALID_HANDLE); return; }
            frame->rax = process_handle_close(process, request.handle) ? MG_OK : MG_ERR_INVALID_HANDLE;
            return;
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
        case VFS_ERR_ACCESS_DENIED: return MG_ERR_ACCESS_DENIED;
        case VFS_ERR_NOT_DIRECTORY: return MG_ERR_NOT_DIRECTORY;
        default: return MG_ERR_BAD_ARGUMENT;
    }
}

static bool syscall_resolve_path(process_t *process, const char *user_path,
                                 char *resolved, usize resolved_size)
{
    char path[256];

    return syscall_copy_path(user_path, path, sizeof(path)) &&
           process_resolve_path(process, path, resolved, resolved_size);
}

static bool syscall_resolve_parent(process_t *process, const char *user_path,
                                   char *parent, usize parent_size,
                                   char *name, usize name_size)
{
    char path[256];

    return syscall_copy_path(user_path, path, sizeof(path)) &&
           process_split_path(process, path, parent, parent_size,
                              name, name_size);
}

static i64 syscall_create_path(process_t *process, const char *user_path,
                               bool directory)
{
    char parent_path[512];
    char name[256];
    vfs_node_t *parent = NULL;
    vfs_node_t *created = NULL;
    int result;
    int lookup_result;

    if (!syscall_resolve_parent(process, user_path, parent_path,
                                sizeof(parent_path), name, sizeof(name))) {
        return MG_ERR_BAD_ARGUMENT;
    }
    lookup_result = vfs_lookup(parent_path, &parent);
    if (lookup_result != VFS_OK || !parent) {
        return syscall_vfs_error(lookup_result);
    }
    if (parent->type != VFS_TYPE_DIRECTORY) return MG_ERR_NOT_DIRECTORY;
    if (!vfs_check_access(parent, VFS_ACCESS_WRITE)) {
        return MG_ERR_ACCESS_DENIED;
    }
    if (vfs_finddir(parent, name)) return MG_ERR_ALREADY_EXISTS;
    result = directory ? vfs_mkdir(parent, name, &created) :
                         vfs_create(parent, name, &created);
    return syscall_vfs_error(result);
}

void syscall_dispatch(void *raw_frame)
{
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;

    if (!frame) return;

    switch (frame->rax) {
        case SYSCALL_OPEN: {
            char path[256];
            char resolved[512];
            vfs_node_t *node = NULL;
            int lookup_result;
            u32 flags = (u32)frame->rsi;
            u32 rights;
            process_handle_t handle;
            kernel_object_t *object;
            if (!syscall_copy_path((const char *)(uintptr_t)frame->rdi,
                                   path, sizeof(path))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!process_resolve_path(process_current(), path,
                                      resolved, sizeof(resolved))) {
                syscall_fail(frame, MG_ERR_NOT_FOUND);
                return;
            }
            if (flags != VFS_OPEN_READ && flags != VFS_OPEN_WRITE &&
                flags != VFS_OPEN_RDWR) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            lookup_result = vfs_lookup(resolved, &node);
            if (lookup_result != VFS_OK || !node) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            if (node->type != VFS_TYPE_FILE) {
                syscall_fail(frame, MG_ERR_NOT_FOUND);
                return;
            }
            if (((flags & VFS_OPEN_READ) &&
                 !vfs_check_access(node, VFS_ACCESS_READ)) ||
                ((flags & VFS_OPEN_WRITE) &&
                 !vfs_check_access(node, VFS_ACCESS_WRITE))) {
                syscall_fail(frame, MG_ERR_ACCESS_DENIED);
                return;
            }
            object = object_file_create_node(node, flags);
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
        case SYSCALL_WRITE: {
            process_handle_t handle = (process_handle_t)frame->rdi;
            void *buffer = (void *)(uintptr_t)frame->rsi;
            u64 length = frame->rdx;
            kernel_object_t *object = process_handle_lookup(
                process_current(), handle, OBJECT_TYPE_INVALID,
                frame->rax == SYSCALL_READ ? OBJECT_RIGHT_READ : OBJECT_RIGHT_WRITE);
            i64 result;
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            if (!syscall_user_buffer_valid(buffer, length)) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = frame->rax == SYSCALL_READ
                ? object_read(object, buffer, length)
                : object_write(object, buffer, length);
            frame->rax = result < 0 ? (u64)MG_ERR_UNSUPPORTED : (u64)result;
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
            bool spawned = process_spawn(process_current(), cmdline, &handle);
            if (!spawned) {
                syscall_fail(frame, MG_ERR_INVALID_EXEC);
                return;
            }
            frame->rax = handle;
            return;
        }
        case SYSCALL_WAIT: {
            process_handle_t handle = (process_handle_t)frame->rdi;
            i32 status;
            i32 *user_status = (i32 *)(uintptr_t)frame->rsi;
            if (!syscall_user_buffer_valid(user_status, sizeof(*user_status))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!process_wait(process_current(), handle, &status)) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            *user_status = status;
            frame->rax = 0;
            return;
        }
        case SYSCALL_CHDIR: {
            char path[256];
            int result;
            if (!syscall_copy_path((const char *)(uintptr_t)frame->rdi,
                                   path, sizeof(path))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = process_chdir_result(process_current(), path);
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

            if (!syscall_user_buffer_valid(user_address,
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
            usize required;

            if (!process || !buffer || capacity == 0 || !user_size ||
                !syscall_user_buffer_valid(buffer, capacity) ||
                !syscall_user_buffer_valid(user_size, sizeof(*user_size))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            required = strlen(process->cwd) + 1;
            *user_size = required;
            if (capacity < required) {
                syscall_fail(frame, MG_ERR_BUFFER_TOO_SMALL);
                return;
            }
            memcpy(buffer, process->cwd, required);
            frame->rax = required - 1;
            return;
        }
        case SYSCALL_PATH_INFO: {
            char resolved[512];
            vfs_node_t *node = NULL;
            mg_path_info_t *info = (mg_path_info_t *)(uintptr_t)frame->rsi;
            int lookup_result;

            if (!info || !syscall_user_buffer_valid(info, sizeof(*info))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (!syscall_resolve_path(process_current(),
                                      (const char *)(uintptr_t)frame->rdi,
                                      resolved, sizeof(resolved))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            lookup_result = vfs_lookup(resolved, &node);
            if (lookup_result != VFS_OK || !node) {
                syscall_fail(frame, syscall_vfs_error(lookup_result));
                return;
            }
            info->type = node->type == VFS_TYPE_DIRECTORY
                ? MG_PATH_TYPE_DIRECTORY : MG_PATH_TYPE_FILE;
            info->permissions = node->permissions;
            info->size = node->size;
            info->identifier = node->inode;
            info->owner_uid = node->owner_uid;
            info->reserved = 0;
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_DIRECTORY_OPEN: {
            char resolved[512];
            vfs_node_t *node = NULL;
            int lookup_result;
            process_handle_t handle;
            kernel_object_t *object;

            if (!syscall_resolve_path(process_current(),
                                      (const char *)(uintptr_t)frame->rdi,
                                      resolved, sizeof(resolved))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            lookup_result = vfs_lookup(resolved, &node);
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
                !syscall_user_buffer_valid(user_entry, sizeof(*user_entry))) {
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
            if (result < 0) {
                syscall_fail(frame, MG_ERR_UNSUPPORTED);
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
        case SYSCALL_DIRECTORY_READ_BATCH: {
            mg_directory_entry_t *user_entries =
                (mg_directory_entry_t *)(uintptr_t)frame->rsi;
            kernel_object_t *object;
            vfs_dirent_t *entries;
            u64 capacity = frame->rdx;
            i64 result;

            if (!user_entries || capacity == 0 ||
                capacity > VFS_DIRECTORY_BATCH_MAX ||
                !syscall_user_buffer_valid(user_entries,
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
                syscall_fail(frame, MG_ERR_NO_MEMORY);
                return;
            }
            result = object_directory_read_batch(object, entries,
                                                 (u32)capacity);
            if (result < 0) {
                kfree(entries);
                syscall_fail(frame, MG_ERR_UNSUPPORTED);
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
            char source_parent_path[512], destination_parent_path[512];
            char source_name[256], destination_name[256];
            vfs_node_t *source_parent = NULL, *destination_parent = NULL;
            int result;

            if (!syscall_resolve_parent(process_current(),
                                        (const char *)(uintptr_t)frame->rdi,
                                        source_parent_path,
                                        sizeof(source_parent_path), source_name,
                                        sizeof(source_name)) ||
                !syscall_resolve_parent(process_current(),
                                        (const char *)(uintptr_t)frame->rsi,
                                        destination_parent_path,
                                        sizeof(destination_parent_path),
                                        destination_name,
                                        sizeof(destination_name))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            int source_lookup = vfs_lookup(source_parent_path, &source_parent);
            int destination_lookup = vfs_lookup(destination_parent_path,
                                                &destination_parent);
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
            if (!vfs_finddir(source_parent, source_name)) {
                syscall_fail(frame, MG_ERR_NOT_FOUND);
                return;
            }
            if (vfs_finddir(destination_parent, destination_name)) {
                syscall_fail(frame, MG_ERR_ALREADY_EXISTS);
                return;
            }
            if (source_parent->super != destination_parent->super) {
                syscall_fail(frame, MG_ERR_UNSUPPORTED);
                return;
            }
            result = vfs_rename(source_parent, source_name, destination_parent,
                                destination_name);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }
        case SYSCALL_PATH_REMOVE: {
            char parent_path[512], name[256];
            vfs_node_t *parent = NULL, *node;
            int result;
            int lookup_result;

            if (!syscall_resolve_parent(process_current(),
                                        (const char *)(uintptr_t)frame->rdi,
                                        parent_path, sizeof(parent_path), name,
                                        sizeof(name))) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            lookup_result = vfs_lookup(parent_path, &parent);
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
            node = vfs_finddir(parent, name);
            if (!node) {
                syscall_fail(frame, MG_ERR_NOT_FOUND);
                return;
            }
            result = node->type == VFS_TYPE_DIRECTORY
                ? vfs_rmdir(parent, name) : vfs_unlink(parent, name);
            frame->rax = (u64)syscall_vfs_error(result);
            return;
        }
        case SYSCALL_FILE_TRUNCATE: {
            kernel_object_t *object = process_handle_lookup(
                process_current(), (process_handle_t)frame->rdi,
                OBJECT_TYPE_FILE, OBJECT_RIGHT_WRITE);
            if (!object) {
                syscall_fail(frame, MG_ERR_INVALID_HANDLE);
                return;
            }
            frame->rax = (u64)syscall_vfs_error(object_file_truncate(object));
            return;
        }
        case SYSCALL_CONSOLE_TRANSACTION: {
            u64 op = frame->rdi;
            if (op == 1) {
                terminal_begin_batch();
            } else {
                terminal_end_batch();
            }
            frame->rax = MG_OK;
            return;
        }
        case SYSCALL_CONSOLE_INPUT_MODE:
            if (frame->rdi > 1U) {
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            if (frame->rdi)
                terminal_cursor_disable();
            else
                terminal_cursor_enable();
            frame->rax = MG_OK;
            return;
        case SYSCALL_UPTIME_MS:
            frame->rax = timer_uptime_ms();
            return;
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
            if (!status || !syscall_user_buffer_valid(status, sizeof(*status))) {
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

            if (!identity || !syscall_user_buffer_valid(identity,
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
            char password[IDENTITY_PASSWORD_MAX_LENGTH + 1U];
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
                        !out_count || !syscall_user_buffer_valid(
                            out_count, sizeof(*out_count)) ||
                        (request.result_capacity && (!result_buffer ||
                            !syscall_user_buffer_valid(result_buffer,
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
                        !syscall_user_buffer_valid(result_buffer,
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
        case SYSCALL_LOGIN: {
            char username[MG_IDENTITY_USERNAME_CAPACITY];
            char password[IDENTITY_PASSWORD_MAX_LENGTH + 1U];
            mg_result_t result;

            if (!syscall_copy_text((const char *)(uintptr_t)frame->rdi,
                                   username, sizeof(username)) ||
                !syscall_copy_text((const char *)(uintptr_t)frame->rsi,
                                   password, sizeof(password))) {
                password_secure_clear(password, sizeof(password));
                syscall_fail(frame, MG_ERR_BAD_ARGUMENT);
                return;
            }
            result = process_authenticate(process_current(), username,
                                          password);
            password_secure_clear(password, sizeof(password));
            frame->rax = (u64)result;
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
