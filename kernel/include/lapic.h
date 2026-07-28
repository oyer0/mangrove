#pragma once

#include <types.h>

#define LAPIC_ID              0x020
#define LAPIC_VERSION         0x030
#define LAPIC_TPR             0x080
#define LAPIC_EOI             0x0B0
#define LAPIC_SVR             0x0F0
#define LAPIC_ESR             0x280
#define LAPIC_ICR_LOW         0x300
#define LAPIC_ICR_HIGH        0x310
#define LAPIC_LVT_TIMER       0x320
#define LAPIC_LVT_LINT0       0x350
#define LAPIC_LVT_LINT1       0x360
#define LAPIC_TIMER_INITIAL   0x380
#define LAPIC_TIMER_CURRENT   0x390
#define LAPIC_TIMER_DIVIDE    0x3E0
#define LAPIC_LVT_MASKED   (1 << 16)

#define LAPIC_LVT_THERMAL       0x330
#define LAPIC_LVT_PERF          0x340
#define LAPIC_LINT0             0x350
#define LAPIC_LINT1             0x360
#define LAPIC_LVT_ERROR         0x370

bool lapic_present(void);

void lapic_init(void);

u32 lapic_read(u32 reg);
void lapic_write(u32 reg, u32 value);

void lapic_eoi(void);

void lapic_enable(void);
