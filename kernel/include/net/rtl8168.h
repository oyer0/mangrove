#pragma once

#include <types.h>

typedef enum {
    RTL8168_INIT_ABSENT = 0,
    RTL8168_INIT_READY,
    RTL8168_INIT_UNAVAILABLE,
    RTL8168_INIT_FAILED
} rtl8168_init_result_t;

/* Initializes the RTL8168h/RTL8111h controller used by the Lenovo target. */
rtl8168_init_result_t rtl8168_init(const char **reason);
