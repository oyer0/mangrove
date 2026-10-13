/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production scanner with generated, read-only media; no duplicated scan logic. */
#define main fat32_existing_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

enum { LARGE_FILE, DEEP_TREE, WIDE_TREE, LONG_DIRECTORY };
static unsigned layout;
static u32 units, root_clusters;
static bool fragmented;
static u32 cross_link, cycle_at;
static unsigned fail_scan_read, remove_scan_read, replace_scan_read;
static bool mirror_conflict;
static unsigned wide_allocations;

static u32 generated_fat(u32 cluster)
{
    if (cluster == 2U && layout != WIDE_TREE && layout != LONG_DIRECTORY)
        return FAT32_EOC_MIN;
    if (layout == LARGE_FILE) {
        if (cluster < 3U || cluster >= 3U + units) return 0U;
        if (cluster == cycle_at) return 3U;
        if (!fragmented)
            return cluster == units + 2U ? FAT32_EOC_MIN : cluster + 1U;
        u32 half = units / 2U;
        if (cluster < 3U + half) return cluster + half;
        return cluster == units + 2U ? FAT32_EOC_MIN : cluster - half + 1U;
    }
    if (layout == DEEP_TREE)
        return cluster < 2U + units ? FAT32_EOC_MIN : 0U;
    if (cluster < 2U || cluster >= 2U + root_clusters +
            (layout == WIDE_TREE ? units : 0U)) return 0U;
    if (cluster < root_clusters + 1U) return cluster + 1U;
    return FAT32_EOC_MIN;
}

static void generated_entry(u8 *buffer, u32 slot, const char *name,
                            u32 first, u32 size, bool directory)
{
    fat32_on_disk_entry_t *entry = (void *)(buffer + slot * 32U);
    memcpy(entry->name, name, 11U);
    entry->attr = directory ? 0x10U : 0x20U;
    entry->first_cluster_low = (u16)first;
    entry->first_cluster_high = (u16)(first >> 16U);
    entry->file_size = size;
}

static bool generated_read(block_device_t *dev, u64 id, u64 lba,
                            u32 count, void *buffer)
{
    if (!online || dev != &device || id != device.id || count != 1U)
        return false;
    reads++;
    if (reads == fail_scan_read) return false;
    u8 *bytes = buffer;
    memset(bytes, 0, 512U);
    if (lba >= fs.fat_start_lba &&
        lba < (u64)fs.fat_start_lba + (u64)fs.fat_size_sectors * fs.fat_count) {
        fat_reads++;
        u32 start = (u32)((lba - fs.fat_start_lba) % fs.fat_size_sectors) * 128U;
        for (u32 i = 0U; i < 128U; i++)
            fat32_store_le32(bytes + i * 4U,
                mirror_conflict && lba >= fs.fat_start_lba + fs.fat_size_sectors &&
                start + i == FAT32_SCAN_CHUNK_CLUSTERS + 3U
                    ? 0U : generated_fat(start + i));
    } else {
        if (lba < fs.data_start_lba || lba >= device.sector_count) return false;
        directory_reads++;
        u32 cluster = (u32)(lba - fs.data_start_lba) + 2U;
        if (layout == LARGE_FILE) {
            assert(cluster == 2U);
            u64 capacity = (u64)units * 512U;
            generated_entry(bytes, 0U, "LARGE   BIN", 3U,
                capacity > 0xffffffffULL ? 0xffffffffU : (u32)capacity, false);
            if (cross_link)
                generated_entry(bytes, 1U, "SHARED  BIN", cross_link, 1U, false);
        } else if (layout == DEEP_TREE) {
            u32 slot = 0U;
            if (cluster != 2U) {
                generated_entry(bytes, slot++, ".          ", cluster, 0U, true);
                generated_entry(bytes, slot++, "..         ", cluster - 1U, 0U, true);
            }
            if (cluster < units + 1U)
                generated_entry(bytes, slot, "SUB        ", cluster + 1U, 0U, true);
            else if (cross_link)
                generated_entry(bytes, slot, "LOOP       ", cross_link, 0U, true);
        } else if (layout == WIDE_TREE) {
            if (cluster < 2U + root_clusters) {
                u32 base = (cluster - 2U) * 16U;
                for (u32 i = 0U; i < 16U && base + i < units; i++) {
                    char name[16];
                    snprintf(name, sizeof(name), "D%07u   ", base + i);
                    generated_entry(bytes, i, name,
                        2U + root_clusters + base + i, 0U, true);
                }
            } else {
                generated_entry(bytes, 0U, ".          ", cluster, 0U, true);
                generated_entry(bytes, 1U, "..         ", 0U, 0U, true);
            }
        }
    }
    if (reads == remove_scan_read) online = false;
    if (reads == replace_scan_read) device.id++;
    return true;
}

static void generated_setup(unsigned kind, u32 count, bool fragment)
{
    setup(); layout = kind; units = count; fragmented = fragment;
    cross_link = cycle_at = 0U;
    fail_scan_read = remove_scan_read = replace_scan_read = 0U;
    mirror_conflict = false;
    root_clusters = kind == WIDE_TREE ? count / 16U + 1U : count;
    fs.total_clusters = kind == WIDE_TREE ? count + root_clusters + 2U : count + 4U;
    if (fs.total_clusters < 65527U) fs.total_clusters = 65527U;
    fs.fat_size_sectors = (fs.total_clusters + 127U) / 128U;
    fs.fat_start_lba = 1U;
    fs.data_start_lba = fs.fat_start_lba + fs.fat_size_sectors;
    device.sector_count = (u64)fs.data_start_lba + fs.total_clusters - 2U;
    scaling_read = generated_read;
}

static void successful_scan(unsigned kind, u32 count, bool fragment)
{
    generated_setup(kind, count, fragment);
    vfs_node_t target = {.super = &super, .type = VFS_TYPE_FILE};
    fat32_node_state_t state = {.node = &target, .first_cluster = 3U,
        .parent_cluster = 2U, .entry_index = 0U, .alive = true,
        .version_valid = true, .generation = 1U};
    target.fs_data = &state;
    u64 length = (u64)count * 512U;
    target.size = length > 0xffffffffULL ? 0xffffffffU : length;
    clock_t start = clock();
    assert(fat32_ownership_preflight_locked(kind == LARGE_FILE ? &target : &root)
           == VFS_OK);
    double milliseconds = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    printf("FAT32 scalable proof kind=%u units=%u fragmented=%u: "
           "peak=%zu allocations=%u reads=%u FAT_reads=%u cpu_ms=%.3f\n",
           kind, count, fragment, (size_t)peak_requested, allocations,
           reads, fat_reads, milliseconds);
    assert(!writes && !live_requested && fs.version_confident &&
           peak_requested <= FAT32_SCAN_MEMORY_BUDGET);
    /* Mirrored sector views cache contiguous runs; fragmentation remains linear. */
    if (kind == LARGE_FILE)
        assert(fat_reads <= (fragment ? count / 64U + 8U : count / 128U + 3U));
    if (kind == DEEP_TREE) assert(allocations <= 6U);
    if (kind == WIDE_TREE) wide_allocations = allocations;
    cleanup();
}

#ifndef FAT32_SCALING_NO_MAIN
int main(int argc, char **argv)
{
    if (argc == 2) { disposable_image_case(argv[1]); return 0; }
    successful_scan(LARGE_FILE, 262144U, false); /* Exact 128 MiB regression. */
    successful_scan(LARGE_FILE, 1048576U, true); /* Fragmented 512 MiB. */
    successful_scan(LARGE_FILE, 8388608U, false); /* Maximum FAT32 file size. */
    successful_scan(DEEP_TREE, 12000U, false);
    successful_scan(WIDE_TREE, 70000U, false); /* >4,096 queue and >65,536 slots. */
    successful_scan(LONG_DIRECTORY, 6000U, false);

    for (unsigned fail = 4U; fail <= wide_allocations; fail += wide_allocations - 4U) {
        generated_setup(WIDE_TREE, 70000U, false); fail_allocation = fail;
        assert(fat32_ownership_preflight_locked(&root) == VFS_ERR_NO_MEM);
        assert(!writes && !live_requested && fs.version_confident); cleanup();
    }
    generated_setup(DEEP_TREE, 12000U, false); cross_link = 3U;
    assert(fat32_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!writes && !live_requested && !fs.version_confident); cleanup();

    generated_setup(LARGE_FILE, 262144U, true);
    assert(fat32_ownership_preflight_locked(&root) == VFS_OK);
    unsigned total_allocations = allocations, total_reads = reads;
    cleanup();
    for (unsigned fail = 1U; fail <= total_allocations; fail++) {
        generated_setup(LARGE_FILE, 262144U, true);
        fail_allocation = fail;
        assert(fat32_ownership_preflight_locked(&root) == VFS_ERR_NO_MEM);
        assert(!writes && !live_requested && fs.version_confident);
        cleanup();
    }
    /* Fail at the start, middle and end of a large scan. */
    for (unsigned fail = 1U; fail <= total_reads; fail += total_reads / 2U) {
        generated_setup(LARGE_FILE, 262144U, true); fail_scan_read = fail;
        assert(fat32_ownership_preflight_locked(&root) == VFS_ERR_IO);
        assert(!writes && !live_requested && fs.version_confident); cleanup();
    }
    for (unsigned mode = 0U; mode < 4U; mode++) {
        generated_setup(LARGE_FILE, 262144U, true);
        if (mode == 0U) cross_link = FAT32_SCAN_CHUNK_CLUSTERS + 3U;
        if (mode == 1U) cycle_at = units + 2U;
        if (mode == 2U) remove_scan_read = 40U;
        if (mode == 3U) replace_scan_read = 40U;
        assert(fat32_ownership_preflight_locked(&root) ==
               (mode < 2U ? VFS_ERR_BAD_FORMAT : VFS_ERR_IO));
        assert(!writes && !live_requested && fs.version_confident == (mode >= 2U));
        cleanup();
    }

    for (unsigned corrupt = 0U; corrupt < 2U; corrupt++) {
        generated_setup(LARGE_FILE, 262144U, true);
        fs.fat_count = 2U; fs.data_start_lba = 1U + 2U * fs.fat_size_sectors;
        device.sector_count = (u64)fs.data_start_lba + fs.total_clusters - 2U;
        mirror_conflict = corrupt;
        assert(fat32_ownership_preflight_locked(&root) ==
               (corrupt ? VFS_ERR_BAD_FORMAT : VFS_OK));
        assert(!writes && !live_requested && fs.version_confident == !corrupt);
        cleanup();
    }

    /* Actual chunk allocation reaches the shared budget, without media writes. */
    setup(); fs.total_clusters = FAT32_EOC_MIN - 1U;
    fat32_ownership_scan_t scan = {.fs = &fs};
    scan.chunk_count = (fs.total_clusters - 1U) / FAT32_SCAN_CHUNK_CLUSTERS + 1U;
    usize index_bytes = (usize)scan.chunk_count * sizeof(*scan.owned);
    scan.owned = fat32_scan_alloc(&scan, index_bytes); assert(scan.owned);
    memset(scan.owned, 0, index_bytes);
    int result = VFS_OK;
    for (u32 i = 0U; i < scan.chunk_count && result == VFS_OK; i++)
        result = fat32_claim_cluster(&scan, i * FAT32_SCAN_CHUNK_CLUSTERS + 2U);
    assert(result == VFS_ERR_BUFFER_TOO_SMALL && !writes &&
           peak_requested <= FAT32_SCAN_MEMORY_BUDGET);
    fat32_scan_free(&scan); assert(!live_requested); cleanup();
    puts("FAT32 scalable ownership proof passed");
    return 0;
}
#endif
