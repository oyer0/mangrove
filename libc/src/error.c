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
    case MG_ERR_CERTIFICATE_INVALID: return EACCES;
    case MG_ERR_CERTIFICATE_SIGNATURE: return EACCES;
    case MG_ERR_TLS_PROTOCOL: return EPROTO;
    case MG_ERR_TRUST_STORE_INVALID: return EPROTO;
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
    case MG_ERR_SHUTDOWN: return ECANCELED;
    case MG_ERR_INTERRUPTED: return EINTR;
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
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted system call";
    case EIO: return "Input/output error";
    case ENXIO: return "No such device or address";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Cannot allocate memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Cross-device link";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENFILE: return "Too many open files in system";
    case EMFILE: return "Too many open files";
    case ENOTTY: return "Inappropriate ioctl for device";
    case ETXTBSY: return "Text file busy";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EMLINK: return "Too many links";
    case EPIPE: return "Broken pipe";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Result out of range";
    case ENOSYS: return "Function not implemented";
    case ENAMETOOLONG: return "File name too long";
    case ENOLCK: return "No locks available";
    case ENOTEMPTY: return "Directory not empty";
    case ELOOP: return "Too many symbolic links encountered";
    case ENOMSG: return "No message of desired type";
    case EIDRM: return "Identifier removed";
    case EOVERFLOW: return "Value too large for data type";
    case EILSEQ: return "Invalid or incomplete multibyte sequence";
    case EPROTO: return "Protocol error";
    case ETIMEDOUT: return "Operation timed out";
    case ECANCELED: return "Operation canceled";
    case ECONNRESET: return "Connection reset";
    case ECONNREFUSED: return "Connection refused";
    case EADDRINUSE: return "Address already in use";
    case ENETUNREACH: return "Network is unreachable";
    case ENETDOWN: return "Network is down";
    case ENOTCONN: return "Transport endpoint is not connected";
    case ENODATA: return "No data available";
    case ENOTSUP: return "Operation not supported";
    default: return "Unknown error";
    }
}
