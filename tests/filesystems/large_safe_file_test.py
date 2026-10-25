#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""8 MiB production safe-file transactions on all three storage backends.
Controlled media/heap/pin adapters are shared with each backend's fault suite.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/shared'))
from host_build import dead_strip_flags

COMMON = r'''
#define LARGE_BYTES (8U * 1024U * 1024U + 17U)
static u64 delivered;
static int verify(void *context, u64 offset, const u8 *data, usize count) {
    (void)context; assert(offset == delivered && count <= 4096);
    for (usize i = 0; i < count; i++) assert(data[i] == (u8)((offset + i) % 251));
    delivered += count; return VFS_OK;
}
static u32 pattern(void *context, u64 offset, u8 *data, u32 count) {
    (void)context;
    for (u32 i = 0; i < count; i++) data[i] = (u8)((offset + i) % 251);
    return count;
}
'''
FAT = r'''
int main(void) {
    setup(); fs.total_clusters = 60000; fs.fat_size_sectors = 469;
    fs.data_start_lba = 600; device.sector_count = TEST_MEDIA_SECTORS;
    fat32_on_disk_entry_t *disk = (void *)media[600];
    memcpy(disk[0].name,"TARGET  TXT",11); disk[0].attr=0x20;
    disk[0].first_cluster_low=3;disk[0].file_size=3;
    memcpy(media[601],"old",3);
    vfs_node_t *file=fat32_finddir(&root,"TARGET.TXT");assert(file);
    u64 pin=0; unsigned before=writes;
    fail_allocation=allocations+1;
    assert(fat32_replace_locked(&super,file->inode,1,LARGE_BYTES,pattern,NULL,&pin)==VFS_ERR_NO_MEM);
    assert(writes==before && !pin); fail_allocation=0;
    assert(fat32_replace_locked(&super,file->inode,1,LARGE_BYTES,pattern,NULL,&pin)==VFS_OK);
    assert(pin && vfs_recovery_pin_finish_handoff(pin)==VFS_OK);
    mg_file_lines_state_t state={0}; delivered=0;
    assert(fat32_snapshot_stream(file,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    before=writes;
    assert(fat32_replace_locked(&super,file->inode,1,LARGE_BYTES,pattern,NULL,&pin)==VFS_ERR_STALE_GENERATION);
    assert(writes==before && !pin);
    vfs_node_t *created=kmalloc(sizeof(*created));assert(created);memset(created,0,sizeof(*created));
    vfs_file_facts_t facts;
    assert(fat32_create_contents_locked(&root,created,"LARGE.TXT",LARGE_BYTES,pattern,NULL,&facts,&pin)==VFS_OK);
    created->inode=facts.record_id;created->type=VFS_TYPE_FILE;created->size=facts.size;created->super=&super;created->ops=&fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin)==VFS_OK);
    delivered=0; assert(fat32_snapshot_stream(created,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    printf("FAT32 8 MiB replace/create/reopen/conflict/allocation: peak=%zu reads=%u writes=%u\n",(size_t)peak_requested,reads,writes);
    cleanup();assert(!live_requested);return 0;
}
'''
EXFAT = r'''
int main(void) {
    setup_large();
    fs.boot.cluster_count=60000;fs.boot.volume_length=TEST_MEDIA_SECTORS;
    fs.boot.fat_length=469;fs.boot.heap_offset=600;device.sector_count=TEST_MEDIA_SECTORS;
    fs.bitmap_length=7500;fs.upcase_cluster=18;
    memcpy(media[600],media[120],512);
    exfat_put64(media[600]+96+24,fs.bitmap_length);
    exfat_put32(media[600]+128+20,18);
    media[601][0]=0xff;media[601][1]=0xff;media[601][2]=1;
    for(u32 c=3;c<=18;c++) exfat_put32(media[24+c/128]+c%128*4,c==17||c==18?EXFAT_CLUSTER_EOC:c+1);
    vfs_node_t *created=prepared_created_node();vfs_file_facts_t facts;u64 pin=0;
    assert(exfat_create_contents_locked(&root,created,"large.txt",LARGE_BYTES,pattern,NULL,&facts,&pin)==VFS_OK);
    bind_created_node(created,&facts);finish_pin();
    mg_file_lines_state_t state={0};delivered=0;
    assert(exfat_snapshot_stream(created,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    unsigned before=writes;fail_allocation=allocations+1;
    assert(exfat_replace_locked(&super,created->inode,1,LARGE_BYTES,pattern,NULL,&pin)==VFS_ERR_NO_MEM);
    assert(writes==before && !pin);fail_allocation=0;
    assert(exfat_replace_locked(&super,created->inode,1,LARGE_BYTES,pattern,NULL,&pin)==VFS_OK);finish_pin();
    delivered=0;assert(exfat_snapshot_stream(created,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    before=writes;
    assert(exfat_replace_locked(&super,created->inode,1,LARGE_BYTES,pattern,NULL,&pin)==VFS_ERR_STALE_GENERATION && writes==before && !pin);
    printf("exFAT 8 MiB create/replace/reopen/conflict/allocation: peak=%zu reads=%u writes=%u\n",(size_t)peak_requested,reads,writes);
    cleanup();assert(!live_requested);return 0;
}
'''
MGFS = r'''
int main(void) {
    seed();u64 pin=0;unsigned before=writes;fail_allocation=allocations+1;
    assert(mgfs_stage_and_publish(&super,2,1,LARGE_BYTES,pattern,NULL)==VFS_ERR_NO_MEM);
    assert(writes==before && !pin);fail_allocation=0;
    assert(mgfs_stage_and_publish(&super,2,1,LARGE_BYTES,pattern,NULL)==VFS_OK);
    vfs_node_t file={.super=&super,.inode=2,.type=VFS_TYPE_FILE};
    mg_file_lines_state_t state={0};delivered=0;
    assert(mgfs_snapshot_stream(&file,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    before=writes;
    assert(mgfs_stage_and_publish(&super,2,1,LARGE_BYTES,pattern,NULL)==VFS_ERR_BUSY && writes==before);
    assert(mgfs_stage_and_publish(&super,2,2,LARGE_BYTES,pattern,NULL)==VFS_OK);
    delivered=0;assert(mgfs_snapshot_stream(&file,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    vfs_node_t dir={.super=&super,.inode=1,.type=VFS_TYPE_DIRECTORY,.fs_data=(void*)1};u64 child=0;
    assert(mgfs_create_contents_private(&dir,"large.txt",LARGE_BYTES,pattern,NULL,&child)==VFS_OK && child==3);
    file.inode=child;delivered=0;
    assert(mgfs_snapshot_stream(&file,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    printf("MGFS 8 MiB replace/create/reopen/conflict/allocation: peak=%zu reads=%u writes=%u\n",(size_t)peak_bytes,reads,writes);
    assert(!live_bytes && !pin_live);
    seed();
    for(u64 block=6;block<FIXTURE_BLOCKS;block+=2)mark_old_block(block);
    seal();
    assert(mgfs_stage_and_publish(&super,2,1,LARGE_BYTES,pattern,NULL)==VFS_OK);
    file.inode=2;delivered=0;unsigned before_reads=reads;
    assert(mgfs_snapshot_stream(&file,LARGE_BYTES,verify,NULL,&state)==VFS_OK && delivered==LARGE_BYTES);
    u64 pages=(LARGE_BYTES+4095ULL)/4096;
    u64 bound=pages+2*(pages/126+1)+16;
    printf("MGFS fragmented 8 MiB snapshot: reads=%u linear_bound=%llu\n",reads-before_reads,(unsigned long long)bound);
    assert(reads-before_reads<=bound && !live_bytes);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-large-safe-') as temp:
    folder=Path(temp)
    for backend, fixture, prefix, body in (
        ('fat32','fat32_mount_identity_test.c','#define TEST_MEDIA_SECTORS 66000U\n',FAT),
        ('exfat','exfat_mount_identity_test.c','#define TEST_MEDIA_SECTORS 66000U\n',EXFAT),
        ('mgfs','mgfs_stage_cancel_test.c','#define FIXTURE_BLOCKS 7000U\n',MGFS)):
        source=folder/(backend+'.c');binary=source.with_suffix('')
        source.write_text(prefix+'#define main existing_fixture_main\n#include "'+str(ROOT/'tests/filesystems'/backend/fixture)+'"\n#undef main\n'+COMMON+body)
        subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-O2','-std=gnu17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-type-limits','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined',*dead_strip_flags(),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),str(source),str(ROOT/'libc/src/utf8.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
