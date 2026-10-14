/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Appended to production block/GPT/mutex code by the host runner. */
static u8 media[8192][512];
static unsigned reads, writes, fail_write;
static bool partial, failed_flush;
static block_device_t parent;
static bool backend_read(block_device_t *d,u64 l,u32 n,void*out)
{ (void)d;assert(l<8192 && n<=8192-l);reads++;memcpy(out,media[l],n*512U);return true; }
static bool backend_write(block_device_t *d,u64 l,u32 n,const void*in)
{
    (void)d;assert(l<8192 && n<=8192-l);writes++;
    bool failed = writes==fail_write;
    memcpy(media[l],in,failed ? (partial ? n*256U : 0U) : n*512U);
    return !failed;
}
static bool backend_flush(block_device_t*d){(void)d;return !failed_flush;}
static void setup_parent_size(u64 sectors)
{
    assert(!block_device_count()); memset(media,0,sizeof(media));
    reads=writes=fail_write=0;partial=failed_flush=false;
    parent=(block_device_t){.sector_size=512,.sector_count=sectors,
        .read=backend_read,.write=backend_write,.flush=backend_flush,.driver_data=media};
    assert(block_register(&parent));
}
static void setup_parent(void) { setup_parent_size(1024); }
static block_device_t *root_device(void)
{ return block_get_device_by_public_id(BLOCK_DEVICE_ID_BASE | (parent.id+1)); }
#ifndef COHERENCY_BASELINE
static block_device_t *add_child(u64 first,u64 last)
{
    assert(partition_count<GPT_REGISTERED_MAX);u32 slot=partition_count++;
    partitions[slot]=(gpt_partition_info_t){.parent=&parent,.parent_id=parent.id,.first_lba=first,.last_lba=last};
    block_device_t child={.type=BLOCK_DEVICE_PARTITION,.sector_size=512,
        .sector_count=last-first+1,.read=partition_read,.write=partition_write,
        .flush=partition_flush,.driver_data=&partitions[slot]};
    assert(block_register(&child));partition_registered[slot]=true;partition_ids[slot]=child.id;
    return block_get_device_by_public_id(BLOCK_DEVICE_ID_BASE | (child.id+1));
}
#endif
static void teardown(void)
{
    assert(gpt_remove_partitions_for_parent(&parent));
    assert(block_unregister_by_id(parent.id));partition_count=0;
}
static void seed_compact_gpt(void)
{
    gpt_table_t old={.info={.type=GPT_TABLE_VALID,.entry_count=4,.entry_size=128,
        .first_usable_lba=3,.last_usable_lba=1021},.primary_entries_lba=2,
        .backup_entries_lba=1022,.array_sectors=1,.disk_guid={1}};
    memset(primary_entries,0,sizeof(primary_entries));
    primary_entries[0]=1;primary_entries[16]=2;
    store_le64(primary_entries+32,3);store_le64(primary_entries+40,100);
    u32 crc=crc32(primary_entries,512,~(u32)0);
    memcpy(media[2],primary_entries,512);memcpy(media[1022],primary_entries,512);
    gpt_make_header(media[1],&old,false,crc);gpt_make_header(media[1023],&old,true,crc);
    assert(storage_mutation_begin());assert(gpt_write_protective_mbr(&parent));storage_mutation_end();
    assert(gpt_scan_device(&parent));
}
static void failed_initialization(void)
{
    setup_parent();seed_compact_gpt();block_device_t *child=block_get_device(1);
    assert(child && child->type==BLOCK_DEVICE_PARTITION);
    u8 out[512];memset(media[3],'o',512);assert(block_read(child,0,1,out)&&out[0]=='o');
    unsigned before=writes;fail_write=before+4U;partial=true;
    int result=gpt_initialize_device(root_device());
#ifdef COHERENCY_BASELINE
    assert(result==VFS_ERR_IO && writes==before+4U && block_device_is_live(child) && media[3][0]==0);
    unsigned old_reads=reads;assert(block_read(child,0,1,out)&&out[0]=='o'&&reads==old_reads);
    puts("baseline failed GPT initialization: overwritten data, live child, stale cache reproduced");
#else
    assert(result==VFS_ERR_BUSY && writes==before && block_device_is_live(child) && media[3][0]=='o');
    unsigned old_reads=reads;assert(block_read(child,0,1,out)&&out[0]=='o'&&reads==old_reads);
    assert(!storage_mutation_busy());
    puts("failed-GPT regression: conflict rejected before MBR write, registration and cache unchanged");
#endif
    teardown();
}
#ifndef COHERENCY_BASELINE
static void aliases(void)
{
    setup_parent();block_device_t *a=add_child(100,119),*b=add_child(110,129),*c=add_child(200,219);
    block_device_t duplicate=parent;assert(!block_register(&duplicate));
    duplicate.registered=false;assert(!block_register(&duplicate));
    u8 out[512],patch[512];memset(patch,'n',512);memset(media[110],'o',512);memset(media[200],'d',512);
    assert(block_read(a,10,1,out)&&out[0]=='o');unsigned old_reads=reads;
    assert(block_read_for_id(b,b->id,0,1,out)&&out[0]=='o'&&reads==old_reads);
    assert(block_read(c,0,1,out)&&out[0]=='d');
    /* Parent write fails after copying half a sector: both aliases must miss. */
    fail_write=writes+1;partial=true;
    assert(!block_write_for_id(root_device(),parent.id,110,1,patch));
    old_reads=reads;assert(block_read(c,0,1,out)&&out[0]=='d'&&reads==old_reads);
    assert(block_read(a,10,1,out)&&out[0]=='n'&&out[256]=='o'&&reads==old_reads+1);
    old_reads=reads;assert(block_read(b,0,1,out)&&out[0]=='n'&&reads==old_reads);
    fail_write=0;memset(patch,'z',512);
    assert(block_write(b,0,1,patch));old_reads=reads;
    assert(block_read(a,10,1,out)&&out[0]=='z'&&reads==old_reads+1);
    old_reads=reads;assert(block_read_for_id(root_device(),parent.id,110,1,out)&&out[0]=='z'&&reads==old_reads);
    assert(storage_mutation_begin());
    assert(storage_management_range_preflight(root_device(),parent.id,0,34)==VFS_OK);
    assert(storage_management_range_preflight(root_device(),parent.id,110,1)==VFS_ERR_BUSY);
    assert(storage_management_range_preflight(a,a->id,0,20)==VFS_ERR_BUSY);
    assert(storage_management_range_preflight(c,c->id,0,20)==VFS_OK);
    assert(storage_management_range_preflight(c,c->id+999,0,1)==VFS_ERR_DEVICE_GONE);
    assert(storage_management_range_preflight(c,c->id,19,2)==VFS_ERR_INVALID_PARAM);
    storage_mutation_end();
    assert(!block_read(a,20,1,out));assert(!block_write(a,UINT64_MAX,1,patch));
    unsigned before_writes=writes;
    root_device()->read_only=true;
    assert(!block_write(b,0,1,patch)&&writes==before_writes);
    root_device()->read_only=false;
    u64 saved_first=partitions[0].first_lba;
    partitions[0].first_lba=120U; /* Invalid length: reject even a cached hit. */
    old_reads=reads;assert(!block_read(a,10,1,out)&&reads==old_reads);
    partitions[0].first_lba=saved_first;
    u64 old_id=a->id;assert(block_unregister_by_id(old_id));
    partition_registered[0]=false; /* Fixture performed this child teardown. */
    block_device_t *replacement=add_child(300,319);
    assert(replacement==a && replacement->id!=old_id);
    old_reads=reads;assert(!block_read_for_id(replacement,old_id,0,1,out)&&reads==old_reads);
    assert(block_read(replacement,0,1,out)&&out[0]==0);
    teardown();
}
static void gpt_plans(void)
{
    setup_parent();assert(gpt_initialize_device(root_device())==VFS_OK);
    gpt_table_info_t info;assert(gpt_get_table_info(root_device(),&info)==VFS_OK);
    unsigned before=writes;block_device_t *child=add_child(10,20); /* Existing conflicting registration. */
    assert(gpt_initialize_device(root_device())==VFS_ERR_BUSY&&writes==before);
    assert(block_unregister_by_id(child->id));partition_registered[0]=false;
    /* No conflict: failed flush remains an error, never success. */
    failed_flush=true;before=writes;
    assert(gpt_initialize_device(root_device())==VFS_ERR_IO&&writes>before);
    failed_flush=false;assert(gpt_initialize_device(root_device())==VFS_OK);
    teardown();
    setup_parent_size(8192);assert(gpt_initialize_device(root_device())==VFS_OK);
    gpt_partition_plan_t plan;
    assert(gpt_plan_partition(root_device(),1024U*1024U,false,&plan)==VFS_OK);
    assert(gpt_create_partition_for_id(root_device(),parent.id+999,plan.partition_number,
        plan.first_lba,plan.last_lba)==VFS_ERR_DEVICE_GONE);
    assert(gpt_create_partition(root_device(),plan.partition_number,plan.first_lba,plan.last_lba)==VFS_OK);
    block_device_t *live_child=block_get_device(1);u64 child_id=live_child->id;
    assert(gpt_plan_partition(root_device(),1024U*1024U,false,&plan)==VFS_OK);
    block_device_t *overlap=add_child(2,3);before=writes;
    assert(gpt_create_partition(root_device(),plan.partition_number,plan.first_lba,plan.last_lba)==VFS_ERR_BUSY);
    assert(gpt_delete_partition(root_device(),live_child,1)==VFS_ERR_BUSY);
    assert(writes==before && block_device_id_is_live(child_id));
    assert(block_unregister_by_id(overlap->id));partition_registered[1]=false;
    assert(gpt_delete_partition_for_id(root_device(),parent.id,live_child,child_id+999,1)==VFS_ERR_DEVICE_GONE);
    assert(writes==before);
    assert(gpt_delete_partition(root_device(),live_child,1)==VFS_OK);
    teardown();
}
static void removal_and_discovery(void)
{
    setup_parent();block_device_t *child=add_child(200,219);
    u64 child_id=child->id, old_parent=parent.id;
    u8 out[512],patch[512];memset(patch,'x',sizeof(patch));
    assert(block_read(child,0,1,out));
    assert(block_device_mark_gone(parent.id));unsigned before=reads;
    assert(!block_read(child,0,1,out)&&reads==before);
    assert(!block_write(child,0,1,patch));
    assert(block_unregister_by_id(old_parent));
    /* Deliberate parent-descriptor reuse without coordinated retirement:
     * old children must fail, never adopt the replacement registration. */
    assert(block_register(&parent)&&parent.id!=old_parent);
    unsigned before_writes=writes;
    assert(!block_read(child,0,1,out)&&reads==before);
    assert(!block_write(child,0,1,patch)&&writes==before_writes);
    assert(gpt_initialize_device_for_id(root_device(),old_parent)==VFS_ERR_DEVICE_GONE);
    assert(writes==before_writes);
    assert(block_unregister_by_id(child_id));partition_registered[0]=false;
    partitions[0].parent=NULL;teardown();
    /* USB's supported boot discovery order: children before parent. */
    memset(media,0,sizeof(media));reads=writes=fail_write=0;
    parent=(block_device_t){.sector_size=512,.sector_count=1024,.read=backend_read,.write=backend_write};
    child=add_child(200,219);partitions[0].parent_id=~(u64)0;
    assert(!block_read(child,0,1,out));assert(block_register(&parent));
    assert(partitions[0].parent_id==parent.id);
    assert(block_read(child,0,1,out));teardown();
}
static void *worker(void *arg)
{
    kernel_thread_t thread={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};self=&thread;
    block_device_t *child=arg;u8 patch[512],out[512];
    for(unsigned i=0;i<1000;i++) {
        assert(block_io_acquire());memset(patch,(u8)i,512);
        assert(block_write(child,0,1,patch));assert(block_read(root_device(),200,1,out));
        assert(!memcmp(patch,out,512));block_io_release();
    }
    return NULL;
}
static void concurrent(void)
{
    setup_parent();block_device_t *child=add_child(200,219);pthread_t threads[4];
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,child));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    teardown();puts("physical cache: 4,000 concurrent parent/child write/read handoffs passed");
}
#endif
int main(void)
{
    kernel_thread_t main_thread={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};self=&main_thread;
    failed_initialization();
#ifndef COHERENCY_BASELINE
    aliases();gpt_plans();removal_and_discovery();concurrent();
    printf("physical cache requested storage: %zu bytes, %zu per entry; no dynamic allocations\n",sizeof(block_cache),sizeof(block_cache_entry_t));
    printf("GPT retained partition mapping: %zu bytes per registration\n",sizeof(gpt_partition_info_t));
#endif
}
