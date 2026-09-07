/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/types.h>

/* Kernel-originated events are copied into service-owned bounded queues. */
#define MG_EVENT_PROTOCOL_VERSION 1U
#define MG_EVENT_NAME_MAX          32U

enum {
    MG_EVENT_CLASS_DEVICE  = 1U << 0,
    MG_EVENT_CLASS_NETWORK = 1U << 1,
    MG_EVENT_CLASS_BLOCK   = 1U << 2,
    MG_EVENT_CLASS_USB     = 1U << 3,
};

enum {
    MG_EVENT_DEVICE_ADDED = 1,
    MG_EVENT_DEVICE_REMOVED,
    MG_EVENT_BLOCK_ADDED,
    MG_EVENT_BLOCK_REMOVED,
    MG_EVENT_USB_DEVICE_ADDED,
    MG_EVENT_USB_DEVICE_REMOVED,
    MG_EVENT_NETWORK_INTERFACE_ADDED,
    MG_EVENT_NETWORK_INTERFACE_REMOVED,
    MG_EVENT_NETWORK_LINK_UP,
    MG_EVENT_NETWORK_LINK_DOWN,
    MG_EVENT_QUEUE_OVERFLOW,
};

#define MG_EVENT_FLAG_OVERFLOW (1U << 0)

typedef struct PACKED {
    u16 version;
    u16 type;
    u32 event_class;
    u32 flags;
    u64 sequence;
    u64 resource_id;
    char name[MG_EVENT_NAME_MAX];
} mg_event_t;
