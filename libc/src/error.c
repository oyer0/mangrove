/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <mg/error.h>

int mg_error_to_errno(mg_result_t result)
{
    switch (result) {
    case MG_OK: return 0;
    case MG_ERR_NOT_FOUND: return ENOENT;
    case MG_ERR_INVALID_HANDLE: return EBADF;
    case MG_ERR_BAD_ARGUMENT: return EINVAL;
    case MG_ERR_NOT_DIRECTORY: return ENOTDIR;
    case MG_ERR_NO_MEMORY: return ENOMEM;
    case MG_ERR_UNSUPPORTED: return ENOSYS;
    case MG_ERR_ACCESS_DENIED: return EACCES;
    case MG_ERR_ALREADY_EXISTS: return EEXIST;
    case MG_ERR_BUFFER_TOO_SMALL: return ERANGE;
    case MG_ERR_END_OF_FILE: return EIO; /* Handle EOF before errno mapping. */
    case MG_ERR_IO: return EIO;
    case MG_ERR_NOT_CHILD: return ECHILD;
    case MG_ERR_BUSY: return EBUSY;
    case MG_ERR_INVALID_EXEC: return ENOEXEC;
    case MG_ERR_NOT_EMPTY: return ENOTEMPTY;
    case MG_ERR_NETWORK_UNAVAILABLE: return ENETDOWN;
    case MG_ERR_TIMEOUT: return ETIMEDOUT;
    case MG_ERR_CONNECTION_RESET: return ECONNRESET;
    case MG_ERR_CONNECTION_CLOSED: return ECONNRESET;
    case MG_ERR_WOULD_BLOCK: return EAGAIN;
    case MG_ERR_ADDRESS_IN_USE: return EADDRINUSE;
    case MG_ERR_AUTH_FAILED: return EACCES;
    case MG_ERR_ENTROPY_UNAVAILABLE: return EIO;
    case MG_ERR_PRIVILEGE_REQUIRED: return EPERM;
    case MG_ERR_CANCELLED: return ECANCELED;
    case MG_ERR_SERVICE_UNAVAILABLE: return ENOSYS;
    case MG_ERR_QUEUE_FULL: return EAGAIN;
    case MG_ERR_PROTOCOL: return EPROTO;
    case MG_ERR_TIME_UNAVAILABLE: return ENODATA;
    case MG_ERR_DEVICE_GONE: return ENODEV;
    case MG_ERR_RETRY: return EAGAIN;
    case MG_ERR_UNCERTAIN: return EIO;
    case MG_ERR_STALE_GENERATION: return EAGAIN;
    case MG_ERR_REPLACEMENT_TOO_LARGE: return EFBIG;
    case MG_ERR_NO_SPACE: return ENOSPC;
    case MG_ERR_NAME_TOO_LONG: return ENAMETOOLONG;
    default: return EIO;
    }
}

void mg_set_errno_from_result(mg_result_t result)
{
    if (result < 0) errno = mg_error_to_errno(result);
}

char *strerror(int error_number)
{
    switch (error_number) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EINTR: return "Interrupted system call";
    case EIO: return "Input/output error";
    case EBADF: return "Bad file descriptor";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Cannot allocate memory";
    case EACCES: return "Permission denied";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EINVAL: return "Invalid argument";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case EROFS: return "Read-only file system";
    case EPIPE: return "Broken pipe";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Result out of range";
    case ENOSYS: return "Function not implemented";
    case ENAMETOOLONG: return "File name too long";
    case ENOTEMPTY: return "Directory not empty";
    case EOVERFLOW: return "Value too large for data type";
    case EILSEQ: return "Invalid or incomplete multibyte sequence";
    case ETIMEDOUT: return "Operation timed out";
    case ECANCELED: return "Operation canceled";
    default: return "Unknown error";
    }
}
