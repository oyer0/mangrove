/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production FAT32; controlled block transport/heap/registry. */
#define main fat32_existing_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

static u64 delivered, reject_at;
static bool remove_in_sink;
static int compare_snapshot(void *context,u64 offset,const u8 *data,usize length)
{
    (void)context;
    assert(offset==delivered && length && length<=4096);
    for(usize i=0;i<length;i++)assert(data[i]==(u8)((offset+i)*13U+7U));
    if(offset>=reject_at)return VFS_ERR_ACCESS_DENIED;
    delivered+=length;
    if(remove_in_sink)online=false;
    return VFS_OK;
}
static vfs_node_t *snapshot_fixture(u32 bytes,u32 sectors,bool fragmented)
{
    setup();fs.data_start_lba=80;fs.fat_size_sectors=65;
    fs.sectors_per_cluster=sectors;fs.cluster_size_bytes=sectors*512;
    fs.total_clusters=(TEST_MEDIA_SECTORS-80)/sectors+2;
    fat32_on_disk_entry_t *entry=(void *)media[80];
    memcpy(entry->name,"TARGET  BIN",11);entry->attr=0x20;
    entry->first_cluster_low=bytes?3:0;entry->file_size=bytes;
    u32 count=(bytes+fs.cluster_size_bytes-1)/fs.cluster_size_bytes;
    for(u32 i=0;i<count;i++) {
        u32 cluster=3+i*(fragmented?2:1),next=i+1==count?FAT32_EOC_MIN:cluster+(fragmented?2:1);
        assert(cluster<fs.total_clusters);
        ((u32 *)media[1+cluster/128])[cluster%128]=next;
        u64 lba=fat32_cluster_to_lba(&fs,cluster);
        for(u32 j=0;j<fs.cluster_size_bytes;j++)
            media[lba+j/512][j%512]=(u8)(((u64)i*fs.cluster_size_bytes+j)*13U+7U);
    }
    vfs_node_t *file=fat32_finddir(&root,"TARGET.BIN");assert(file);
    delivered=0;reject_at=~(u64)0;remove_in_sink=false;return file;
}
static int run_snapshot(vfs_node_t *file,bool bound)
{
    u32 sectors=fs.sectors_per_cluster;
    u64 clusters=(file->size+fs.cluster_size_bytes-1)/fs.cluster_size_bytes;
    unsigned before=reads,issued=writes,requests=allocations;usize heap=live_requested;peak_requested=heap;
    mg_file_lines_state_t state={0};
    assert(fat32_snapshot_stream(file,MG_FILE_CONTENT_MAX_BYTES,compare_snapshot,NULL,&state)==VFS_OK);
    assert(state.length==file->size && state.identifier==file->inode && state.revision && delivered==file->size);
    assert(writes==issued && live_requested==heap && !active_pin);
    u64 limit=(file->size+511)/512+3*clusters+sectors;
    printf("SNAPSHOT bytes=%llu cluster=%u reads=%u linear_bound=%llu heap_requests=%u peak_requested=%zu\n",
        (unsigned long long)file->size,fs.cluster_size_bytes,reads-before,(unsigned long long)limit,allocations-requests,(size_t)(peak_requested-heap));
    if(bound && reads-before>limit) {
        fprintf(stderr,"SNAPSHOT_LINEAR_BOUND: repeated FAT traversal reads=%u limit=%llu\n",reads-before,(unsigned long long)limit);
        return 3;
    }
    assert(allocations-requests<=2*clusters+2 && peak_requested-heap<=fs.cluster_size_bytes+512U);
    return 0;
}
static void failures(void)
{
    const u32 bytes=8193;
    for(u32 sectors=1;sectors<=128;sectors*=2) {
        for(u32 fault=0;fault<5;fault++) {
            vfs_node_t *file=snapshot_fixture(bytes,sectors,true);
            mg_file_lines_state_t state={123,456,789,42},original=state;
            u64 first=fat32_cluster_to_lba(&fs,3);
            if(fault==0)fail_read_lba=1;
            if(fault==1)fail_read_lba=first;
            if(fault==2)reject_at=4096;
            if(fault==3)remove_in_sink=true;
            int expected=fault==2?VFS_ERR_ACCESS_DENIED:fault==4?VFS_ERR_BUFFER_TOO_SMALL:VFS_ERR_IO;
            assert(fat32_snapshot_stream(file,fault==4?bytes-1:bytes,compare_snapshot,NULL,&state)==expected);
            assert(!memcmp(&state,&original,sizeof(state)) && !writes && !active_pin);
            cleanup();assert(!live_requested);
        }
    }
}
static int independent_image(const char *path)
{
    setup();image_file=fopen(path,"rb");assert(image_file);
    assert(!fseeko(image_file,0,SEEK_END));image_sectors=(u64)ftello(image_file)/512;
    device.id=906;device.sector_size=512;device.sector_count=image_sectors;image_writable=false;
    vfs_super_t *mounted;assert(fat32_mount(&fat32_fs_type,&device,&mounted,true)==VFS_OK);
    vfs_node_t *file=fat32_finddir(mounted->root_node,"TARGET.BIN");assert(file);
    fat32_fs_t *state=mounted->private_data;
    delivered=0;reject_at=~(u64)0;u64 limit=(file->size+511)/512+3*(file->size/state->cluster_size_bytes+1)+state->sectors_per_cluster;
    unsigned before=reads;mg_file_lines_state_t facts={0};
    assert(fat32_snapshot_stream(file,MG_FILE_CONTENT_MAX_BYTES,compare_snapshot,NULL,&facts)==VFS_OK);
    assert(delivered==MG_FILE_CONTENT_MAX_BYTES && facts.length==delivered && reads-before<=limit && !writes);
    vfs_node_t *nodes[2];unsigned count=0;
    for(fat32_node_state_t *n=state->nodes;n;n=n->next) {assert(count<2);nodes[count++]=n->node;}
    assert(fat32_unmount(mounted)==VFS_OK);
    for(unsigned i=0;i<count;i++)kfree(nodes[i]);
    kfree(mounted);
    assert(!live_requested && !fclose(image_file));image_file=NULL;
    puts("Independent formatted FAT32: exact maximum snapshot, unchanged image, linear reads");return 0;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc==3 && !strcmp(argv[1],"--image"))return independent_image(argv[2]);
    assert(argc==1);
    for(u32 sectors=1;sectors<=128;sectors*=2)
        for(unsigned fragmented=0;fragmented<2;fragmented++)
            for(unsigned i=0;i<5;i++) {
                static const u32 sizes[]={MG_FILE_CONTENT_MAX_BYTES,0,1,4095,8193};
                vfs_node_t *file=snapshot_fixture(sizes[i],sectors,fragmented);
                int result=run_snapshot(file,true);cleanup();assert(!live_requested);
                if(result)return result;
            }
    failures();puts("FAT32 streaming snapshot: boundaries, fragmentation, cluster bytes=512..65536, sink/read/removal failures passed");
}
