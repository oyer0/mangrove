#pragma once

#include <types.h>

enum syscall_number {
    /* 0 remains reserved from the pre-handle validation ABI. */
    SYSCALL_YIELD = 1,
    SYSCALL_EXIT = 2,
    SYSCALL_OPEN = 3,
    SYSCALL_READ = 4,
    SYSCALL_WRITE = 5,
    SYSCALL_CLOSE = 6,
    SYSCALL_SPAWN = 7,
    SYSCALL_WAIT = 8,
    SYSCALL_CHDIR = 9,
    SYSCALL_MEMORY_MAP = 10,
    SYSCALL_MEMORY_UNMAP = 11,
    SYSCALL_GETCWD = 12,
    SYSCALL_PATH_INFO = 13,
    SYSCALL_DIRECTORY_OPEN = 14,
    SYSCALL_DIRECTORY_READ = 15,
    SYSCALL_FILE_CREATE = 16,
    SYSCALL_DIRECTORY_CREATE = 17,
    SYSCALL_PATH_MOVE = 18,
    SYSCALL_PATH_REMOVE = 19,
    SYSCALL_FILE_TRUNCATE = 20,
    SYSCALL_CONSOLE_TRANSACTION = 21,
    SYSCALL_UPTIME_MS = 22,
    SYSCALL_NETWORK = 23,
    SYSCALL_POWER_OFF = 24,
    SYSCALL_REBOOT = 25,
    SYSCALL_POWER_STATUS = 26,
};

void syscall_init(void);
void syscall_dispatch(void *frame);
