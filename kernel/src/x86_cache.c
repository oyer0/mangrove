/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <x86_cache.h>
#include <cpu.h>
#include <msr.h>
#include <vmm.h>
#include <string.h>

#define CACHE_PAT_MSR 0x277U
#define CACHE_MTRR_CAP 0xfeU
#define CACHE_MTRR_DEFAULT 0x2ffU
#define CACHE_PAT_SLOT 4U
#define CACHE_MTRR_MAX 16U
#define CACHE_UC 0U
#define CACHE_WC 1U
#define CACHE_WB 6U
#define CACHE_PAT_4K (1ULL << 7)

typedef struct {
    bool pat;
    bool wc;
    u32 physical_bits;
    u32 count;
    u64 pat_value;
    u64 default_type;
    u64 base[CACHE_MTRR_MAX];
    u64 mask[CACHE_MTRR_MAX];
} cache_snapshot_t;

static cache_snapshot_t cache_bsp;
static u64 cache_expected_pat;
static u64 cache_uc_flags = PTE_CACHEDISABLE | PTE_WRITETHROUGH;
static bool cache_initialized;
static bool cache_wc_available;
static u64 cache_framebuffer_start, cache_framebuffer_size;
/* Kept per CPU for admission and diagnostics; published before online state. */
static u64 cache_cpu_pat[CPU_MAX_COUNT];
static bool cache_cpu_valid[CPU_MAX_COUNT];

static void cache_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static u64 cache_cr0(void)
{
    u64 value;
    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static u64 cache_cr3(void)
{
    u64 value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static bool cache_boot_context(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return !(flags & (1ULL << 9));
}

/* The only changed PAT slot has no users on this CPU. Still drain caches
 * and global/non-global translations using the architectural cache-control
 * sequence; retain every unrelated PAT entry and leave MTRRs untouched. */
static void cache_install_pat(u64 value)
{
    u64 cr0 = cache_cr0(), cr3 = cache_cr3(), cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    u64 disabled = (cr0 | (1ULL << 30)) & ~(1ULL << 29);
    u64 no_global = cr4 & ~(1ULL << 7);
    __asm__ volatile("mov %0, %%cr0; wbinvd; mov %1, %%cr4; mov %2, %%cr3"
                     :: "r"(disabled), "r"(no_global), "r"(cr3) : "memory");
    wrmsr(CACHE_PAT_MSR, value);
    __asm__ volatile("wbinvd; mov %0, %%cr3; mov %1, %%cr4; mov %2, %%cr0"
                     :: "r"(cr3), "r"(cr4), "r"(cr0) : "memory");
}

void x86_cache_activate_boot_root(u64 physical)
{
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    u64 no_global = cr4 & ~(1ULL << 7);
    __asm__ volatile("wbinvd; mov %0, %%cr4; mov %1, %%cr3; mov %2, %%cr4"
                     :: "r"(no_global), "r"(physical), "r"(cr4) : "memory");
}

static bool cache_snapshot(cache_snapshot_t *state)
{
    u32 a, b, c, d, maximum;
    memset(state, 0, sizeof(*state));
    cache_cpuid(0, &maximum, &b, &c, &d);
    bool known_vendor = (b == 0x756e6547U && d == 0x49656e69U && c == 0x6c65746eU) ||
                        (b == 0x68747541U && d == 0x69746e65U && c == 0x444d4163U);
    if (maximum < 1U) return false;
    cache_cpuid(1, &a, &b, &c, &d);
    state->pat = (d & (1U << 16)) != 0U;
    if (state->pat) state->pat_value = rdmsr(CACHE_PAT_MSR);
    if (!(d & (1U << 12))) return true;
    u64 capability = rdmsr(CACHE_MTRR_CAP);
    state->default_type = rdmsr(CACHE_MTRR_DEFAULT);
    state->count = (u32)(capability & 255U);
    if (state->count > CACHE_MTRR_MAX) return true;
    for (u32 i = 0; i < state->count; i++) {
        state->base[i] = rdmsr(0x200U + i * 2U);
        state->mask[i] = rdmsr(0x201U + i * 2U);
    }
    cache_cpuid(0x80000000U, &a, &b, &c, &d);
    state->physical_bits = 36U;
    if (a >= 0x80000008U) {
        cache_cpuid(0x80000008U, &a, &b, &c, &d);
        state->physical_bits = a & 255U;
    }
    cache_cpuid(1, &a, &b, &c, &d);
    state->wc = known_vendor && state->pat && (capability & (1ULL << 10)) &&
                (d & (1U << 25)) && (state->default_type & (1ULL << 11)) &&
                state->physical_bits >= 32U && state->physical_bits <= 52U &&
                !(cache_cr0() & ((1ULL << 30) | (1ULL << 29)));
    return true;
}

static u64 cache_selector_flags(u32 index)
{
    return ((index & 1U) ? PTE_WRITETHROUGH : 0U) |
           ((index & 2U) ? PTE_CACHEDISABLE : 0U) |
           ((index & 4U) ? CACHE_PAT_4K : 0U);
}

/* Check the loader's active hierarchy, including huge-page PAT selectors.
 * Page-table walks use slots 0..3; changing slot 4 cannot affect them. */
static bool cache_slot_unused(u64 physical, u32 level)
{
    page_table_t *table = phys_to_virt(physical & PTE_FRAME_MASK);
    if (!table || !level) return false;
    for (u32 i = 0; i < 512U; i++) {
        u64 entry = table->entries[i];
        if (!(entry & PTE_PRESENT)) continue;
        if (level == 1U || (entry & PTE_HUGE)) {
            if (level == 4U) return false;
            u64 pat_bit = level == 1U ? CACHE_PAT_4K : (1ULL << 12);
            u32 selector = ((entry >> 3) & 3U) | ((entry & pat_bit) ? 4U : 0U);
            if (selector == CACHE_PAT_SLOT) return false;
        } else if (!cache_slot_unused(entry, level - 1U)) return false;
    }
    return true;
}

bool x86_cache_init_bsp(void)
{
    if (cache_initialized || cpu_current_index() != 0U ||
        !cache_boot_context() || !cache_snapshot(&cache_bsp))
        return false;
    cache_expected_pat = cache_bsp.pat_value;
    if (cache_bsp.pat) {
        u32 uc = 8U;
        /* Prefer the legacy selector; never select UC- as a UC fallback. */
        if (((cache_expected_pat >> 24) & 255U) == CACHE_UC) uc = 3U;
        else for (u32 i = 0; i < 8U; i++)
            if (((cache_expected_pat >> (i * 8U)) & 255U) == CACHE_UC) {uc = i; break;}
        if (uc == 8U) return false;
        cache_uc_flags = cache_selector_flags(uc);
        cache_wc_available = cache_bsp.wc && uc != CACHE_PAT_SLOT &&
                             cache_slot_unused(cache_cr3(), 4U);
        if (cache_wc_available) {
            cache_expected_pat = (cache_expected_pat & ~(255ULL << 32)) |
                                 ((u64)CACHE_WC << 32);
            if (cache_expected_pat != cache_bsp.pat_value)
                cache_install_pat(cache_expected_pat);
            if (rdmsr(CACHE_PAT_MSR) != cache_expected_pat) return false;
        }
    }
    cache_cpu_pat[0] = cache_expected_pat;
    cache_cpu_valid[0] = true;
    cache_initialized = true;
    return true;
}

bool x86_cache_init_ap(u32 index)
{
    cache_snapshot_t ap;
    if (!cache_initialized || !index || index >= CPU_MAX_COUNT ||
        cache_cpu_valid[index] ||
        cpu_current_index() != index ||
        !cache_boot_context() || !cache_snapshot(&ap) || ap.pat != cache_bsp.pat)
        return false;
    if (cache_wc_available) {
        /* APs have not touched the framebuffer or entered the scheduler.
         * Reject unrelated firmware-PAT or MTRR differences, rather than
         * changing types already in use by ordinary kernel mappings. */
        u64 keep = ~(255ULL << 32);
        if (!ap.wc || (ap.pat_value & keep) != (cache_expected_pat & keep) ||
            ap.count != cache_bsp.count ||
            ap.physical_bits != cache_bsp.physical_bits ||
            ap.default_type != cache_bsp.default_type ||
            memcmp(ap.base, cache_bsp.base, sizeof(ap.base)) ||
            memcmp(ap.mask, cache_bsp.mask, sizeof(ap.mask))) return false;
        if (ap.pat_value != cache_expected_pat) cache_install_pat(cache_expected_pat);
    } else if (ap.pat && ap.pat_value != cache_expected_pat) return false;
    if (ap.pat && rdmsr(CACHE_PAT_MSR) != cache_expected_pat) return false;
    cache_cpu_pat[index] = ap.pat ? rdmsr(CACHE_PAT_MSR) : 0U;
    __atomic_store_n(&cache_cpu_valid[index], true, __ATOMIC_RELEASE);
    return true;
}

bool x86_cache_cpu_ready(u32 index)
{
    return index < CPU_MAX_COUNT &&
           __atomic_load_n(&cache_cpu_valid[index], __ATOMIC_ACQUIRE);
}

u64 x86_cache_uc_flags(void) { return cache_uc_flags; }

static bool cache_wc_range(const cache_snapshot_t *state, u64 first, u64 size)
{
    if (!state->wc || first < 0x100000ULL || !size || first > ~(u64)0 - size)
        return false;
    u64 limit = first + size;
    u64 physical_mask = (1ULL << state->physical_bits) - 1ULL;
    if (limit - 1U > physical_mask) return false;
    u32 selected = 255U;
    bool uc = false;
    for (u32 i = 0; i < state->count; i++) {
        u64 mask = state->mask[i], base = state->base[i];
        if (!(mask & (1ULL << 11))) continue;
        u64 address_mask = mask & physical_mask & ~4095ULL;
        u64 bytes = ((~address_mask) & physical_mask) + 1ULL;
        u64 start = base & physical_mask & ~4095ULL;
        u32 type = base & 255U;
        if ((mask & ~(physical_mask & ~4095ULL) & ~(1ULL << 11)) ||
            (base & ~((physical_mask & ~4095ULL) | 255ULL)) ||
            !bytes || (bytes & (bytes - 1U)) || (start & (bytes - 1U)) ||
            (type != CACHE_UC && type != CACHE_WC && type != 4U &&
             type != 5U && type != CACHE_WB)) return false;
        if (start >= limit || start + bytes <= first) continue;
        /* Keep the first policy bounded: a partial MTRR boundary through
         * the image falls back to UC, even if segmentation could permit WC. */
        if (start > first || start + bytes < limit) return false;
        if (type == CACHE_UC) uc = true;
        else if (selected == 255U) selected = type;
        else if (selected != type) selected = 254U;
    }
    if (uc) return true; /* PAT WC + MTRR UC = WC, not WB. */
    if (selected == 255U) selected = state->default_type & 255U;
    return selected == CACHE_UC || selected == CACHE_WC || selected == CACHE_WB;
}

bool x86_cache_framebuffer_wc(u64 physical, u64 size, u64 *flags)
{
    if (!cache_initialized || !cache_wc_available || !flags ||
        cpu_current_index() != 0U || !cache_boot_context() ||
        ((physical | size) & 4095ULL) ||
        (cache_framebuffer_size && (physical != cache_framebuffer_start ||
                                    size != cache_framebuffer_size)) ||
        !cache_wc_range(&cache_bsp, physical, size)) return false;
    cache_framebuffer_start = physical;
    cache_framebuffer_size = size;
    *flags = cache_selector_flags(CACHE_PAT_SLOT);
    return true;
}
