#include <mangrove.h>

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

mg_result_t process_wait(mg_handle_t handle, i32 *status)
{
    return (mg_result_t)mg_syscall(8, handle, (unsigned long)status, 0);
}

mg_result_t process_chdir(const char *path)
{
    return (mg_result_t)mg_syscall(9, (unsigned long)path, 0, 0);
}

mg_result_t process_yield(void)
{
    return (mg_result_t)mg_syscall(1, 0, 0, 0);
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
