/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLAB_CORE_DIAGNOSTICS
#error "slab core is compiled only in isolated diagnostic builds"
#endif

#include <slab.h>
#include <heap.h>
#include <pmm.h>
#include <spinlock.h>
#include <panic.h>
#include <stddef.h>
#include <address_layout.h>

#define SLAB_BITMAP_WORDS 4U
#define SLAB_BITMAP_BYTES (SLAB_BITMAP_WORDS * sizeof(u64))
#define SLAB_DESCRIPTOR_BYTES 64U
#define SLAB_STATE_PARTIAL 1U
#define SLAB_STATE_FULL 2U
#define SLAB_STATE_EMPTY 3U
#define SLAB_DESCRIPTOR_MAGIC 0x534c414250495448ULL
#define SLAB_INTEGRITY_SEED 0xa31d59c7e20b846fULL
#define SLAB_CLASSIFIER_FANOUT 512U
#define SLAB_CLASSIFIER_ROOT_PAGES \
    ((usize)SLAB_CLASSIFIER_FANOUT * SLAB_CLASSIFIER_FANOUT)
#define SLAB_CLASSIFIER_CLASS_BITS 5U
#define SLAB_CLASSIFIER_CLASS_MASK ((1U << SLAB_CLASSIFIER_CLASS_BITS) - 1U)
#define SLAB_CLASSIFIER_ROOT_MAGIC 0x534c4142524f4f54ULL
#define SLAB_CLASSIFIER_MIDDLE_MAGIC 0x534c41424d494444ULL
#define SLAB_CLASSIFIER_LEAF_MAGIC 0x534c41424c454146ULL

_Static_assert(PAGE_SIZE == 4096U, "Pith slab pages are 4 KiB");
_Static_assert(SLAB_BITMAP_BYTES == 32U,
               "slab bitmap covers 256 slots");
_Static_assert((PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) / 16U <=
                   SLAB_BITMAP_WORDS * 64U,
               "bitmap covers every slot in the smallest class");
_Static_assert((SLAB_OBJECT_AREA_OFFSET & 15U) == 0,
               "slab object area preserves 16-byte alignment");
_Static_assert(SLAB_CLASS_COUNT <= SLAB_CLASSIFIER_CLASS_MASK,
               "page-classifier tags fit in the class field");

typedef struct slab_descriptor slab_descriptor_t;
struct slab_descriptor {
    u64 magic;
    u32 class_index;
    u16 slot_count;
    u16 allocated_count;
    u32 state;
    u32 reserved0;
    slab_descriptor_t *partial_prev;
    slab_descriptor_t *partial_next;
    u64 integrity;
    u64 reserved[2];
};

typedef struct {
    u64 magic;
    usize entry_count;
    uintptr_t entries[];
} slab_classifier_root_t;

typedef struct {
    u64 magic;
    usize root_index;
    uintptr_t leaves[SLAB_CLASSIFIER_FANOUT];
} slab_classifier_middle_t;

/* The low five bits store class + 1; the remaining bits retain a page
 * generation across retirement and same-address reuse. */
typedef struct {
    u64 magic;
    usize leaf_index;
    u64 page_tokens[SLAB_CLASSIFIER_FANOUT];
} slab_classifier_leaf_t;

typedef struct {
    spinlock_t lock;
    slab_descriptor_t *partial_head;
    slab_class_stats_t stats;
} slab_class_t;

_Static_assert(sizeof(slab_descriptor_t) == SLAB_DESCRIPTOR_BYTES,
               "slab descriptor occupies 64 bytes");
_Static_assert(_Alignof(slab_descriptor_t) >= 8U,
               "slab descriptor requires natural x86-64 alignment");
_Static_assert(SLAB_DESCRIPTOR_BYTES + SLAB_BITMAP_BYTES ==
                   SLAB_OBJECT_AREA_OFFSET,
               "slab objects begin after descriptor and bitmap");
_Static_assert(sizeof(slab_classifier_middle_t) == 4112U,
               "classifier middle node layout is stable");
_Static_assert(sizeof(slab_classifier_leaf_t) == 4112U,
               "classifier leaf node layout is stable");
_Static_assert(offsetof(slab_classifier_leaf_t, page_tokens) % 8U == 0U,
               "page tokens are aligned for atomic access");

static const u16 slab_sizes[SLAB_CLASS_COUNT] = {
    16U, 32U, 48U, 64U, 80U, 96U, 112U, 128U,
    144U, 160U, 176U, 192U, 208U, 224U, 240U, 256U,
    288U, 320U, 352U, 384U
};
_Static_assert(sizeof(slab_sizes) / sizeof(slab_sizes[0]) == SLAB_CLASS_COUNT,
               "slab class table matches its public count");

static slab_class_t slab_classes[SLAB_CLASS_COUNT];
static uintptr_t slab_heap_start;
static uintptr_t slab_heap_end;
static usize slab_heap_pages;
static usize slab_classifier_root_entries;
static slab_classifier_root_t *slab_classifier_root;
static u32 slab_initialization_state;
static u64 slab_classifier_root_payload_bytes;
static u64 slab_classifier_middle_payload_bytes;
static u64 slab_classifier_leaf_payload_bytes;
static u64 slab_classifier_retained_payload_bytes;
static u64 slab_classifier_peak_payload_bytes;
static u64 slab_classifier_root_tables;
static u64 slab_classifier_middle_tables;
static u64 slab_classifier_leaf_tables;
static u64 slab_classifier_allocation_failures;

static u64 *slab_bitmap(slab_descriptor_t *slab)
{
    return (u64 *)((u8 *)slab + SLAB_DESCRIPTOR_BYTES);
}

static const u64 *slab_bitmap_const(const slab_descriptor_t *slab)
{
    return (const u64 *)((const u8 *)slab + SLAB_DESCRIPTOR_BYTES);
}

static u8 slab_token_class(u64 token)
{
    return (u8)(token & SLAB_CLASSIFIER_CLASS_MASK);
}

static bool slab_classifier_metadata_valid(const void *pointer, usize bytes)
{
    return pointer && !((uintptr_t)pointer & 15U) &&
           heap_address_range_is_mapped(pointer, bytes);
}

static void slab_classifier_require_metadata(const void *pointer, usize bytes,
                                             const char *reason)
{
    if (!slab_classifier_metadata_valid(pointer, bytes))
        panic(reason);
}

static usize slab_classifier_payload_size(usize bytes)
{
    if (!bytes || bytes > (usize)-1 - 15U)
        return 0;
    return (bytes + 15U) & ~(usize)15U;
}

static void slab_classifier_record_published(u32 kind, usize requested)
{
    u64 payload = slab_classifier_payload_size(requested);
    u64 total;

    if (!payload)
        panic("slab: classifier metadata size overflow");
    if (kind == 0U) {
        __atomic_add_fetch(&slab_classifier_root_payload_bytes, payload,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_classifier_root_tables, 1U,
                           __ATOMIC_RELAXED);
    } else if (kind == 1U) {
        __atomic_add_fetch(&slab_classifier_middle_payload_bytes, payload,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_classifier_middle_tables, 1U,
                           __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&slab_classifier_leaf_payload_bytes, payload,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&slab_classifier_leaf_tables, 1U,
                           __ATOMIC_RELAXED);
    }
    total = __atomic_add_fetch(&slab_classifier_retained_payload_bytes,
                               payload, __ATOMIC_RELAXED);
    u64 peak = __atomic_load_n(&slab_classifier_peak_payload_bytes,
                               __ATOMIC_RELAXED);
    while (total > peak && !__atomic_compare_exchange_n(
               &slab_classifier_peak_payload_bytes, &peak, total, true,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void *slab_classifier_allocate(usize bytes)
{
    if (!slab_classifier_payload_size(bytes)) {
        __atomic_add_fetch(&slab_classifier_allocation_failures, 1U,
                           __ATOMIC_RELAXED);
        return NULL;
    }
    void *allocation = heap_alloc_raw(bytes);
    if (!allocation)
        __atomic_add_fetch(&slab_classifier_allocation_failures, 1U,
                           __ATOMIC_RELAXED);
    return allocation;
}

static slab_classifier_root_t *slab_classifier_root_ensure(void)
{
    slab_classifier_root_t *root = __atomic_load_n(&slab_classifier_root,
                                                    __ATOMIC_ACQUIRE);
    if (root) {
        slab_classifier_require_metadata(root, sizeof(*root),
            "slab: classifier root outside mapped heap");
        if (root->magic != SLAB_CLASSIFIER_ROOT_MAGIC ||
            root->entry_count != slab_classifier_root_entries)
            panic("slab: corrupted classifier root");
        return root;
    }
    if (slab_classifier_root_entries >
        ((usize)-1 - sizeof(*root)) / sizeof(root->entries[0])) {
        __atomic_add_fetch(&slab_classifier_allocation_failures, 1U,
                           __ATOMIC_RELAXED);
        return NULL;
    }

    usize bytes = sizeof(*root) + slab_classifier_root_entries *
                  sizeof(root->entries[0]);
    root = slab_classifier_allocate(bytes);
    if (!root)
        return NULL;
    root->magic = SLAB_CLASSIFIER_ROOT_MAGIC;
    root->entry_count = slab_classifier_root_entries;
    for (usize index = 0; index < slab_classifier_root_entries; index++)
        __atomic_store_n(&root->entries[index], (uintptr_t)0,
                         __ATOMIC_RELAXED);

    slab_classifier_root_t *expected = NULL;
    if (!__atomic_compare_exchange_n(&slab_classifier_root, &expected, root,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        heap_free_raw(root);
        root = expected;
    } else {
        slab_classifier_record_published(0U, bytes);
    }
    slab_classifier_require_metadata(root, sizeof(*root),
        "slab: published classifier root outside mapped heap");
    if (root->magic != SLAB_CLASSIFIER_ROOT_MAGIC ||
        root->entry_count != slab_classifier_root_entries)
        panic("slab: corrupted published classifier root");
    return root;
}

static slab_classifier_middle_t *slab_classifier_middle(
    slab_classifier_root_t *root, usize root_index, bool create)
{
    uintptr_t value = __atomic_load_n(&root->entries[root_index],
                                      __ATOMIC_ACQUIRE);
    slab_classifier_middle_t *middle = (slab_classifier_middle_t *)value;
    if (middle) {
        slab_classifier_require_metadata(middle, sizeof(*middle),
            "slab: classifier middle node outside mapped heap");
        if (middle->magic != SLAB_CLASSIFIER_MIDDLE_MAGIC ||
            middle->root_index != root_index)
            panic("slab: corrupted classifier middle node");
        return middle;
    }
    if (!create)
        return NULL;

    middle = slab_classifier_allocate(sizeof(*middle));
    if (!middle)
        return NULL;
    middle->magic = SLAB_CLASSIFIER_MIDDLE_MAGIC;
    middle->root_index = root_index;
    for (usize index = 0; index < SLAB_CLASSIFIER_FANOUT; index++)
        __atomic_store_n(&middle->leaves[index], (uintptr_t)0,
                         __ATOMIC_RELAXED);
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(&root->entries[root_index], &expected,
                                     (uintptr_t)middle, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        heap_free_raw(middle);
        middle = (slab_classifier_middle_t *)expected;
    } else {
        slab_classifier_record_published(1U, sizeof(*middle));
    }
    slab_classifier_require_metadata(middle, sizeof(*middle),
        "slab: published classifier middle node outside mapped heap");
    if (middle->magic != SLAB_CLASSIFIER_MIDDLE_MAGIC ||
        middle->root_index != root_index)
        panic("slab: corrupted published classifier middle node");
    return middle;
}

static slab_classifier_leaf_t *slab_classifier_leaf(
    slab_classifier_middle_t *middle, usize leaf_index, bool create)
{
    usize slot = leaf_index % SLAB_CLASSIFIER_FANOUT;
    uintptr_t value = __atomic_load_n(&middle->leaves[slot],
                                      __ATOMIC_ACQUIRE);
    slab_classifier_leaf_t *leaf = (slab_classifier_leaf_t *)value;
    if (leaf) {
        slab_classifier_require_metadata(leaf, sizeof(*leaf),
            "slab: classifier leaf node outside mapped heap");
        if (leaf->magic != SLAB_CLASSIFIER_LEAF_MAGIC ||
            leaf->leaf_index != leaf_index)
            panic("slab: corrupted classifier leaf node");
        return leaf;
    }
    if (!create)
        return NULL;

    leaf = slab_classifier_allocate(sizeof(*leaf));
    if (!leaf)
        return NULL;
    leaf->magic = SLAB_CLASSIFIER_LEAF_MAGIC;
    leaf->leaf_index = leaf_index;
    for (usize index = 0; index < SLAB_CLASSIFIER_FANOUT; index++)
        __atomic_store_n(&leaf->page_tokens[index], 0U, __ATOMIC_RELAXED);
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(&middle->leaves[slot], &expected,
                                     (uintptr_t)leaf, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        heap_free_raw(leaf);
        leaf = (slab_classifier_leaf_t *)expected;
    } else {
        slab_classifier_record_published(2U, sizeof(*leaf));
    }
    slab_classifier_require_metadata(leaf, sizeof(*leaf),
        "slab: published classifier leaf node outside mapped heap");
    if (leaf->magic != SLAB_CLASSIFIER_LEAF_MAGIC ||
        leaf->leaf_index != leaf_index)
        panic("slab: corrupted published classifier leaf node");
    return leaf;
}

static bool slab_classifier_ensure(usize page_index)
{
    usize root_index = page_index / SLAB_CLASSIFIER_ROOT_PAGES;
    usize leaf_index = page_index / SLAB_CLASSIFIER_FANOUT;
    slab_classifier_root_t *root;
    slab_classifier_middle_t *middle;

    if (page_index >= slab_heap_pages ||
        root_index >= slab_classifier_root_entries)
        return false;
    root = slab_classifier_root_ensure();
    if (!root)
        return false;
    middle = slab_classifier_middle(root, root_index, true);
    if (!middle)
        return false;
    return slab_classifier_leaf(middle, leaf_index, true) != NULL;
}

static u64 slab_page_token_load(usize page_index)
{
    usize root_index = page_index / SLAB_CLASSIFIER_ROOT_PAGES;
    usize leaf_index = page_index / SLAB_CLASSIFIER_FANOUT;
    slab_classifier_root_t *root = __atomic_load_n(&slab_classifier_root,
                                                    __ATOMIC_ACQUIRE);
    if (!root)
        return 0;
    slab_classifier_require_metadata(root, sizeof(*root),
        "slab: classifier root outside mapped heap");
    if (root->magic != SLAB_CLASSIFIER_ROOT_MAGIC ||
        root->entry_count != slab_classifier_root_entries)
        panic("slab: corrupted classifier root");
    if (root_index >= root->entry_count)
        panic("slab: classifier root index out of range");
    slab_classifier_middle_t *middle = slab_classifier_middle(
        root, root_index, false);
    if (!middle)
        return 0;
    slab_classifier_leaf_t *leaf = slab_classifier_leaf(
        middle, leaf_index, false);
    if (!leaf)
        return 0;
    return __atomic_load_n(
        &leaf->page_tokens[page_index % SLAB_CLASSIFIER_FANOUT],
        __ATOMIC_ACQUIRE);
}

static void slab_page_token_store(usize page_index, u64 token)
{
    usize root_index = page_index / SLAB_CLASSIFIER_ROOT_PAGES;
    usize leaf_index = page_index / SLAB_CLASSIFIER_FANOUT;
    slab_classifier_root_t *root = __atomic_load_n(&slab_classifier_root,
                                                    __ATOMIC_ACQUIRE);
    if (!root)
        panic("slab: missing classifier root during tag update");
    slab_classifier_require_metadata(root, sizeof(*root),
        "slab: classifier root outside mapped heap during tag update");
    if (root->magic != SLAB_CLASSIFIER_ROOT_MAGIC ||
        root->entry_count != slab_classifier_root_entries ||
        root_index >= root->entry_count)
        panic("slab: invalid classifier root during tag update");
    slab_classifier_middle_t *middle = slab_classifier_middle(
        root, root_index, false);
    slab_classifier_leaf_t *leaf = middle
        ? slab_classifier_leaf(middle, leaf_index, false) : NULL;
    if (!leaf)
        panic("slab: missing classifier leaf during tag update");
    __atomic_store_n(
        &leaf->page_tokens[page_index % SLAB_CLASSIFIER_FANOUT], token,
        __ATOMIC_RELEASE);
}

static bool slab_page_index(const void *page, usize *index_out)
{
    uintptr_t address = (uintptr_t)page;
    uintptr_t relative;

    if (!index_out || !slab_heap_start || address < slab_heap_start ||
        address >= slab_heap_end || (address & (PAGE_SIZE - 1U)) ||
        !heap_address_is_mapped(page))
        return false;
    relative = address - slab_heap_start;
    if (relative & (PAGE_SIZE - 1U))
        return false;
    usize index = (usize)(relative / PAGE_SIZE);
    if (index >= slab_heap_pages)
        return false;
    *index_out = index;
    return true;
}

static bool slab_pointer_page(const void *pointer, uintptr_t *page_out,
                              usize *index_out)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t page;
    usize index;

    if (!pointer || !slab_heap_start || address < slab_heap_start ||
        address >= slab_heap_end)
        return false;
    page = address & ~((uintptr_t)PAGE_SIZE - 1U);
    if (page < slab_heap_start || page >= slab_heap_end ||
        !slab_page_index((const void *)page, &index))
        return false;
    if (page_out) *page_out = page;
    if (index_out) *index_out = index;
    return true;
}

static u16 slab_slots_for_class(u32 class_index)
{
    if (class_index >= SLAB_CLASS_COUNT)
        return 0;
    return (u16)((PAGE_SIZE - SLAB_OBJECT_AREA_OFFSET) /
                 slab_sizes[class_index]);
}

static u64 slab_descriptor_integrity(const slab_descriptor_t *slab)
{
    u64 value = SLAB_INTEGRITY_SEED ^ slab->magic;
    value ^= ((u64)slab->class_index << 32) |
             ((u64)slab->slot_count << 16) | slab->allocated_count;
    value ^= (u64)slab->state << 48;
    value ^= (uintptr_t)slab->partial_prev;
    value = (value << 17) | (value >> (64U - 17U));
    value ^= (uintptr_t)slab->partial_next;
    value ^= ((u64)slab->reserved0 << 9) ^ slab->reserved[0] ^
             ((slab->reserved[1] << 7) | (slab->reserved[1] >> 57));
    const u64 *bitmap = slab_bitmap_const(slab);
    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        value ^= bitmap[word] + 0x9e3779b97f4a7c15ULL +
                 ((u64)word << 32);
        value = (value << 13) | (value >> (64U - 13U));
    }
    value ^= value >> 29;
    value *= 0x9e3779b185ebca87ULL;
    value ^= value >> 31;
    return value;
}

static u32 slab_word_population(u64 word)
{
    word -= (word >> 1) & 0x5555555555555555ULL;
    word = (word & 0x3333333333333333ULL) +
           ((word >> 2) & 0x3333333333333333ULL);
    word = (word + (word >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (u32)((word * 0x0101010101010101ULL) >> 56);
}

static bool slab_bitmap_valid(const slab_descriptor_t *slab)
{
    const u64 *bitmap = slab_bitmap_const(slab);
    u32 population = 0;

    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        u32 first_slot = word * 64U;
        u32 remaining = slab->slot_count > first_slot
            ? (u32)slab->slot_count - first_slot : 0U;
        u64 valid_mask = remaining >= 64U ? ~(u64)0 :
            (remaining ? (((u64)1U << remaining) - 1U) : 0U);
        if (bitmap[word] & ~valid_mask)
            return false;
        population += slab_word_population(bitmap[word]);
    }
    return population == slab->allocated_count;
}

static bool slab_descriptor_address_valid(const slab_descriptor_t *slab,
                                          u32 class_index)
{
    usize page_index;
    if (!slab || class_index >= SLAB_CLASS_COUNT ||
        ((uintptr_t)slab & (PAGE_SIZE - 1U)) ||
        !slab_page_index(slab, &page_index))
        return false;
    u64 token = slab_page_token_load(page_index);
    return slab_token_class(token) == (u8)(class_index + 1U) &&
        (token >> SLAB_CLASSIFIER_CLASS_BITS) != 0;
}

static bool slab_descriptor_fields_valid(const slab_descriptor_t *slab,
                                         u32 class_index)
{
    if (!slab_descriptor_address_valid(slab, class_index))
        return false;
    if (slab->magic != SLAB_DESCRIPTOR_MAGIC ||
        slab->class_index != class_index ||
        slab->slot_count != slab_slots_for_class(class_index) ||
        !slab->allocated_count ||
        slab->allocated_count > slab->slot_count ||
        slab->reserved0 || slab->reserved[0] || slab->reserved[1] ||
        slab->integrity != slab_descriptor_integrity(slab) ||
        !slab_bitmap_valid(slab))
        return false;
    if (slab->allocated_count == slab->slot_count) {
        if (slab->state != SLAB_STATE_FULL || slab->partial_prev ||
            slab->partial_next)
            return false;
    } else if (slab->state != SLAB_STATE_PARTIAL) {
        return false;
    }
    return true;
}

static bool slab_descriptor_valid_locked(const slab_descriptor_t *slab,
                                         u32 class_index)
{
    if (!slab_descriptor_fields_valid(slab, class_index))
        return false;
    if (slab->state == SLAB_STATE_PARTIAL) {
        if (!slab->partial_prev) {
            if (slab_classes[class_index].partial_head != slab)
                return false;
        } else if (slab_classes[class_index].partial_head == slab ||
                   slab->partial_prev == slab ||
                   !slab_descriptor_fields_valid(slab->partial_prev,
                                                  class_index) ||
                   slab->partial_prev->state != SLAB_STATE_PARTIAL ||
                   slab->partial_prev->partial_next != slab) {
            return false;
        }
        if (slab->partial_next == slab || (slab->partial_next &&
            (!slab_descriptor_fields_valid(slab->partial_next,
                                           class_index) ||
             slab->partial_next->state != SLAB_STATE_PARTIAL ||
             slab->partial_next->partial_prev != slab)))
            return false;
    }
    return true;
}

static void slab_require_valid(bool valid, const char *reason)
{
    if (!valid)
        panic(reason);
}

static void slab_update_integrity(slab_descriptor_t *slab)
{
    slab->integrity = slab_descriptor_integrity(slab);
}

static void slab_list_insert_locked(u32 class_index,
                                    slab_descriptor_t *slab)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *head = class_state->partial_head;

    slab_require_valid(slab->state == SLAB_STATE_PARTIAL &&
                       !slab->partial_prev && !slab->partial_next &&
                       head != slab,
                       "slab: duplicate or invalid partial-list insertion");
    if (head) {
        slab_require_valid(slab_descriptor_valid_locked(head, class_index) &&
                           head->state == SLAB_STATE_PARTIAL &&
                           !head->partial_prev,
                           "slab: invalid partial-list head");
        head->partial_prev = slab;
        slab_update_integrity(head);
    }
    slab->partial_next = head;
    class_state->partial_head = slab;
    slab_update_integrity(slab);
}

static void slab_list_remove_locked(u32 class_index,
                                    slab_descriptor_t *slab)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *previous = slab->partial_prev;
    slab_descriptor_t *next = slab->partial_next;

    slab_require_valid(slab_descriptor_valid_locked(slab, class_index) &&
                       slab->state == SLAB_STATE_PARTIAL,
                       "slab: invalid partial-list removal");
    if (previous) {
        previous->partial_next = next;
        slab_update_integrity(previous);
    } else {
        slab_require_valid(class_state->partial_head == slab,
                           "slab: partial slab missing from class list");
        class_state->partial_head = next;
    }
    if (next) {
        next->partial_prev = previous;
        slab_update_integrity(next);
    }
    slab->partial_prev = NULL;
    slab->partial_next = NULL;
    slab_update_integrity(slab);
}

/* Caller holds this class lock.  At most four words are inspected. */
static bool slab_find_free_slot_locked(slab_descriptor_t *slab,
                                       u32 class_index, u16 *slot_out)
{
    u64 *bitmap = slab_bitmap(slab);
    u32 slots = slab->slot_count;

    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++) {
        u32 first_slot = word * 64U;
        u32 remaining = slots > first_slot ? slots - first_slot : 0U;
        u64 valid_mask = remaining >= 64U ? ~(u64)0 :
            (remaining ? (((u64)1U << remaining) - 1U) : 0U);
        u64 available;
        slab_classes[class_index].stats.bitmap_words_examined++;
        available = ~bitmap[word] & valid_mask;
        if (available) {
            u32 bit = 0;
            while (!(available & ((u64)1U << bit))) bit++;
            *slot_out = (u16)(first_slot + bit);
            return true;
        }
    }
    return false;
}

static void slab_record_live_locked(slab_class_t *class_state)
{
    class_state->stats.live_slots++;
    if (class_state->stats.live_slots > class_state->stats.peak_live_slots)
        class_state->stats.peak_live_slots = class_state->stats.live_slots;
    class_state->stats.allocations++;
}

static void *slab_take_existing_locked(u32 class_index)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *slab = class_state->partial_head;
    u16 slot;
    u64 *bitmap;
    u32 old_count;

    if (!slab)
        return NULL;
    slab_require_valid(slab_descriptor_valid_locked(slab, class_index) &&
                       slab->state == SLAB_STATE_PARTIAL &&
                       !slab->partial_prev,
                       "slab: corrupted partial-list head");
    if (!slab_find_free_slot_locked(slab, class_index, &slot))
        panic("slab: partial slab has no free slot");

    old_count = slab->allocated_count;
    if (old_count + 1U == slab->slot_count) {
        slab_require_valid(class_state->stats.partial_slabs != 0,
                           "slab: partial-slab counter underflow");
        slab_list_remove_locked(class_index, slab);
    }
    bitmap = slab_bitmap(slab);
    bitmap[slot / 64U] |= (u64)1U << (slot % 64U);
    slab->allocated_count++;
    slab_record_live_locked(class_state);
    if (slab->allocated_count == slab->slot_count) {
        slab->state = SLAB_STATE_FULL;
        class_state->stats.partial_slabs--;
        class_state->stats.full_slabs++;
        class_state->stats.transitions_to_full++;
    } else if (slab->allocated_count <= old_count) {
        panic("slab: allocation count did not advance");
    }
    slab_update_integrity(slab);
    slab_require_valid(slab_descriptor_valid_locked(slab, class_index),
                       "slab: invalid descriptor after allocation");
    return (u8 *)slab + SLAB_OBJECT_AREA_OFFSET +
           (usize)slot * slab_sizes[class_index];
}

bool slab_init(void *heap_start, usize heap_capacity)
{
    uintptr_t start = (uintptr_t)heap_start;
    uintptr_t end;
    u32 expected = 0U;

    if (!heap_start || (start & (PAGE_SIZE - 1U)) || !heap_capacity ||
        (heap_capacity & (PAGE_SIZE - 1U)) ||
        heap_capacity > (usize)(KERNEL_HEAP_LIMIT - KERNEL_HEAP_BASE) ||
        start > (uintptr_t)-1 - heap_capacity)
        return false;
    end = start + heap_capacity;
    if (!__atomic_compare_exchange_n(&slab_initialization_state, &expected,
                                     1U, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;

    slab_heap_start = start;
    slab_heap_end = end;
    slab_heap_pages = heap_capacity / PAGE_SIZE;
    slab_classifier_root_entries = slab_heap_pages /
        SLAB_CLASSIFIER_ROOT_PAGES +
        (slab_heap_pages % SLAB_CLASSIFIER_ROOT_PAGES != 0U);
    if (!slab_classifier_root_entries) {
        __atomic_store_n(&slab_initialization_state, 0U, __ATOMIC_RELEASE);
        return false;
    }
    for (u32 index = 0; index < SLAB_CLASS_COUNT; index++) {
        spinlock_init(&slab_classes[index].lock);
        slab_classes[index].partial_head = NULL;
        slab_classes[index].stats = (slab_class_stats_t){0};
    }
    __atomic_store_n(&slab_initialization_state, 2U, __ATOMIC_RELEASE);
    return true;
}

static bool slab_select_class(usize size, u32 *class_index)
{
    if (!size || size > SLAB_MAX_OBJECT_SIZE || !class_index)
        return false;
    for (u32 index = 0; index < SLAB_CLASS_COUNT; index++) {
        if (size <= slab_sizes[index]) {
            *class_index = index;
            return true;
        }
    }
    return false;
}

static void slab_initialize_descriptor(slab_descriptor_t *slab,
                                       u32 class_index)
{
    *slab = (slab_descriptor_t){
        .magic = SLAB_DESCRIPTOR_MAGIC,
        .class_index = class_index,
        .slot_count = slab_slots_for_class(class_index),
        .allocated_count = 0,
        .state = SLAB_STATE_PARTIAL,
        .partial_prev = NULL,
        .partial_next = NULL,
        .integrity = 0,
        .reserved0 = 0,
        .reserved = {0, 0},
    };
    u64 *bitmap = slab_bitmap(slab);
    for (u32 word = 0; word < SLAB_BITMAP_WORDS; word++)
        bitmap[word] = 0;
    slab_update_integrity(slab);
}

static void *slab_publish_new_locked(u32 class_index, void *backing)
{
    slab_class_t *class_state = &slab_classes[class_index];
    slab_descriptor_t *slab = (slab_descriptor_t *)backing;
    usize page_index;
    u16 slot = 0;
    u64 old_token;
    u64 generation;

    slab_require_valid(slab_page_index(backing, &page_index),
                       "slab: backing page outside mapped heap");
    old_token = slab_page_token_load(page_index);
    slab_require_valid(slab_token_class(old_token) == 0U,
                       "slab: backing page already classified");
    slab_require_valid(slab->magic == SLAB_DESCRIPTOR_MAGIC &&
                       slab->class_index == class_index &&
                       slab->slot_count == slab_slots_for_class(class_index) &&
                       !slab->allocated_count &&
                       slab->state == SLAB_STATE_PARTIAL &&
                       !slab->partial_prev && !slab->partial_next &&
                       !slab->reserved0 && !slab->reserved[0] &&
                       !slab->reserved[1] &&
                       slab->integrity == slab_descriptor_integrity(slab) &&
                       slab_bitmap_valid(slab),
                       "slab: invalid unpublished descriptor");
    slab_bitmap(slab)[0] = 1U;
    slab->allocated_count = 1U;
    slab_update_integrity(slab);
    generation = old_token >> SLAB_CLASSIFIER_CLASS_BITS;
    if (generation == ((u64)-1 >> SLAB_CLASSIFIER_CLASS_BITS))
        panic("slab: page classification generation exhausted");
    generation++;
    slab_page_token_store(page_index,
        (generation << SLAB_CLASSIFIER_CLASS_BITS) | (class_index + 1U));
    slab_list_insert_locked(class_index, slab);
    class_state->stats.partial_slabs++;
    class_state->stats.backing_allocations++;
    slab_record_live_locked(class_state);
    slab_require_valid(slab_descriptor_valid_locked(slab, class_index),
                       "slab: invalid descriptor after publication");
    return (u8 *)slab + SLAB_OBJECT_AREA_OFFSET +
           (usize)slot * slab_sizes[class_index];
}

void *slab_alloc(usize size)
{
    u32 class_index;
    slab_class_t *class_state;
    void *backing;
    u64 flags;

    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return NULL;
    if (!slab_select_class(size, &class_index))
        return NULL;
    class_state = &slab_classes[class_index];

    flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    void *object = slab_take_existing_locked(class_index);
    if (object) {
        spin_unlock_irqrestore(&class_state->lock, flags);
        return object;
    }
    spin_unlock_irqrestore(&class_state->lock, flags);

    /* Backing allocation may grow the indexed heap and therefore must run
     * without a class lock held. */
    backing = heap_alloc_aligned_page();
    if (!backing) {
        flags = spin_lock_irqsave(&class_state->lock);
        class_state->stats.lock_acquisitions++;
        object = slab_take_existing_locked(class_index);
        if (!object) class_state->stats.allocation_failures++;
        spin_unlock_irqrestore(&class_state->lock, flags);
        return object;
    }
    usize backing_page_index;
    slab_require_valid(slab_page_index(backing, &backing_page_index),
                       "slab: heap returned invalid aligned backing");
    if (!slab_classifier_ensure(backing_page_index)) {
        heap_free_aligned_page(backing);
        flags = spin_lock_irqsave(&class_state->lock);
        class_state->stats.lock_acquisitions++;
        object = slab_take_existing_locked(class_index);
        if (!object) class_state->stats.allocation_failures++;
        spin_unlock_irqrestore(&class_state->lock, flags);
        return object;
    }
    slab_require_valid(slab_token_class(
                           slab_page_token_load(backing_page_index)) == 0U,
                       "slab: heap returned classified slab backing");
    /* This heap block is exclusively owned.  Prepare metadata before taking
     * the class lock; publish only after rechecking the partial list. */
    slab_initialize_descriptor((slab_descriptor_t *)backing, class_index);

    flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    object = slab_take_existing_locked(class_index);
    bool release_backing = object != NULL;
    if (!release_backing)
        object = slab_publish_new_locked(class_index, backing);
    else
        class_state->stats.backing_allocations++;
    if (release_backing) {
        class_state->stats.backing_releases++;
        spin_unlock_irqrestore(&class_state->lock, flags);
        heap_free_aligned_page(backing);
        return object;
    }
    spin_unlock_irqrestore(&class_state->lock, flags);
    return object;
}

bool slab_try_free(void *ptr)
{
    uintptr_t page;
    usize page_index;
    u64 initial_token;
    u32 class_index;
    slab_class_t *class_state;
    slab_descriptor_t *slab;
    uintptr_t object_start;
    uintptr_t object_address = (uintptr_t)ptr;
    usize offset;
    u16 slot;
    u64 flags;
    bool release_backing = false;

    if (!ptr || __atomic_load_n(&slab_initialization_state,
                                __ATOMIC_ACQUIRE) != 2U ||
        !slab_pointer_page(ptr, &page, &page_index))
        return false;
    initial_token = slab_page_token_load(page_index);
    u8 initial_class = slab_token_class(initial_token);
    if (!initial_class)
        return false;
    slab_require_valid(initial_class <= SLAB_CLASS_COUNT &&
                       (initial_token >> SLAB_CLASSIFIER_CLASS_BITS) != 0,
                       "slab: invalid page-classification value");
    class_index = (u32)initial_class - 1U;
    class_state = &slab_classes[class_index];

    flags = spin_lock_irqsave(&class_state->lock);
    class_state->stats.lock_acquisitions++;
    u64 current_token = slab_page_token_load(page_index);
    slab_require_valid(current_token == initial_token,
                       "slab: stale page classification during free");
    slab = (slab_descriptor_t *)page;
    slab_require_valid(slab_descriptor_valid_locked(slab, class_index),
                       "slab: corrupted descriptor during free");

    object_start = page + SLAB_OBJECT_AREA_OFFSET;
    slab_require_valid(object_address >= object_start,
                       "slab: pointer precedes object area");
    offset = object_address - object_start;
    slab_require_valid(!(offset % slab_sizes[class_index]),
                       "slab: interior or misaligned object free");
    slot = (u16)(offset / slab_sizes[class_index]);
    slab_require_valid(slot < slab->slot_count,
                       "slab: object pointer outside slot range");

    u64 *bitmap = slab_bitmap(slab);
    u64 mask = (u64)1U << (slot % 64U);
    slab_require_valid(bitmap[slot / 64U] & mask,
                       "slab: duplicate or unallocated object free");
    bool was_full = slab->state == SLAB_STATE_FULL;
    bool will_empty = slab->allocated_count == 1U;
    slab_require_valid(class_state->stats.live_slots != 0,
                       "slab: live-slot counter underflow");
    if (will_empty) {
        slab_require_valid(class_state->stats.partial_slabs != 0,
                           "slab: partial-slab counter underflow");
        slab_list_remove_locked(class_index, slab);
    } else if (was_full) {
        slab_require_valid(class_state->stats.full_slabs != 0,
                           "slab: full-slab counter underflow");
    }
    bitmap[slot / 64U] &= ~mask;
    slab->allocated_count--;
    class_state->stats.frees++;
    class_state->stats.live_slots--;

    if (will_empty) {
        slab->state = SLAB_STATE_EMPTY;
        slab_update_integrity(slab);
        class_state->stats.partial_slabs--;
        class_state->stats.retired_slabs++;
        class_state->stats.backing_releases++;
        slab_page_token_store(page_index,
            current_token & ~((u64)SLAB_CLASSIFIER_CLASS_MASK));
        release_backing = true;
    } else if (was_full) {
        slab->state = SLAB_STATE_PARTIAL;
        slab_update_integrity(slab);
        slab_list_insert_locked(class_index, slab);
        class_state->stats.full_slabs--;
        class_state->stats.partial_slabs++;
        class_state->stats.transitions_to_partial++;
    } else {
        slab->state = SLAB_STATE_PARTIAL;
        slab_update_integrity(slab);
    }

    if (!release_backing)
        slab_require_valid(slab_descriptor_valid_locked(slab, class_index),
                           "slab: invalid descriptor after free");
    spin_unlock_irqrestore(&class_state->lock, flags);
    if (release_backing)
        heap_free_aligned_page((void *)page);
    return true;
}

u16 slab_class_size(u32 class_index)
{
    return class_index < SLAB_CLASS_COUNT ? slab_sizes[class_index] : 0U;
}

u16 slab_class_slots(u32 class_index)
{
    return slab_slots_for_class(class_index);
}

bool slab_get_class_stats(u32 class_index, slab_class_stats_t *stats)
{
    if (class_index >= SLAB_CLASS_COUNT || !stats ||
        __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    u64 flags = spin_lock_irqsave(&slab_classes[class_index].lock);
    *stats = slab_classes[class_index].stats;
    spin_unlock_irqrestore(&slab_classes[class_index].lock, flags);
    return true;
}

bool slab_get_classifier_stats(slab_classifier_stats_t *stats)
{
    if (!stats ||
        __atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    *stats = (slab_classifier_stats_t){
        .root_payload_bytes = __atomic_load_n(
            &slab_classifier_root_payload_bytes, __ATOMIC_RELAXED),
        .middle_payload_bytes = __atomic_load_n(
            &slab_classifier_middle_payload_bytes, __ATOMIC_RELAXED),
        .leaf_payload_bytes = __atomic_load_n(
            &slab_classifier_leaf_payload_bytes, __ATOMIC_RELAXED),
        .retained_payload_bytes = __atomic_load_n(
            &slab_classifier_retained_payload_bytes, __ATOMIC_RELAXED),
        .peak_payload_bytes = __atomic_load_n(
            &slab_classifier_peak_payload_bytes, __ATOMIC_RELAXED),
        .root_tables = __atomic_load_n(&slab_classifier_root_tables,
                                       __ATOMIC_RELAXED),
        .middle_tables = __atomic_load_n(&slab_classifier_middle_tables,
                                         __ATOMIC_RELAXED),
        .leaf_tables = __atomic_load_n(&slab_classifier_leaf_tables,
                                       __ATOMIC_RELAXED),
        .allocation_failures = __atomic_load_n(
            &slab_classifier_allocation_failures, __ATOMIC_RELAXED),
    };
    return true;
}

bool slab_verify(void)
{
    if (__atomic_load_n(&slab_initialization_state, __ATOMIC_ACQUIRE) != 2U)
        return false;
    for (u32 class_index = 0; class_index < SLAB_CLASS_COUNT; class_index++) {
        slab_class_t *class_state = &slab_classes[class_index];
        u64 flags = spin_lock_irqsave(&class_state->lock);
        slab_descriptor_t *slab = class_state->partial_head;
        slab_descriptor_t *previous = NULL;
        usize partial_count = 0;
        usize mapped_partial_count = 0;
        usize full_count = 0;
        u64 live_count = 0;

        while (slab) {
            if (++partial_count > slab_heap_pages ||
                !slab_descriptor_valid_locked(slab, class_index) ||
                slab->state != SLAB_STATE_PARTIAL ||
                slab->partial_prev != previous) {
                spin_unlock_irqrestore(&class_state->lock, flags);
                return false;
            }
            live_count += slab->allocated_count;
            previous = slab;
            slab = slab->partial_next;
        }
        for (usize page_index = 0; page_index < slab_heap_pages;
             page_index++) {
            uintptr_t page_address = slab_heap_start +
                                     page_index * PAGE_SIZE;
            /* Heap pages are committed as one monotonic prefix.  Do not scan
             * the large uncommitted virtual reservation during diagnostics. */
            if (!heap_address_is_mapped((const void *)page_address))
                break;
            u64 token = slab_page_token_load(page_index);
            u8 tag = slab_token_class(token);
            if (tag > SLAB_CLASS_COUNT ||
                (tag && !(token >> SLAB_CLASSIFIER_CLASS_BITS))) {
                spin_unlock_irqrestore(&class_state->lock, flags);
                return false;
            }
            if (tag != class_index + 1U)
                continue;
            slab = (slab_descriptor_t *)page_address;
            if (!slab_descriptor_fields_valid(slab, class_index)) {
                spin_unlock_irqrestore(&class_state->lock, flags);
                return false;
            }
            if (slab->state == SLAB_STATE_FULL) {
                full_count++;
                live_count += slab->allocated_count;
            } else if (slab->state == SLAB_STATE_PARTIAL) {
                mapped_partial_count++;
            } else {
                spin_unlock_irqrestore(&class_state->lock, flags);
                return false;
            }
        }
        if (partial_count != mapped_partial_count ||
            partial_count != class_state->stats.partial_slabs ||
            full_count != class_state->stats.full_slabs ||
            live_count != class_state->stats.live_slots) {
            spin_unlock_irqrestore(&class_state->lock, flags);
            return false;
        }
        spin_unlock_irqrestore(&class_state->lock, flags);
    }
    return true;
}
