#pragma once

#include <types.h>

#define KERNEL_BOOT_LOG_CAPACITY 65536U

void kprint(const char *fmt, ...);
void kprint_debug(const char *fmt, ...);
void kprint_debug_screen(const char *fmt, ...);

usize kprint_boot_log_size(void);
usize kprint_boot_log_read(usize offset, char *buffer, usize capacity);
void kprint_boot_log_stop(void);

#ifndef KERNEL_BOOT_DEBUG
#define KERNEL_BOOT_DEBUG 0
#endif

/* Detailed bring-up output is intentionally separate from production boot
 * status.  Keep the call sites visible so a debug build can still expose the
 * low-level initialization path without making normal boot noisy. */
#define KERNEL_BOOT_DEBUG_LOG(...) kprint_debug(__VA_ARGS__)
