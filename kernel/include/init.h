#pragma once

#include <types.h>

typedef enum {
    INIT_UNINITIALIZED = 0,
    INIT_INITIALIZING,
    INIT_READY,
    INIT_UNAVAILABLE,
    INIT_FAILED,
    INIT_SKIPPED
} init_state_t;

typedef enum {
    INIT_RESULT_OK = 0,
    INIT_RESULT_UNAVAILABLE,
    INIT_RESULT_FAILED,
    INIT_RESULT_DEPENDENCY
} init_result_t;

/* These are kernel services, not discovered devices. */
typedef enum {
    INIT_PROCESS = 0,
    INIT_SCHEDULER,
    INIT_IRQ_ROUTING,
    INIT_VFS,
    INIT_PCI,
    INIT_AHCI,
    INIT_NETWORK_CORE,
    INIT_NETWORK_DEVICE,
    INIT_NETWORK_PROTOCOLS,
    INIT_FILESYSTEMS,
    INIT_CPU_IRQS_ENABLED,
    INIT_NETWORK_CONFIG,
    INIT_XHCI,
    INIT_ROOTFS,
    INIT_COUNT
} init_id_t;

typedef struct {
    const char *name;
    init_state_t state;
    init_result_t result;
    const char *reason;
} init_status_t;

/* Starts every post-early-boot subsystem in dependency order. */
bool kernel_bringup(void);

init_state_t kernel_init_state(init_id_t id);
const init_status_t *kernel_init_status(init_id_t id);
