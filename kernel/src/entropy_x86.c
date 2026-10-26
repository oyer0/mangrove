/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <entropy_platform.h>

static void entropy_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx,
                          u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static bool entropy_x86_source_supported(entropy_source_t source)
{
    u32 eax, ebx, ecx, edx, max_leaf;

    entropy_cpuid(0U, 0U, &max_leaf, &ebx, &ecx, &edx);
    if (source == ENTROPY_SOURCE_RDRAND) {
        if (max_leaf < 1U) return false;
        entropy_cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
        return (ecx & (1U << 30)) != 0;
    }
    if (source == ENTROPY_SOURCE_RDSEED) {
        if (max_leaf < 7U) return false;
        entropy_cpuid(7U, 0U, &eax, &ebx, &ecx, &edx);
        return (ebx & (1U << 18)) != 0;
    }
    return false;
}

bool entropy_platform_source_supported(entropy_source_t source)
{
    return entropy_x86_source_supported(source);
}

static u64 entropy_irq_save_disable(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void entropy_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

entropy_word_result_t entropy_platform_read_word(entropy_source_t source,
                                                 u64 *value)
{
    unsigned char ready = 0;
    u64 candidate = 0;
    u64 flags;

    if (!value) return ENTROPY_WORD_UNSUPPORTED;
    flags = entropy_irq_save_disable();
    if (!entropy_x86_source_supported(source)) {
        entropy_irq_restore(flags);
        return ENTROPY_WORD_UNSUPPORTED;
    }
    if (source == ENTROPY_SOURCE_RDSEED)
        __asm__ volatile("rdseed %0; setc %1"
                         : "=r"(candidate), "=qm"(ready) :: "memory");
    else if (source == ENTROPY_SOURCE_RDRAND)
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(candidate), "=qm"(ready) :: "memory");
    else {
        entropy_irq_restore(flags);
        return ENTROPY_WORD_UNSUPPORTED;
    }
    entropy_irq_restore(flags);

    if (!ready) {
        volatile u8 *bytes = (volatile u8 *)&candidate;
        for (usize index = 0; index < sizeof(candidate); index++)
            bytes[index] = 0;
        return ENTROPY_WORD_NOT_READY;
    }
    *value = candidate;
    {
        volatile u8 *bytes = (volatile u8 *)&candidate;
        for (usize index = 0; index < sizeof(candidate); index++)
            bytes[index] = 0;
    }
    return ENTROPY_WORD_READY;
}
