/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* The firmware-selected page is chosen at boot from conventional memory. */
#define SMP_TRAMPOLINE_PAGE_SIZE       0x1000ULL
#define SMP_TRAMPOLINE_MAILBOX_OFFSET  0x800U
#define SMP_TRAMPOLINE_LOW_LIMIT       0x00100000ULL
#define SMP_TRAMPOLINE_VGA_LIMIT       0x000A0000ULL
#define SMP_AP_STACK_SIZE               (64U * 1024U)
#define SMP_AP_IDLE_STACK_SIZE          (16U * 1024U)

typedef enum {
    SMP_AP_MAILBOX_IDLE = 0,
    SMP_AP_MAILBOX_PREPARED,
    SMP_AP_MAILBOX_STARTED,
    SMP_AP_MAILBOX_ONLINE,
    SMP_AP_MAILBOX_FAILED,
} smp_ap_mailbox_state_t;

/* This layout is copied to the low trampoline page and is also consumed by
 * the 16/32/64-bit startup code.  Keep fields naturally sized and stable. */
typedef struct {
    u32 cpu_index;
    u32 apic_id;
    u64 cr3;
    u64 stack_top;
    u64 entry;
    u64 cpu_local;
    u32 state;
    u32 reserved;
} smp_trampoline_mailbox_t;

_Static_assert(sizeof(smp_trampoline_mailbox_t) == 48,
               "SMP trampoline mailbox layout changed");

bool smp_start(void);
void smp_ap_entry(u32 cpu_index);
