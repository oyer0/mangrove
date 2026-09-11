/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <fpu.h>
#include <kprint.h>

#define CR0_MP             (1ULL << 1)
#define CR0_EM             (1ULL << 2)
#define CR0_TS             (1ULL << 3)
#define CR0_NE             (1ULL << 5)
#define CR4_OSFXSR         (1ULL << 9)
#define CR4_OSXMMEXCPT     (1ULL << 10)

#define CPUID_FXSR         (1U << 24)
#define CPUID_SSE          (1U << 25)
#define CPUID_SSE2         (1U << 26)

/* FCW=0x037f, all x87 registers empty, MXCSR=0x1f80.  Reserved bytes remain
 * zero so FXRSTOR receives a canonical baseline image. */
static const fpu_state_t initial_fpu_state = {
    .bytes = {
        [0] = 0x7f,
        [1] = 0x03,
        [24] = 0x80,
        [25] = 0x1f,
    },
};

static void fpu_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx,
                      u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static u64 fpu_read_cr0(void)
{
    u64 value;

    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static u64 fpu_read_cr4(void)
{
    u64 value;

    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void fpu_write_cr0(u64 value)
{
    __asm__ volatile("mov %0, %%cr0" :: "r"(value) : "memory");
}

static void fpu_write_cr4(u64 value)
{
    __asm__ volatile("mov %0, %%cr4" :: "r"(value) : "memory");
}

bool fpu_init_cpu(void)
{
    u32 max_leaf;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u64 cr0;
    u64 cr4;

    fpu_cpuid(0, 0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf < 1U) {
        kprint("[FAIL] CPU CPUID leaf 1 is unavailable\n");
        return false;
    }
    fpu_cpuid(1, 0, &max_leaf, &ebx, &ecx, &edx);
    if ((edx & (CPUID_FXSR | CPUID_SSE | CPUID_SSE2)) !=
        (CPUID_FXSR | CPUID_SSE | CPUID_SSE2)) {
        kprint("[FAIL] CPU lacks required FXSR/SSE/SSE2 support\n");
        return false;
    }

    cr0 = fpu_read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    fpu_write_cr0(cr0);

    cr4 = fpu_read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    fpu_write_cr4(cr4);

    /* Reset x87, MMX, and XMM state on every CPU without enabling AVX or
     * relying on whatever state firmware or the AP reset path supplied. */
    fpu_state_restore(&initial_fpu_state);

    cr0 = fpu_read_cr0();
    cr4 = fpu_read_cr4();
    if ((cr0 & (CR0_MP | CR0_EM | CR0_TS | CR0_NE)) !=
            (CR0_MP | CR0_NE) ||
        (cr4 & (CR4_OSFXSR | CR4_OSXMMEXCPT)) !=
            (CR4_OSFXSR | CR4_OSXMMEXCPT)) {
        kprint("[FAIL] CPU FPU control state could not be initialized\n");
        return false;
    }
    KERNEL_BOOT_DEBUG_LOG("[FPU] ready CR0=%p CR4=%p\n",
                          (void *)(uintptr_t)cr0,
                          (void *)(uintptr_t)cr4);
    return true;
}

void fpu_state_init(fpu_state_t *state)
{
    if (!state)
        return;
    for (u32 index = 0; index < FPU_STATE_SIZE; index++)
        state->bytes[index] = initial_fpu_state.bytes[index];
}

void fpu_state_save(fpu_state_t *state)
{
    if (!state)
        return;
    __asm__ volatile("fxsave %0" : "=m"(*state) :: "memory");
}

void fpu_state_restore(const fpu_state_t *state)
{
    if (!state)
        return;
    __asm__ volatile("fxrstor %0" :: "m"(*state) : "memory");
}
