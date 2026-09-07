/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <bootinfo.h>

void handoff(
    void *Entry,
    BOOT_INFO *BootInfo,
    void *StackTop,
    void *BootstrapPml4
);
