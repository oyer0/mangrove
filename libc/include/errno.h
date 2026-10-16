/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

int *__errno_location(void);
#define errno (*__errno_location())

/* Positive userspace errno values are independent of MG_ERR_* results. */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ETXTBSY 26
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENOSYS 38
#define ENAMETOOLONG 36
#define ENOLCK 37
#define ENOTEMPTY 39
#define ELOOP 40
#define ENOMSG 42
#define EIDRM 43
#define EOVERFLOW 75
#define EILSEQ 84
#define EPROTO 71
#define ETIMEDOUT 110
#define ECONNRESET 104
#define ECONNREFUSED 111
#define EADDRINUSE 98
#define ENETUNREACH 101
#define ENETDOWN 100
#define ENOTCONN 107
#define ECANCELED 125
#define ENODATA 61
#define ENOTSUP 95
#define EOPNOTSUPP ENOTSUP
#define EWOULDBLOCK EAGAIN
