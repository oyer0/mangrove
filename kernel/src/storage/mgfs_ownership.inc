/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Temporary mount certificate. Ranges include all allocated Records, not
 * merely reachable files or bytes below EOF. No certificate survives an
 * uncertain mutation or authorizes repairing media. */
#define MGFS_PROOF_BUDGET (8ULL * 1024ULL * 1024ULL)
#define MGFS_PROOF_PAGE 4096U
#define MGFS_PROOF_PAGES (MGFS_PROOF_BUDGET / MGFS_PROOF_PAGE)
#define MGFS_PROOF_ITEMS (MGFS_PROOF_PAGE / sizeof(mgfs_owned_range_t))

typedef struct {
    mgfs_owned_range_t *pages[MGFS_PROOF_PAGES];
    u64 count;
} mgfs_proof_vector_t;

typedef struct mgfs_proof_names {
    struct mgfs_proof_names *next;
    usize used;
    char bytes[MGFS_PROOF_PAGE - sizeof(void *) - sizeof(usize)];
} mgfs_proof_names_t;

typedef struct {
    u64 live, peak;
    mgfs_ownership_result_t result;
    mgfs_proof_vector_t ranges, names;
    mgfs_proof_names_t *arena;
    u64 *parents;
    u8 *colors;
} mgfs_proof_t;

static void *mgfs_proof_alloc(mgfs_proof_t *proof, u64 bytes)
{
    if (!bytes || bytes > (u64)(usize)-1 ||
        bytes > MGFS_PROOF_BUDGET - proof->live) {
        proof->result = MGFS_OWNERSHIP_LIMIT;
        return NULL;
    }
    void *memory = kmalloc((usize)bytes);
    if (!memory) { proof->result = MGFS_OWNERSHIP_NO_MEM; return NULL; }
    proof->live += bytes;
    if (proof->live > proof->peak) proof->peak = proof->live;
    return memory;
}

static void mgfs_proof_free(mgfs_proof_t *proof, void *memory, u64 bytes)
{
    if (memory) { kfree(memory); proof->live -= bytes; }
}

static mgfs_owned_range_t *mgfs_proof_item(mgfs_proof_vector_t *vector, u64 i)
{
    return &vector->pages[i / MGFS_PROOF_ITEMS][i % MGFS_PROOF_ITEMS];
}

static bool mgfs_proof_push(mgfs_proof_t *proof, mgfs_proof_vector_t *vector,
                            mgfs_owned_range_t item)
{
    u64 page = vector->count / MGFS_PROOF_ITEMS;
    if (page >= MGFS_PROOF_PAGES) {
        proof->result = MGFS_OWNERSHIP_LIMIT;
        return false;
    }
    if (vector->count % MGFS_PROOF_ITEMS == 0) {
        vector->pages[page] = mgfs_proof_alloc(proof, MGFS_PROOF_PAGE);
        if (!vector->pages[page]) return false;
    }
    *mgfs_proof_item(vector, vector->count++) = item;
    return true;
}

static void mgfs_proof_clear(mgfs_proof_t *proof, mgfs_proof_vector_t *vector)
{
    for (u64 i = 0; i < mgfs_ceil_div(vector->count, MGFS_PROOF_ITEMS); i++) {
        mgfs_proof_free(proof, vector->pages[i], MGFS_PROOF_PAGE);
        vector->pages[i] = NULL;
    }
    vector->count = 0;
}

static mgfs_ownership_result_t mgfs_proof_claim(void *context,
                                               mgfs_owned_range_t range)
{
    mgfs_proof_t *proof = context;
    return mgfs_proof_push(proof, &proof->ranges, range)
        ? MGFS_OWNERSHIP_VALID : proof->result;
}

static int mgfs_proof_compare(mgfs_owned_range_t a, mgfs_owned_range_t b,
                              bool names)
{
    if (!names) return a.first < b.first ? -1 : a.first != b.first;
    u64 n = a.end < b.end ? a.end : b.end;
    int order = memcmp((void *)(uintptr_t)a.first, (void *)(uintptr_t)b.first,
                       (usize)n);
    return order ? order : a.end < b.end ? -1 : a.end != b.end;
}

static void mgfs_proof_sift(mgfs_proof_vector_t *vector, u64 root, u64 count,
                            bool names)
{
    while (root < count / 2) {
        u64 child = root * 2 + 1;
        if (child + 1 < count && mgfs_proof_compare(
                *mgfs_proof_item(vector, child),
                *mgfs_proof_item(vector, child + 1), names) < 0) child++;
        if (mgfs_proof_compare(*mgfs_proof_item(vector, root),
                               *mgfs_proof_item(vector, child), names) >= 0) break;
        mgfs_owned_range_t swap = *mgfs_proof_item(vector, root);
        *mgfs_proof_item(vector, root) = *mgfs_proof_item(vector, child);
        *mgfs_proof_item(vector, child) = swap;
        root = child;
    }
}

static void mgfs_proof_sort(mgfs_proof_vector_t *vector, bool names)
{
    for (u64 i = vector->count / 2; i; i--)
        mgfs_proof_sift(vector, i - 1, vector->count, names);
    for (u64 end = vector->count; end > 1;) {
        mgfs_owned_range_t swap = *mgfs_proof_item(vector, 0);
        *mgfs_proof_item(vector, 0) = *mgfs_proof_item(vector, --end);
        *mgfs_proof_item(vector, end) = swap;
        mgfs_proof_sift(vector, 0, end, names);
    }
}

static u64 mgfs_proof_record_index(mgfs_fs_t *fs, u64 id)
{
    u64 mask = fs->record_id_capacity - 1;
    u64 i = mgfs_hash_id(id) & mask;
    while (fs->record_ids[i].id && fs->record_ids[i].id != id)
        i = (i + 1) & mask;
    return i;
}

static bool mgfs_proof_name(mgfs_proof_t *proof,
                            const mgfs_directory_entry_t *entry)
{
    if (!proof->arena || entry->name_length >
        sizeof(proof->arena->bytes) - proof->arena->used) {
        mgfs_proof_names_t *page = mgfs_proof_alloc(proof, sizeof(*page));
        if (!page) return false;
        page->next = proof->arena;
        page->used = 0;
        proof->arena = page;
    }
    char *name = proof->arena->bytes + proof->arena->used;
    memcpy(name, entry->name, (usize)entry->name_length);
    proof->arena->used += (usize)entry->name_length;
    return mgfs_proof_push(proof, &proof->names,
        (mgfs_owned_range_t){(u64)(uintptr_t)name, entry->name_length});
}

static void mgfs_proof_clear_names(mgfs_proof_t *proof)
{
    mgfs_proof_clear(proof, &proof->names);
    while (proof->arena) {
        mgfs_proof_names_t *next = proof->arena->next;
        mgfs_proof_free(proof, proof->arena, sizeof(*proof->arena));
        proof->arena = next;
    }
}

static mgfs_ownership_result_t mgfs_complete_ownership_proof(mgfs_fs_t *fs,
                                                            bool read_only)
{
    mgfs_proof_t *proof = kmalloc(sizeof(*proof));
    mgfs_ownership_scan_t *scan = NULL;
    mgfs_extent_desc_t *mapping = NULL;
    u64 mapping_bytes = 0;
    mgfs_ownership_result_t result = MGFS_OWNERSHIP_NO_MEM;
    if (!proof) return result;
    memset(proof, 0, sizeof(*proof));
    proof->live = proof->peak = sizeof(*proof);
    proof->result = MGFS_OWNERSHIP_VALID;
    scan = mgfs_proof_alloc(proof, sizeof(*scan));
    if (!scan) goto done;
    memset(scan, 0, sizeof(*scan));
    scan->fs = fs;
    scan->claim_all = mgfs_proof_claim;
    scan->claim_context = proof;
    result = MGFS_OWNERSHIP_CORRUPT;
    if (fs->record_id_capacity > MGFS_PROOF_BUDGET / sizeof(u64)) {
        proof->result = MGFS_OWNERSHIP_LIMIT;
        goto done;
    }
    proof->parents = mgfs_proof_alloc(proof, fs->record_id_capacity * sizeof(u64));
    proof->colors = mgfs_proof_alloc(proof, fs->record_id_capacity);
    if (!proof->parents || !proof->colors) goto done;
    memset(proof->parents, 0, (usize)(fs->record_id_capacity * sizeof(u64)));
    memset(proof->colors, 0, (usize)fs->record_id_capacity);

    for (u64 i = 0; i < fs->record_id_capacity; i++) {
        u8 record[MGFS_RECORD_BYTES];
        u64 id = fs->record_ids[i].id;
        if (!id) continue;
        if (id >= fs->next_record_id) goto done;
        if (!mgfs_read_record(fs, id, record)) { result = MGFS_OWNERSHIP_IO; goto done; }
        result = mgfs_ownership_scan_record(scan, record, true);
        if (result != MGFS_OWNERSHIP_VALID) goto done;
        result = MGFS_OWNERSHIP_CORRUPT;
        if (mgfs_get_le64(record) != MGFS_RECORD_DIRECTORY) continue;
        u64 capacity = mgfs_get_le64(record + 40), count = 0;
        if (capacity > MGFS_PROOF_BUDGET / sizeof(*mapping)) {
            proof->result = MGFS_OWNERSHIP_LIMIT; goto done;
        }
        mapping_bytes = capacity * sizeof(*mapping);
        if (capacity) {
            mapping = mgfs_proof_alloc(proof, mapping_bytes);
            if (!mapping) goto done;
            if (!mgfs_collect_file_extents(fs, record, mapping, capacity, &count,
                                           MGFS_EXTENT_DIRECTORY_METADATA)) {
                result = MGFS_OWNERSHIP_IO; goto done;
            }
        }
        u64 size = mgfs_get_le64(record + 32), offset = 0;
        while (offset < size) {
            mgfs_directory_entry_t entry;
            if (!mgfs_read_directory_entry_mapped(fs, record, offset, &entry,
                                                   mapping, count)) {
                result = MGFS_OWNERSHIP_CORRUPT; goto done;
            }
            offset += entry.total_bytes;
            if (entry.flags != MGFS_DIRENT_IN_USE) continue;
            u64 child = mgfs_proof_record_index(fs, entry.target_record_id);
            if (!fs->record_ids[child].id ||
                entry.target_record_id == fs->root_record_id ||
                proof->parents[child]) goto done;
            proof->parents[child] = i + 1; /* zero denotes no incoming reference */
            if (!mgfs_proof_name(proof, &entry)) goto done;
        }
        mgfs_proof_sort(&proof->names, true);
        for (u64 n = 1; n < proof->names.count; n++)
            if (!mgfs_proof_compare(*mgfs_proof_item(&proof->names, n - 1),
                                    *mgfs_proof_item(&proof->names, n), true)) goto done;
        mgfs_proof_clear_names(proof);
        mgfs_proof_free(proof, mapping, mapping_bytes);
        mapping = NULL; mapping_bytes = 0;
    }
    /* Parent graph validation includes unreachable directory cycles. Every
     * vertex is colored once; stack depth and total work stay bounded. */
    for (u64 i = 0; i < fs->record_id_capacity; i++) {
        if (!fs->record_ids[i].id || proof->colors[i]) continue;
        u64 p = i;
        while (!proof->colors[p]) {
            proof->colors[p] = 1;
            if (!proof->parents[p]) break;
            p = proof->parents[p] - 1;
        }
        if (proof->parents[p] && proof->colors[p] == 1) goto done;
        p = i;
        while (proof->colors[p] == 1) {
            proof->colors[p] = 2;
            if (!proof->parents[p]) break;
            p = proof->parents[p] - 1;
        }
    }
    mgfs_proof_sort(&proof->ranges, false);
    u64 previous_end = 0;
    scan->claim_all = NULL;
    for (u64 i = 0; i < proof->ranges.count; i++) {
        mgfs_owned_range_t range = *mgfs_proof_item(&proof->ranges, i);
        if (!read_only && range.first < previous_end) goto done;
        if (range.end > previous_end) previous_end = range.end;
        /* Sorted claims reuse bitmap blocks instead of one bitmap read per
         * extent in arbitrary Record order. This also validates free bits. */
        result = mgfs_ownership_claim(scan, range.first, range.end - range.first, false);
        if (result != MGFS_OWNERSHIP_VALID) goto done;
        result = MGFS_OWNERSHIP_CORRUPT;
    }
    result = MGFS_OWNERSHIP_VALID;
done:
    if (proof->result != MGFS_OWNERSHIP_VALID) result = proof->result;
    fs->ownership_proof_peak = proof->peak;
    fs->ownership_proof_ranges = proof->ranges.count;
    mgfs_proof_free(proof, mapping, mapping_bytes);
    mgfs_proof_clear_names(proof);
    mgfs_proof_clear(proof, &proof->ranges);
    mgfs_proof_free(proof, proof->parents, fs->record_id_capacity * sizeof(u64));
    mgfs_proof_free(proof, proof->colors, fs->record_id_capacity);
    mgfs_proof_free(proof, scan, sizeof(*scan));
    kfree(proof);
    return result;
}
