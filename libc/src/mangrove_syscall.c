/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/volume_service.h>
#include <mg/storage.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

extern long mg_syscall(unsigned long number, unsigned long arg0,
                       unsigned long arg1, unsigned long arg2);

mg_result_t file_open(const char *path, u32 flags)
{
    return (mg_result_t)mg_syscall(3, (unsigned long)path, flags, 0);
}

u64 uptime_ms(void)
{
    return (u64)mg_syscall(22, 0, 0, 0);
}

mg_result_t system_poweroff(void)
{
    return (mg_result_t)mg_syscall(24, 0, 0, 0);
}

mg_result_t system_reboot(void)
{
    return (mg_result_t)mg_syscall(25, 0, 0, 0);
}

mg_result_t power_status(mg_power_status_t *status)
{
    return (mg_result_t)mg_syscall(26, (unsigned long)status, 0, 0);
}

mg_result_t process_get_identity(mg_identity_t *identity)
{
    return (mg_result_t)mg_syscall(27, (unsigned long)identity, 0, 0);
}

mg_result_t pass_authenticate_account(const char *username,
                                       const char *password,
                                       mg_identity_t *identity)
{
    return (mg_result_t)mg_syscall(55, (unsigned long)username,
                                   (unsigned long)password,
                                   (unsigned long)identity);
}

mg_result_t session_autologin_identity(mg_identity_t *identity)
{
    return (mg_result_t)mg_syscall(56, (unsigned long)identity, 0, 0);
}

mg_result_t session_launch_shell(mg_session_id_t session_id,
                                  mg_handle_t *out_shell)
{
    return (mg_result_t)mg_syscall(58, (unsigned long)session_id,
                                   (unsigned long)out_shell, 0);
}

mg_result_t session_query(mg_session_id_t session_id,
                          mg_session_status_t *status)
{
    return (mg_result_t)mg_syscall(59, (unsigned long)session_id,
                                   (unsigned long)status, 0);
}

mg_result_t session_list(u32 offset, mg_session_status_t *status, u32 capacity,
                         u32 *out_count, u32 *out_total)
{
    mg_session_list_request_t request = {
        offset, capacity, status, out_count, out_total
    };
    return (mg_result_t)mg_syscall(60, (unsigned long)&request, 0, 0);
}

mg_result_t session_create_from_request(mg_handle_t request,
                                         const char *username,
                                         mg_session_info_t *session)
{
    return (mg_result_t)mg_syscall(57, request, (unsigned long)username,
                                   (unsigned long)session);
}

mg_result_t session_end(mg_session_id_t session_id)
{
    return (mg_result_t)mg_syscall(33, (unsigned long)session_id, 0, 0);
}

mg_result_t service_start(mg_service_id_t service_id,
                          mg_handle_t *out_handle)
{
    return (mg_result_t)mg_syscall(34, (unsigned long)service_id,
                                   (unsigned long)out_handle, 0);
}

mg_result_t pass_authorize_service_request(mg_handle_t request, u32 operation,
                                           mg_service_id_t service_id)
{
    return (mg_result_t)mg_syscall(44, request, operation, service_id);
}

mg_result_t pass_authorize_network_request(mg_handle_t request, u32 operation)
{
    return (mg_result_t)mg_syscall(45, request, operation, 0);
}

mg_result_t device_snapshot(mg_device_snapshot_request_t *request)
{
    return (mg_result_t)mg_syscall(46, (unsigned long)request, 0, 0);
}

mg_result_t service_register(const char *name, mg_handle_t *out_endpoint)
{
    return (mg_result_t)mg_syscall(35, (unsigned long)name,
                                   (unsigned long)out_endpoint, 0);
}

mg_result_t service_lookup(const char *name, mg_handle_t *out_endpoint)
{
    return (mg_result_t)mg_syscall(36, (unsigned long)name,
                                   (unsigned long)out_endpoint, 0);
}

mg_result_t ipc_request(mg_handle_t endpoint,
                        const mg_ipc_message_t *request,
                        mg_ipc_message_t *reply)
{
    return (mg_result_t)mg_syscall(37, endpoint, (unsigned long)request,
                                   (unsigned long)reply);
}

mg_result_t ipc_receive(mg_handle_t endpoint, mg_ipc_received_t *received)
{
    return (mg_result_t)mg_syscall(38, endpoint,
                                   (unsigned long)received, 0);
}

mg_result_t ipc_receive_timed(mg_handle_t endpoint,
                              mg_ipc_received_t *received,
                              u32 timeout_ms)
{
    return (mg_result_t)mg_syscall(66, endpoint,
                                   (unsigned long)received, timeout_ms);
}

mg_result_t volume_mount(const mg_volume_mount_request_t *request)
{
    return (mg_result_t)mg_syscall(67, (unsigned long)request, 0, 0);
}

mg_result_t volume_unmount(const mg_volume_unmount_request_t *request)
{
    return (mg_result_t)mg_syscall(68, (unsigned long)request, 0, 0);
}

mg_result_t volume_set_suppressed(
    const mg_volume_suppression_request_t *request)
{
    return (mg_result_t)mg_syscall(69, (unsigned long)request, 0, 0);
}

mg_result_t pass_authorize_volume_request(mg_handle_t request, u32 operation)
{
    return (mg_result_t)mg_syscall(70, request, operation, 0);
}

mg_result_t storage_authorize(u32 operation)
{
    return (mg_result_t)mg_syscall(71, operation, 0, 0);
}

mg_result_t storage_session_authorize(void)
{
    return (mg_result_t)mg_syscall(80, 0, 0, 0);
}

mg_result_t storage_authorize_cancel(void)
{
    return (mg_result_t)mg_syscall(72, 0, 0, 0);
}

mg_result_t storage_format(const mg_storage_format_request_t *request)
{
    return (mg_result_t)mg_syscall(73, (unsigned long)request, 0, 0);
}

mg_result_t storage_set_label(const mg_storage_label_request_t *request)
{
    return (mg_result_t)mg_syscall(74, (unsigned long)request, 0, 0);
}

mg_result_t storage_gpt_info(u64 instance_id, mg_storage_gpt_info_t *info)
{
    mg_storage_gpt_info_request_t request = {instance_id, 0, 0};
    return (mg_result_t)mg_syscall(75, (unsigned long)&request,
                                   (unsigned long)info, 0);
}

mg_result_t storage_gpt_plan(const mg_storage_gpt_plan_request_t *request,
                             mg_storage_gpt_plan_t *plan)
{
    return (mg_result_t)mg_syscall(76, (unsigned long)request,
                                   (unsigned long)plan, 0);
}

mg_result_t storage_gpt_initialize(u64 instance_id)
{
    return (mg_result_t)mg_syscall(77, instance_id, 0, 0);
}

mg_result_t storage_gpt_create(const mg_storage_gpt_create_request_t *request)
{
    return (mg_result_t)mg_syscall(78, (unsigned long)request, 0, 0);
}

mg_result_t storage_gpt_delete(const mg_storage_gpt_delete_request_t *request)
{
    return (mg_result_t)mg_syscall(79, (unsigned long)request, 0, 0);
}

mg_result_t ipc_try_receive(mg_handle_t endpoint, mg_ipc_received_t *received)
{
    return (mg_result_t)mg_syscall(40, endpoint,
                                   (unsigned long)received, 0);
}

mg_result_t ipc_reply(mg_handle_t request, const mg_ipc_message_t *reply)
{
    return (mg_result_t)mg_syscall(39, request, (unsigned long)reply, 0);
}

mg_result_t ipc_event_subscribe(mg_handle_t endpoint, u32 event_classes)
{
    return (mg_result_t)mg_syscall(49, endpoint, event_classes, 0);
}

mg_result_t ipc_event_unsubscribe(mg_handle_t endpoint)
{
    return (mg_result_t)mg_syscall(50, endpoint, 0, 0);
}

mg_result_t account_list(mg_account_info_t *accounts, usize capacity,
                         usize *out_count)
{
    mg_account_request_t request = {0};
    request.operation = MG_ACCOUNT_OP_LIST;
    request.result = accounts;
    request.result_capacity = capacity;
    request.out_count = out_count;
    return (mg_result_t)mg_syscall(28, (unsigned long)&request, 0, 0);
}

mg_result_t account_show(const char *username, mg_account_info_t *account)
{
    mg_account_request_t request = {0};
    request.operation = MG_ACCOUNT_OP_SHOW;
    request.username = username;
    request.result = account;
    request.result_capacity = 1;
    return (mg_result_t)mg_syscall(28, (unsigned long)&request, 0, 0);
}

mg_result_t account_create(const char *username, const char *password)
{
    mg_account_request_t request = {0};
    request.operation = MG_ACCOUNT_OP_CREATE;
    request.username = username;
    request.password = password;
    return (mg_result_t)mg_syscall(28, (unsigned long)&request, 0, 0);
}

mg_result_t account_remove(const char *username, bool purge)
{
    mg_account_request_t request = {0};
    request.operation = MG_ACCOUNT_OP_REMOVE;
    request.flags = purge ? MG_ACCOUNT_REMOVE_PURGE : 0;
    request.username = username;
    return (mg_result_t)mg_syscall(28, (unsigned long)&request, 0, 0);
}

mg_result_t account_set_role(const char *username, mg_identity_role_t role)
{
    mg_account_request_t request = {0};
    request.operation = MG_ACCOUNT_OP_SET_ROLE;
    request.role = role;
    request.username = username;
    return (mg_result_t)mg_syscall(28, (unsigned long)&request, 0, 0);
}

mg_result_t account_set_password(const char *username, const char *password)
{
    mg_account_request_t request = {0};
    request.operation = MG_ACCOUNT_OP_SET_PASSWORD;
    request.username = username;
    request.password = password;
    return (mg_result_t)mg_syscall(28, (unsigned long)&request, 0, 0);
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    return (mg_result_t)mg_syscall(4, handle, (unsigned long)buffer,
                                   length);
}

mg_result_t object_write(mg_handle_t handle, const void *buffer, usize length)
{
    return (mg_result_t)mg_syscall(5, handle, (unsigned long)buffer,
                                   length);
}

mg_result_t handle_close(mg_handle_t handle)
{
    return (mg_result_t)mg_syscall(6, handle, 0, 0);
}

mg_result_t process_spawn(const char *path)
{
    return (mg_result_t)mg_syscall(7, (unsigned long)path, 0, 0);
}

mg_result_t process_spawn_argv(const char *const *argv, usize argc)
{
    if (!argv || argc == 0 || argc > MG_PROCESS_MAX_ARGUMENTS)
        return MG_ERR_BAD_ARGUMENT;
    return (mg_result_t)mg_syscall(64, (unsigned long)argv, argc, 0);
}

mg_result_t process_spawn_with_output(const char *path,
                                      mg_handle_t output_handle)
{
    return (mg_result_t)mg_syscall(51, (unsigned long)path,
                                   output_handle, 0);
}

mg_result_t process_redirect_output(mg_handle_t output_handle,
                                    mg_handle_t *saved_handle)
{
    return (mg_result_t)mg_syscall(53, output_handle,
                                   (unsigned long)saved_handle, 0);
}

mg_result_t process_restore_output(mg_handle_t saved_handle)
{
    return (mg_result_t)mg_syscall(54, saved_handle, 0, 0);
}

mg_result_t process_wait(mg_handle_t handle, i32 *status)
{
    return (mg_result_t)mg_syscall(8, handle, (unsigned long)status, 0);
}

mg_result_t process_poll(mg_handle_t handle, i32 *status)
{
    return (mg_result_t)mg_syscall(41, handle, (unsigned long)status, 0);
}

mg_result_t process_terminate(mg_handle_t handle, i32 status)
{
    return (mg_result_t)mg_syscall(42, handle, (unsigned long)(long)status,
                                   0);
}

u64 process_handle_pid(mg_handle_t handle)
{
    return (u64)mg_syscall(43, handle, 0, 0);
}

mg_result_t process_chdir(const char *path)
{
    return (mg_result_t)mg_syscall(9, (unsigned long)path, 0, 0);
}

mg_result_t process_yield(void)
{
    return (mg_result_t)mg_syscall(1, 0, 0, 0);
}

mg_result_t process_snapshot(mg_process_snapshot_request_t *request)
{
    return (mg_result_t)mg_syscall(47, (unsigned long)request, 0, 0);
}

mg_result_t memory_info(mg_system_memory_info_t *info)
{
    return (mg_result_t)mg_syscall(48, (unsigned long)info, 0, 0);
}

mg_result_t cpu_snapshot(mg_cpu_snapshot_request_t *request)
{
    return (mg_result_t)mg_syscall(81, (unsigned long)request, 0, 0);
}

mg_result_t system_info(mg_system_info_t *info)
{
    return (mg_result_t)mg_syscall(83, (unsigned long)info, 0, 0);
}

mg_result_t memory_map(usize size, void **out_address)
{
    return (mg_result_t)mg_syscall(10, size, (unsigned long)out_address, 0);
}

mg_result_t memory_unmap(void *address)
{
    return (mg_result_t)mg_syscall(11, (unsigned long)address, 0, 0);
}

mg_result_t process_getcwd(char *buffer, usize capacity, usize *out_size)
{
    return (mg_result_t)mg_syscall(12, (unsigned long)buffer, capacity,
                                   (unsigned long)out_size);
}

mg_result_t path_info(const char *path, mg_path_info_t *out_info)
{
    return (mg_result_t)mg_syscall(13, (unsigned long)path,
                                   (unsigned long)out_info, 0);
}

mg_result_t directory_open(const char *path)
{
    return (mg_result_t)mg_syscall(14, (unsigned long)path, 0, 0);
}

mg_result_t directory_read(mg_handle_t handle, mg_directory_entry_t *out_entry)
{
    return (mg_result_t)mg_syscall(15, handle, (unsigned long)out_entry, 0);
}

mg_result_t directory_read_batch(mg_handle_t handle,
                                 mg_directory_entry_t *out_entries,
                                 usize capacity, usize *out_count)
{
    long result;

    if (!out_count) return MG_ERR_BAD_ARGUMENT;
    result = mg_syscall(31, handle, (unsigned long)out_entries, capacity);
    if (result < 0) return (mg_result_t)result;
    *out_count = (usize)result;
    return MG_OK;
}

mg_result_t file_create(const char *path)
{
    return (mg_result_t)mg_syscall(16, (unsigned long)path, 0, 0);
}

mg_result_t directory_create(const char *path)
{
    return (mg_result_t)mg_syscall(17, (unsigned long)path, 0, 0);
}

mg_result_t path_move(const char *source, const char *destination)
{
    return (mg_result_t)mg_syscall(18, (unsigned long)source,
                                   (unsigned long)destination, 0);
}

mg_result_t path_remove(const char *path)
{
    return (mg_result_t)mg_syscall(19, (unsigned long)path, 0, 0);
}

mg_result_t file_truncate(mg_handle_t handle)
{
    return (mg_result_t)mg_syscall(20, handle, 0, 0);
}

mg_result_t file_seek(mg_handle_t handle, i64 offset,
                      mg_seek_whence_t whence)
{
    return (mg_result_t)mg_syscall(52, handle, (unsigned long)offset,
                                   (unsigned long)whence);
}

const char *error_string(mg_result_t error)
{
    switch (error) {
        case MG_OK: return "success";
        case MG_ERR_NOT_FOUND: return "not found";
        case MG_ERR_INVALID_HANDLE: return "invalid handle";
        case MG_ERR_BAD_ARGUMENT: return "bad argument";
        case MG_ERR_NOT_DIRECTORY: return "not a directory";
        case MG_ERR_NO_MEMORY: return "out of memory";
        case MG_ERR_UNSUPPORTED: return "unsupported operation";
        case MG_ERR_ACCESS_DENIED: return "access denied";
        case MG_ERR_ALREADY_EXISTS: return "already exists";
        case MG_ERR_BUFFER_TOO_SMALL: return "buffer too small";
        case MG_ERR_END_OF_FILE: return "end of file";
        case MG_ERR_IO: return "I/O failure";
        case MG_ERR_NOT_CHILD: return "not a child process";
        case MG_ERR_BUSY: return "busy";
        case MG_ERR_INVALID_EXEC: return "invalid executable";
        case MG_ERR_NOT_EMPTY: return "directory is not empty";
        case MG_ERR_NETWORK_UNAVAILABLE: return "network unavailable";
        case MG_ERR_TIMEOUT: return "timed out";
        case MG_ERR_CONNECTION_RESET: return "connection reset";
        case MG_ERR_CONNECTION_CLOSED: return "connection closed";
        case MG_ERR_WOULD_BLOCK: return "would block";
        case MG_ERR_ADDRESS_IN_USE: return "address in use";
        case MG_ERR_AUTH_FAILED: return "authentication failed";
        case MG_ERR_ENTROPY_UNAVAILABLE: return "secure randomness unavailable";
        case MG_ERR_PRIVILEGE_REQUIRED: return "administrator privileges required";
        case MG_ERR_CANCELLED: return "cancelled";
        case MG_ERR_SERVICE_UNAVAILABLE: return "service unavailable";
        case MG_ERR_QUEUE_FULL: return "service queue full";
        case MG_ERR_PROTOCOL: return "invalid IPC protocol";
        case MG_ERR_DEVICE_GONE: return "backing device gone";
        case MG_ERR_RETRY: return "please retry";
        default: return "unknown error";
    }
}

void process_exit(i32 status)
{
    (void)mg_syscall(2, (unsigned long)(long)status, 0, 0);
    for (;;) __asm__ volatile("pause");
}

mg_result_t console_begin_transaction(void)
{
    return (mg_result_t)mg_syscall(21, 1, 0, 0);
}

mg_result_t console_end_transaction(void)
{
    return (mg_result_t)mg_syscall(21, 0, 0, 0);
}

mg_result_t console_set_secure_input(bool secure)
{
    return (mg_result_t)mg_syscall(30, secure ? 1U : 0U, 0, 0);
}

static mg_result_t terminal_control(u32 operation, const void *input,
                                     void *output)
{
    return (mg_result_t)mg_syscall(65, operation,
                                   (unsigned long)input,
                                   (unsigned long)output);
}

mg_result_t terminal_alternate_enter(void)
{
    return terminal_control(MG_TERMINAL_OP_ALTERNATE_ENTER, NULL, NULL);
}

mg_result_t terminal_alternate_leave(void)
{
    return terminal_control(MG_TERMINAL_OP_ALTERNATE_LEAVE, NULL, NULL);
}

mg_result_t terminal_cursor_move(u32 row, u32 column)
{
    mg_terminal_cursor_request_t request = {
        MG_TERMINAL_API_VERSION, row, column
    };
    return terminal_control(MG_TERMINAL_OP_CURSOR_MOVE, &request, NULL);
}

mg_result_t terminal_cursor_set_visible(bool visible)
{
    mg_terminal_visibility_request_t request = {
        MG_TERMINAL_API_VERSION, visible ? 1U : 0U
    };
    return terminal_control(MG_TERMINAL_OP_CURSOR_VISIBILITY,
                            &request, NULL);
}

static mg_result_t terminal_clear_mode(u32 mode, u32 first_row,
                                       u32 first_column, u32 last_row,
                                       u32 last_column)
{
    mg_terminal_clear_request_t request = {
        MG_TERMINAL_API_VERSION, mode, first_row, first_column,
        last_row, last_column
    };
    return terminal_control(MG_TERMINAL_OP_CLEAR, &request, NULL);
}

mg_result_t terminal_clear_line(void)
{
    return terminal_clear_mode(MG_TERMINAL_CLEAR_LINE, 0, 0, 0, 0);
}

mg_result_t terminal_clear_to_end(void)
{
    return terminal_clear_mode(MG_TERMINAL_CLEAR_TO_END, 0, 0, 0, 0);
}

mg_result_t terminal_clear_region(u32 first_row, u32 first_column,
                                  u32 last_row, u32 last_column)
{
    return terminal_clear_mode(MG_TERMINAL_CLEAR_REGION, first_row,
                               first_column, last_row, last_column);
}

mg_result_t terminal_clear_screen(void)
{
    return terminal_clear_mode(MG_TERMINAL_CLEAR_SCREEN, 0, 0, 0, 0);
}

mg_result_t terminal_get_size(mg_terminal_size_t *out_size)
{
    if (!out_size) return MG_ERR_BAD_ARGUMENT;
    out_size->version = MG_TERMINAL_API_VERSION;
    return terminal_control(MG_TERMINAL_OP_SIZE, NULL, out_size);
}

mg_result_t terminal_get_capabilities(
    mg_terminal_capabilities_t *out_capabilities)
{
    if (!out_capabilities) return MG_ERR_BAD_ARGUMENT;
    out_capabilities->version = MG_TERMINAL_API_VERSION;
    return terminal_control(MG_TERMINAL_OP_CAPABILITIES, NULL,
                             out_capabilities);
}

mg_result_t terminal_overlay_set(const mg_terminal_overlay_cell_t *cells,
                                 u32 rows, u32 columns)
{
    mg_terminal_overlay_request_t request = {
        MG_TERMINAL_API_VERSION, rows, columns, 0,
        (u64)rows * (u64)columns
    };

    if (!cells || !rows || !columns) return MG_ERR_BAD_ARGUMENT;
    return terminal_control(MG_TERMINAL_OP_OVERLAY_SET, &request,
                            (void *)cells);
}

mg_result_t terminal_overlay_clear(void)
{
    return terminal_control(MG_TERMINAL_OP_OVERLAY_CLEAR, NULL, NULL);
}

mg_result_t terminal_read_key(u32 timeout_ms, u32 *out_key)
{
    mg_terminal_key_request_t request = {
        MG_TERMINAL_API_VERSION, timeout_ms
    };
    mg_terminal_key_result_t result;
    mg_result_t status;

    if (!out_key) return MG_ERR_BAD_ARGUMENT;
    status = terminal_control(MG_TERMINAL_OP_READ_KEY, &request, &result);
    if (status == MG_OK) *out_key = result.key;
    return status;
}

mg_result_t terminal_input_raw(bool enabled)
{
    mg_terminal_visibility_request_t request = {
        MG_TERMINAL_API_VERSION, enabled ? 1U : 0U
    };
    return terminal_control(MG_TERMINAL_OP_INPUT_MODE, &request, NULL);
}

mg_result_t terminal_update_begin(void)
{
    return terminal_control(MG_TERMINAL_OP_UPDATE_BEGIN, NULL, NULL);
}

mg_result_t terminal_update_end(void)
{
    return terminal_control(MG_TERMINAL_OP_UPDATE_END, NULL, NULL);
}

mg_result_t terminal_write_styled(const void *buffer, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background)
{
    mg_terminal_styled_write_request_t request = {
        MG_TERMINAL_API_VERSION, foreground, background, 0, length
    };

    if (length && !buffer) return MG_ERR_BAD_ARGUMENT;
    if (foreground >= MG_TERMINAL_COLOR_COUNT ||
        background >= MG_TERMINAL_COLOR_COUNT)
        return MG_ERR_BAD_ARGUMENT;
    return terminal_control(MG_TERMINAL_OP_STYLED_WRITE, &request,
                            (void *)buffer);
}

mg_result_t terminal_write_semantic(const void *buffer, usize length,
                                    mg_terminal_style_role_t role)
{
    mg_terminal_semantic_write_request_t request = {
        MG_TERMINAL_API_VERSION, role, 0, length
    };

    if (length && !buffer) return MG_ERR_BAD_ARGUMENT;
    if (role >= MG_TERMINAL_STYLE_COUNT) return MG_ERR_BAD_ARGUMENT;
    return terminal_control(MG_TERMINAL_OP_SEMANTIC_WRITE, &request,
                             (void *)buffer);
}
