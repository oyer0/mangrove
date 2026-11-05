/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Appended to production block/GPT/mutex sources by storage_coherency_test.py.
 * Only the media backend, scheduler/IRQ fixture and VFS policy are simulated. */
static FILE *image_file;
static u64 image_sectors;
static u64 failing_lba = ~(u64)0;
static unsigned media_reads, media_failures, tail_reads;
static block_device_t image_parent;
static enum {
    IMAGE_UNCHANGED, PRIMARY_CRC, BACKUP_CRC, ARRAY_CRC,
    WRONG_ALTERNATE, WRONG_BACKUP_ID, DIFFERENT_ARRAYS, OVERLAPPING_ENTRIES,
    BLANK_IMAGE
} image_fault;
static u8 changed_entries[GPT_ENTRY_ARRAY_BYTES];
static u32 changed_crc;
static u64 entries_lba, alternate_lba, alternate_entries_lba;

static bool image_read(block_device_t *device, u64 lba, u32 count, void *buffer)
{
    u8 *bytes = buffer;
    assert(lba < device->sector_count && count <= device->sector_count - lba);
    media_reads++;
    if (lba == device->sector_count - 1U) tail_reads++;
    if (failing_lba >= lba && failing_lba - lba < count) {
        media_failures++;
        return false;
    }
    memset(bytes, 0, (usize)count * 512U);
    assert(fseek(image_file, (long)(lba * 512U), SEEK_SET) == 0);
    (void)fread(bytes, 1, (usize)count * 512U, image_file);
    assert(!ferror(image_file)); /* The larger backing's unwritten tail is zero. */
    if (image_fault == BLANK_IMAGE) {
        memset(bytes, 0, (usize)count * 512U);
        return true;
    }
    if (lba == 1U || lba == alternate_lba) {
        if ((image_fault == PRIMARY_CRC && lba == 1U) ||
            (image_fault == BACKUP_CRC && lba == alternate_lba)) bytes[16] ^= 1U;
        if (image_fault == WRONG_ALTERNATE && lba == 1U) {
            store_le64(bytes + 32U, alternate_lba - 1U);
            store_le32(bytes + 16U, crc32(bytes, le32(bytes + 12U), 16U));
        }
        if (image_fault == WRONG_BACKUP_ID && lba == alternate_lba) {
            bytes[56] ^= 1U;
            store_le32(bytes + 16U, crc32(bytes, le32(bytes + 12U), 16U));
        }
        if (image_fault == OVERLAPPING_ENTRIES ||
            (image_fault == DIFFERENT_ARRAYS && lba == alternate_lba)) {
            store_le32(bytes + 88U, changed_crc);
            store_le32(bytes + 16U, crc32(bytes, le32(bytes + 12U), 16U));
        }
    }
    if (image_fault == ARRAY_CRC && lba == entries_lba) bytes[56] ^= 1U;
    if ((image_fault == DIFFERENT_ARRAYS && lba == alternate_entries_lba) ||
        (image_fault == OVERLAPPING_ENTRIES &&
         (lba == entries_lba || lba == alternate_entries_lba))) {
        assert(count == GPT_ENTRY_ARRAY_SECTORS);
        memcpy(bytes, changed_entries, sizeof(changed_entries));
    }
    return true;
}

static void setup_image(u64 capacity)
{
    assert(block_device_count() == 0U);
    image_parent = (block_device_t){.type = BLOCK_DEVICE_USB,
        .sector_size = 512U, .sector_count = capacity,
        .read = image_read, .driver_data = image_file};
    media_reads = media_failures = tail_reads = 0;
    failing_lba = ~(u64)0;
    image_fault = IMAGE_UNCHANGED;
}

static void remove_image(void)
{
    assert(gpt_remove_partitions_for_parent(&image_parent));
    if (image_parent.registered) assert(block_unregister_by_id(image_parent.id));
    assert(block_device_count() == 0U);
    partition_count = 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    kernel_thread_t thread = {.state=THREAD_STATE_RUNNING,
                             .condition=PTHREAD_COND_INITIALIZER};
    self = &thread;
    image_file = fopen(argv[1], "rb"); assert(image_file);
    assert(fseek(image_file, 0, SEEK_END) == 0);
    long image_bytes = ftell(image_file); assert(image_bytes > 0 && image_bytes % 512 == 0);
    image_sectors = (u64)image_bytes / 512U;
    u8 header[512];
    assert(fseek(image_file, 512, SEEK_SET) == 0 && fread(header, 1, 512, image_file) == 512);
    entries_lba = le64(header + 72U); alternate_lba = le64(header + 32U);
    assert(alternate_lba == image_sectors - 1U);
    assert(fseek(image_file, (long)(alternate_lba * 512U), SEEK_SET) == 0);
    assert(fread(header, 1, 512, image_file) == 512);
    alternate_entries_lba = le64(header + 72U);

    setup_image(image_sectors);
    gpt_table_t table;
    assert(gpt_load(&image_parent, &table) == VFS_OK && table.info.partition_count == 2U);
    assert(media_reads == 5U && media_failures == 0U && tail_reads == 1U);
    /* USB's real publication order: scan first, then publish/bind the parent. */
    assert(!image_parent.registered && gpt_scan_device(&image_parent));
    assert(gpt_result_calls == 1U && last_gpt_result == VFS_OK);
    assert(block_device_count() == 2U);
    assert(block_register(&image_parent) && block_device_count() == 3U);
    unsigned roots = 0;
    for (u32 i = 0; i < block_device_count(); i++) {
        block_device_t *device = block_get_device(i);
        gpt_partition_info_t part;
        if (!gpt_get_partition_info(device, &part)) continue;
        assert(part.parent_id == image_parent.id &&
               gpt_partition_parent_matches(&part, &image_parent));
        if (strcmp(part.name, GPT_MANGROVE_ROOT_NAME) == 0) {
            u8 sector[512], expected[512]; roots++;
            assert(part.first_lba == 133120U && part.last_lba == 264191U);
            assert(device->sector_count == 131072U && block_read(device, 0, 1, sector));
            assert(fseek(image_file, (long)(part.first_lba * 512U), SEEK_SET) == 0);
            assert(fread(expected, 1, 512, image_file) == 512 && !memcmp(expected, sector, 512));
        }
    }
    assert(roots == 1U && media_failures == 0U);
    remove_image();

    /* Actual small image on SanDisk capacity. Successful tail reads are not
     * evidence of a valid backup header or a completed partition scan. */
    u64 larger = image_sectors < 30629376U ? 30629376U : image_sectors + 1U;
    setup_image(larger);
    assert(gpt_load(&image_parent, &table) == VFS_ERR_BAD_FORMAT);
    assert(media_reads == 3U && media_failures == 0U && tail_reads == 1U);
    assert(!gpt_scan_device(&image_parent) && block_device_count() == 0U);
    assert(last_gpt_result == VFS_ERR_BAD_FORMAT);
    assert(block_register(&image_parent) && block_device_count() == 1U);
    gpt_table_info_t info;
    assert(gpt_get_table_info(&image_parent, &info) == VFS_OK && info.type == GPT_TABLE_CORRUPT);
    assert(media_failures == 0U);
    printf("capacity mismatch: image=%llu reported=%llu alternate=%llu GPT rc=%d; all reads succeeded, no partitions\n",
           (unsigned long long)image_sectors, (unsigned long long)larger,
           (unsigned long long)alternate_lba, VFS_ERR_BAD_FORMAT);
    remove_image();

    setup_image(image_sectors);
    const u64 failures[] = {0U, 1U, image_sectors - 1U, entries_lba, alternate_entries_lba};
    for (u32 i = 0; i < sizeof(failures)/sizeof(failures[0]); i++) {
        failing_lba = failures[i];
        assert(gpt_load(&image_parent, &table) == VFS_ERR_IO);
        assert(!gpt_scan_device(&image_parent) && last_gpt_result == VFS_ERR_IO);
    }
    failing_lba = ~(u64)0;
    for (unsigned fault = PRIMARY_CRC; fault <= OVERLAPPING_ENTRIES; fault++) {
        image_fault = fault;
        assert(fseek(image_file, (long)(entries_lba * 512U), SEEK_SET) == 0);
        assert(fread(changed_entries, 1, sizeof(changed_entries), image_file) == sizeof(changed_entries));
        if (fault == DIFFERENT_ARRAYS) changed_entries[56] ^= 1U;
        if (fault == OVERLAPPING_ENTRIES)
            store_le64(changed_entries + GPT_PARTITION_ENTRY_BYTES + 32U, 2048U);
        changed_crc = crc32(changed_entries, sizeof(changed_entries), ~(u32)0U);
        assert(gpt_load(&image_parent, &table) == VFS_ERR_BAD_FORMAT);
        assert(!gpt_scan_device(&image_parent) && block_device_count() == 0U);
    }
    remove_image();
    setup_image(image_sectors);
    image_fault = BLANK_IMAGE;
    assert(!gpt_scan_device(&image_parent) && last_gpt_result == VFS_ERR_NOT_FOUND);
    assert(media_reads == 3U && media_failures == 0U);
    image_parent.sector_size = 4096U;
    media_reads = 0;
    assert(!gpt_scan_device(&image_parent) && last_gpt_result == VFS_ERR_UNSUPPORTED);
    assert(media_reads == 0U);
    remove_image();
    assert(fclose(image_file) == 0);
    puts("PASS: actual image GPT, late parent binding, root partition reads, larger capacity rejection, I/O errors, CRC/identity/array/overlap negative controls");
}
