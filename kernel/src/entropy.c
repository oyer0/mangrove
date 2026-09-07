/* SPDX-License-Identifier: GPL-3.0-only */
#include <entropy.h>

static void entropy_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx,
                          u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf));
}

static void entropy_features(bool *has_rdrand, bool *has_rdseed)
{
    u32 eax, ebx, ecx, edx;
    u32 max_leaf;

    if (has_rdrand) *has_rdrand = false;
    if (has_rdseed) *has_rdseed = false;
    entropy_cpuid(0, 0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf >= 1U) {
        entropy_cpuid(1U, 0, &eax, &ebx, &ecx, &edx);
        if (has_rdrand) *has_rdrand = (ecx & (1U << 30)) != 0;
    }
    if (max_leaf >= 7U) {
        entropy_cpuid(7U, 0, &eax, &ebx, &ecx, &edx);
        if (has_rdseed) *has_rdseed = (ebx & (1U << 18)) != 0;
    }
}

bool entropy_available(void)
{
    bool has_rdrand;
    bool has_rdseed;

    entropy_features(&has_rdrand, &has_rdseed);
    return has_rdrand || has_rdseed;
}

bool entropy_random_u64(u64 *value)
{
    bool has_rdrand;
    bool has_rdseed;
    unsigned char ready;

    if (!value) return false;
    entropy_features(&has_rdrand, &has_rdseed);
    if (has_rdseed) {
        for (u32 attempt = 0; attempt < 16U; attempt++) {
            __asm__ volatile("rdseed %0; setc %1"
                             : "=r"(*value), "=qm"(ready));
            if (ready) return true;
        }
    }
    if (has_rdrand) {
        for (u32 attempt = 0; attempt < 16U; attempt++) {
            __asm__ volatile("rdrand %0; setc %1"
                             : "=r"(*value), "=qm"(ready));
            if (ready) return true;
        }
    }
    return false;
}
