/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production exFAT with controlled transport, allocation and identity. */
#define main exfat_existing_fixture_main
#include "exfat_mount_identity_test.c"
#undef main
static u64 delivered, initialized, reject_at;
static bool remove_in_sink;
static int compare_stream(void *context,u64 offset,const u8 *data,usize count)
{
    (void)context;assert(offset==delivered && count && count<=4096);
    for(usize i=0;i<count;i++)assert(data[i]==(offset+i<initialized?(u8)((offset+i)*13U+7U):0));
    if(offset>=reject_at)return VFS_ERR_ACCESS_DENIED;
    delivered+=count;if(remove_in_sink)online=false;return VFS_OK;
}
static vfs_node_t *fixture(u32 length,u32 sectors,bool contiguous,bool fragmented,u64 valid)
{
    setup_large();fs.sectors_per_cluster=sectors;fs.cluster_bytes=sectors*512;
    fs.boot.cluster_count=(12000-120)/sectors;
    fs.bitmap_length=(fs.boot.cluster_count+7)/8;
    fs.boot.volume_length=12000;
    memset(media[120],0,sectors*512);
    for(u32 c=3;c<=6;c++) {
        exfat_put32(media[24+c/128]+(c%128)*4,c==3 && sectors==1?4:c==4 && sectors==1?5:EXFAT_CLUSTER_EOC);
    }
    media[120+sectors][0]=0x1f;
    u8 set[(EXFAT_MAX_SECONDARY+1)*32],count;
    assert(exfat_build_file_set(&fs,"target.bin",0,length?8:0,valid,length,contiguous && length,set,&count));
    memcpy(media[120],set,count*32);
    for(u32 i=0,n=(length+fs.cluster_bytes-1)/fs.cluster_bytes;i<n;i++) {
        u32 c=8+i*((!contiguous && fragmented)?2:1),next=i+1==n?EXFAT_CLUSTER_EOC:c+((!contiguous && fragmented)?2:1);
        assert(c<=fs.boot.cluster_count+1);
        exfat_put32(media[24+c/128]+(c%128)*4,next);
        u32 bit=c-2;media[120+sectors+bit/4096][(bit/8)%512]|=1U<<(bit%8);
        u64 lba=exfat_cluster_lba(&fs,c);
        for(u32 j=0;j<fs.cluster_bytes;j++) {
            u64 position=(u64)i*fs.cluster_bytes+j;
            media[lba+j/512][j%512]=position<valid?(u8)(position*13U+7U):0xa5;
        }
    }
    vfs_node_t *file=exfat_finddir(&root,"target.bin");
    if(!file) {
        exfat_entry_info_t parsed;bool io=false;
        bool ok=exfat_entry_set_read_checked(&fs,2,false,0,&parsed,&io);
        fprintf(stderr,"fixture lookup length=%u spc=%u contig=%u fragment=%u parse=%u io=%u confidence=%u first=%u\n",length,sectors,contiguous,fragmented,ok,io,fs.version_confident,exfat_le32(media[120]+52));
    }
    assert(file);
    delivered=0;initialized=valid;reject_at=~(u64)0;remove_in_sink=false;return file;
}
static int run(vfs_node_t *file)
{
    mg_file_lines_state_t state={0};unsigned before=reads,requests=allocations;usize heap=live_requested;peak_requested=heap;
    assert(exfat_snapshot_stream(file,MG_FILE_CONTENT_MAX_BYTES,compare_stream,NULL,&state)==VFS_OK);
    assert(delivered==file->size && state.length==file->size && state.identifier==file->inode && state.revision && !writes && live_requested==heap && !pin_active);
    u64 clusters=(file->size+fs.cluster_bytes-1)/fs.cluster_bytes;
    u64 bound=(file->size+511)/512+5*clusters+256;
    printf("EXSNAPSHOT bytes=%llu cluster=%llu contiguous=%u reads=%u bound=%llu heap_requests=%u peak_requested=%zu\n",(unsigned long long)file->size,(unsigned long long)fs.cluster_bytes,((exfat_node_t *)file->fs_data)->no_fat_chain,reads-before,(unsigned long long)bound,allocations-requests,(size_t)(peak_requested-heap));
    if(reads-before>bound) {fprintf(stderr,"EXFAT_LINEAR_BOUND: repeated mapping reads=%u bound=%llu\n",reads-before,(unsigned long long)bound);return 3;}
    assert(allocations==requests && peak_requested==heap);
    u8 *data=malloc(file->size?file->size:1);assert(data);u64 got=0;before=reads;
    assert(exfat_node_read_checked(file,0,file->size,data,&got)==VFS_OK && got==file->size);
    for(u64 i=0;i<got;i++)assert(data[i]==(i<initialized?(u8)(i*13U+7U):0));
    assert(reads-before<=bound && live_requested==heap && !writes);free(data);return 0;
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    const u32 sizes[]={MG_FILE_CONTENT_MAX_BYTES,0,1,511,4096,8193};
    for(unsigned spc=1;spc<=64;spc*=8)
        for(unsigned mode=0;mode<3;mode++)
            for(unsigned i=0;i<6;i++) {
                u32 bytes=sizes[i];u64 valid=bytes>37?bytes-37:bytes;
                vfs_node_t *file=fixture(bytes,spc,mode==2,mode==1,valid);
                int result=run(file);cleanup();if(result)return result;
            }
    for(unsigned fault=0;fault<5;fault++) {
        vfs_node_t *file=fixture(8193,1,false,true,8193);
        mg_file_lines_state_t state={123,456,789,42},old=state;
        if(fault==0)fail_read_lba=24;
        if(fault==1)fail_read_lba=exfat_cluster_lba(&fs,8);
        if(fault==2)reject_at=4096;
        if(fault==3)remove_in_sink=true;
        int want=fault==2?VFS_ERR_ACCESS_DENIED:fault==4?VFS_ERR_BUFFER_TOO_SMALL:VFS_ERR_IO;
        assert(exfat_snapshot_stream(file,fault==4?8192:8193,compare_stream,NULL,&state)==want);
        assert(!memcmp(&state,&old,sizeof(state)) && !writes && !pin_active);cleanup();
    }
    vfs_node_t *file=fixture(8193,1,false,true,0);assert(!run(file));cleanup();
    for(unsigned contiguous=0;contiguous<2;contiguous++) {
        const u32 length=MG_FILE_CONTENT_MAX_BYTES+8193;
        file=fixture(length,1,contiguous,true,length);
        mg_file_lines_state_t state={123,456,789,42},old=state;
        assert(exfat_snapshot_stream(file,MG_FILE_CONTENT_MAX_BYTES,compare_stream,NULL,&state)==VFS_ERR_BUFFER_TOO_SMALL);
        assert(!memcmp(&state,&old,sizeof(state)) && fs.version_confident);
        u8 *bytes=malloc(length);assert(bytes);u64 count=0;
        assert(exfat_node_read_checked(file,0,length,bytes,&count)==VFS_OK && count==length);
        for(u32 i=0;i<length;i++)assert(bytes[i]==(u8)(i*13U+7U));
        free(bytes);cleanup();
    }
    puts("exFAT forward snapshot/read: fragmented FAT, NoFatChain, initialized/poisoned slack, sink/I/O/removal failures passed");
}
