#pragma once

#include <bootinfo.h>

void handoff(
    void *Entry,
    BOOT_INFO *BootInfo,
    void *StackTop
);
