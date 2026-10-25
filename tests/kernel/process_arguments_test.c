/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process_arguments.h>
#include <mg/filesystem.h>
#define VMM_PAGE_SIZE 4096U
#define PTE_USER 4U
#define PTE_READWRITE 2U
#define PTE_NX (1ULL << 63)
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM (-1)
#define VFS_ERR_NO_MEM (-2)
#define VFS_ERR_BUFFER_TOO_SMALL (-15)
#define VFS_ERR_NAME_TOO_LONG (-17)
typedef u64 phys_addr_t;
typedef process_arguments_t process_args_t;
typedef struct kernel_thread {void *entry_argument;} kernel_thread_t;
typedef struct {int unused;} process_credentials_t;
typedef u64 mg_session_id_t;
typedef u32 process_handle_t;
typedef struct {int type;} kernel_object_t;
typedef struct {
    void *address_space;
    int state;
    bool system_service, credentials_initialized, session_shell;
    u32 service_id;
    mg_session_id_t session_id;
    process_credentials_t credentials;
    uintptr_t entry_point;
    char executable_path[512];
    kernel_object_t object;
    uintptr_t user_stack_top, user_stack_sp, user_argc, user_argv;
} process_t;
static usize allocations, live_allocations, live_bytes, peak_bytes, fail_allocation = (usize)-1;
void *kmalloc(usize size)
{
    if (allocations++ == fail_allocation) return NULL;
    usize *p = malloc(size + sizeof(usize));
    if (!p) return NULL;
    *p = size;
    live_allocations++;live_bytes+=size;if(live_bytes>peak_bytes)peak_bytes=live_bytes;
    return p + 1;
}
void kfree(void *p) {
    if (p) {usize *header=(usize *)p-1;assert(live_allocations);live_allocations--;live_bytes-=*header;free(header);}
}
static void process_string_release(char **text)
{if (text && *text) {kfree(*text);*text=NULL;}}
#define PROCESS_STRING_AUTO __attribute__((cleanup(process_string_release)))
static void panic(const char *reason) { fprintf(stderr,"%s\n",reason); abort(); }
#define PAGES (MG_PROCESS_ARGUMENT_BYTES_MAX / 4096U)
static void *frames[PAGES + 1U];
static phys_addr_t mappings[PAGES];
static usize frame_calls, map_calls, fail_frame = (usize)-1, fail_map = (usize)-1;
static usize live_frames;
static phys_addr_t pmm_alloc_frame(void)
{
    if (frame_calls++ == fail_frame) return 0;
    for (usize i=1;i<=PAGES;i++) if (!frames[i]) {
        frames[i]=malloc(4096); assert(frames[i]); live_frames++; return i*4096;
    }
    return 0;
}
static void pmm_free_frame(phys_addr_t frame)
{usize i=frame/4096; assert(i && frames[i]); free(frames[i]);frames[i]=NULL;live_frames--;}
static void *phys_to_virt(phys_addr_t frame) {return frames[frame/4096];}
static bool vmm_direct_map_valid(phys_addr_t frame) {return phys_to_virt(frame)!=NULL;}
static bool vmm_map_user_page(void *root, void *address, phys_addr_t frame, u64 flags)
{
    (void)root; assert(flags==(PTE_USER|PTE_READWRITE|PTE_NX));
    if (map_calls++ == fail_map) return false;
    usize i=((uintptr_t)address-PROCESS_USER_ARGUMENT_BASE)/4096;
    assert(i<PAGES && !mappings[i]); mappings[i]=frame;return true;
}
static bool vmm_unmap_user_page(void *root, void *address, phys_addr_t *frame)
{
    (void)root; usize i=((uintptr_t)address-PROCESS_USER_ARGUMENT_BASE)/4096;
    assert(i<PAGES && mappings[i]);*frame=mappings[i];mappings[i]=0;return true;
}
#define PROCESS_COMMAND_LINE_CAPACITY 256U
/* LEGACY */
/* INSTALLER */
static u8 child_byte(uintptr_t address)
{
    usize offset=address-PROCESS_USER_ARGUMENT_BASE;
    assert(offset<MG_PROCESS_ARGUMENT_BYTES_MAX && mappings[offset/4096]);
    return ((u8 *)phys_to_virt(mappings[offset/4096]))[offset%4096];
}
static uintptr_t child_pointer(usize index)
{
    uintptr_t value=0;for(usize j=0;j<8;j++)value|=(uintptr_t)child_byte(PROCESS_USER_ARGUMENT_BASE+index*8+j)<<(j*8);
    return value;
}
static void verify_child(process_t *p,const char *const *argv,usize argc)
{
    assert(p->user_argc==argc && p->user_argv==PROCESS_USER_ARGUMENT_BASE);
    assert(p->user_stack_sp==p->user_stack_top-16 && !(p->user_stack_sp&15));
    assert(child_pointer(argc)==0);
    for(usize i=0;i<argc;i++) {
        uintptr_t a=child_pointer(i);usize n=strlen(argv[i])+1;
        for(usize j=0;j<n;j++)assert(child_byte(a+j)==(u8)argv[i][j]);
    }
}
static void destroy_child(void)
{
    for(usize i=0;i<PAGES;i++)if(mappings[i]){pmm_free_frame(mappings[i]);mappings[i]=0;}
    assert(!live_frames);
}
static uintptr_t denied_page;
static bool readable(const void *p,usize size)
{
    uintptr_t a=(uintptr_t)p;
    if (denied_page && a < denied_page+4096 && a+size > denied_page) return false;
    return a>=0x10000U && a<0x0000800000000000ULL && size<0x10000000U &&
           size<=0x0000800000000000ULL-a;
}
static void capture_and_map(void)
{
    usize argc=4096; const char **argv=calloc(argc,sizeof(*argv));assert(argv);
    char *user_page=aligned_alloc(4096,4096);assert(user_page);memset(user_page,0,4096);
    strcpy(user_page,"/bin/fixture");strcpy(user_page+64,"spaces and\nnewlines");strcpy(user_page+128,"中文 café");
    for(usize i=0;i<argc;i++)argv[i]= i%3==0 ? user_page+192 : i%3==1 ? user_page+64 : user_page+128;
    argv[0]=user_page;
    process_arguments_t args;
    allocations=0;
    assert(process_arguments_collect(&args,(uintptr_t)argv,argc,readable)==MG_OK);
    usize used_allocations=allocations;
    printf("4096-argument snapshot: strings=%llu kernel staging=%llu bytes\n",args.bytes,live_bytes);
    assert(live_bytes <= (args.argc+1)*8+args.bytes+65536U);
    process_t child={.user_stack_top=PROCESS_USER_STACK_TOP};
    user_page[0]='!';
    assert(setup_user_stack_args(&child,&args)==VFS_OK);
    assert(child_byte(child_pointer(0))=='/');
    user_page[0]='/';
    verify_child(&child,argv,argc);destroy_child();process_arguments_destroy(&args);
    assert(!live_allocations);
    for(usize fail=0;fail<used_allocations;fail++) {
        allocations=0;fail_allocation=fail;
        assert(process_arguments_collect(&args,(uintptr_t)argv,argc,readable)==MG_ERR_NO_MEMORY);
        assert(!live_allocations && !args.offsets && !args.first);
    }
    fail_allocation=(usize)-1;
    assert(process_arguments_collect(&args,(uintptr_t)argv,argc,readable)==MG_OK);
    usize pages=((argc+1)*8+args.bytes+4095)/4096;
    for(usize i=0;i<pages;i++) {
        child.user_argc=child.user_argv=0;frame_calls=map_calls=0;fail_frame=i;
        assert(setup_user_stack_args(&child,&args)==VFS_ERR_NO_MEM);
        assert(!live_frames && !child.user_argc && !child.user_argv);
        fail_frame=(usize)-1;frame_calls=map_calls=0;fail_map=i;
        assert(setup_user_stack_args(&child,&args)==VFS_ERR_NO_MEM);
        assert(!live_frames && !child.user_argc && !child.user_argv);
        fail_map=(usize)-1;
    }
    process_arguments_destroy(&args);free(argv);free(user_page);assert(!live_allocations);
    printf("4096 arguments: %llu pages; capture and every allocation/frame/map failure PASS\n",pages);
}
static void boundaries(void)
{
    process_arguments_t args;
    char *user_page=aligned_alloc(4096,4096);assert(user_page);memset(user_page,0,4096);strcpy(user_page,"/bin/fixture");
    const char *argv[]={user_page,user_page+128};
    assert(process_arguments_collect(&args,0,2,readable)==MG_ERR_BAD_ARGUMENT);
    assert(process_arguments_collect(&args,(uintptr_t)argv,0,readable)==MG_ERR_BAD_ARGUMENT);
    assert(process_arguments_collect(&args,(uintptr_t)argv,~(usize)0,readable)==MG_ERR_BAD_ARGUMENT);
    assert(process_arguments_collect(&args,~(uintptr_t)0-8U,2,readable)==MG_ERR_BAD_ARGUMENT);
    argv[1]=NULL;assert(process_arguments_collect(&args,(uintptr_t)argv,2,readable)==MG_ERR_BAD_ARGUMENT);
    argv[1]=(void *)0x1000U;assert(process_arguments_collect(&args,(uintptr_t)argv,2,readable)==MG_ERR_BAD_ARGUMENT);
    assert(!live_allocations);
    usize length=MG_PROCESS_ARGUMENT_BYTES_MAX-24U-(strlen(user_page)+1U);
    char *large=aligned_alloc(4096,MG_PROCESS_ARGUMENT_BYTES_MAX+4096U);assert(large);memset(large,'x',length);large[length-1]=0;
    const char *one[]={user_page,large};
    denied_page=((uintptr_t)large)+4096U;
    assert(process_arguments_collect(&args,(uintptr_t)one,2,readable)==MG_ERR_BAD_ARGUMENT);
    assert(!live_allocations);denied_page=0;
    assert(process_arguments_collect(&args,(uintptr_t)one,2,readable)==MG_OK);
    assert(args.bytes==MG_PROCESS_ARGUMENT_BYTES_MAX-24U);
    assert(live_bytes <= MG_PROCESS_ARGUMENT_BYTES_MAX+65536U);
    printf("8 MiB admission: kernel staging peak=%llu bytes\n",peak_bytes);
    process_t child={.user_stack_top=PROCESS_USER_STACK_TOP};
    assert(setup_user_stack_args(&child,&args)==VFS_OK);
    assert(child_byte(child_pointer(1)+length-1U)==0);destroy_child();process_arguments_destroy(&args);
    large[length-1]='x';large[length]=0;
    assert(process_arguments_collect(&args,(uintptr_t)one,2,readable)==MG_ERR_BUFFER_TOO_SMALL);
    assert(!live_allocations);
    free(large);
    usize count=MG_PROCESS_MAX_ARGUMENTS-1U;
    const char **many=malloc((count+1U)*sizeof(*many));assert(many);
    many[0]=user_page;for(usize i=1;i<=count;i++)many[i]=user_page+128;
    assert(process_arguments_collect(&args,(uintptr_t)many,count,readable)==MG_OK);
    assert(args.argc==count && args.bytes==count+strlen(user_page));
    process_arguments_destroy(&args);
    assert(process_arguments_collect(&args,(uintptr_t)many,count+1U,readable)==MG_ERR_BUFFER_TOO_SMALL);
    assert(!live_allocations);free(many);free(user_page);
    puts("derived argument-count ceiling and empty-argument byte accounting PASS");
    puts("8 MiB packed boundary, multi-megabyte argument, empty strings, invalid pointers and overflow PASS");
}

#define PROCESS_STATE_ACTIVE 0
#define PROCESS_NO_SESSION 0
#define PROCESS_INITIAL_STDIN_HANDLE 2
#define PROCESS_INITIAL_CONSOLE_HANDLE 1
#define OBJECT_TYPE_CONSOLE 1
#define OBJECT_TYPE_INVALID 0
#define OBJECT_TYPE_FILE 2
#define OBJECT_RIGHT_READ 1
#define OBJECT_RIGHT_WRITE 2
#define VFS_ERR_BAD_FORMAT (-5)
#define VFS_LOCATION_AUTO
static unsigned stage, failed_stage;
static bool thread_live, child_live, parent_handle_live;
static unsigned child_handles;
static process_t spawned;
static kernel_thread_t spawned_thread;
static kernel_object_t console = {.type=OBJECT_TYPE_CONSOLE};
typedef struct {int unused;} vfs_location_t;
static vfs_location_t location;
static bool stage_ok(void) {return ++stage != failed_stage;}
static bool identity_credentials_valid(const process_credentials_t *c){(void)c;return true;}
static bool identity_credentials_is_system(const process_credentials_t *c){(void)c;return true;}
static int process_lookup_path(process_t *p,const char *n,vfs_location_t **out)
{(void)p;(void)n;if(!stage_ok())return VFS_ERR_NO_MEM;*out=&location;return VFS_OK;}
static int vfs_location_path(vfs_location_t *p,char *out,usize cap,void *required)
{(void)p;(void)cap;(void)required;if(!stage_ok())return VFS_ERR_NO_MEM;strcpy(out,"/bin/fixture");return VFS_OK;}
static void *vfs_location_node(vfs_location_t *p){return p;}
static kernel_object_t *process_handle_lookup(process_t *p,process_handle_t h,int t,u32 r)
{(void)p;(void)h;(void)t;(void)r;return &console;}
static kernel_object_t *process_handle_lookup_any(process_t *p,process_handle_t h,int t,u32 r)
{return process_handle_lookup(p,h,t,r);}
static kernel_thread_t *thread_create_suspended(const char *n,void (*entry)(void *),void *arg)
{(void)n;(void)entry;(void)arg;if(!stage_ok())return NULL;thread_live=true;return &spawned_thread;}
static void process_user_thread_entry(void *arg){(void)arg;}
static bool thread_destroy(kernel_thread_t *t){(void)t;assert(thread_live);thread_live=false;return true;}
static process_t *process_create(const char *n,process_t *p,kernel_thread_t *t)
{(void)n;(void)p;(void)t;if(!stage_ok())return NULL;memset(&spawned,0,sizeof(spawned));child_live=true;return &spawned;}
static int process_chdir_result(process_t *p,const char *n){(void)p;(void)n;return VFS_OK;}
static int elf_load_process_node_result(process_t *p,void *n,uintptr_t *entry,uintptr_t *stack)
{(void)n;if(!stage_ok())return VFS_ERR_NO_MEM;*entry=1;*stack=PROCESS_USER_STACK_TOP;p->user_stack_top=*stack;return VFS_OK;}
static void process_abort(process_t *p)
{assert(p==&spawned && child_live && thread_live && !parent_handle_live);destroy_child();child_live=thread_live=false;child_handles=0;}
static bool process_handle_install(process_t *p,kernel_object_t *o,u32 r,process_handle_t *out)
{
 (void)o;(void)r;if(!stage_ok())return false;
 if(p==&spawned){*out=++child_handles;}else{assert(!parent_handle_live);parent_handle_live=true;*out=77;}
 return true;
}
static bool process_handle_close(process_t *p,process_handle_t h)
{if(p==&spawned){assert(child_handles && h);child_handles--;}else{assert(h==77 && parent_handle_live);parent_handle_live=false;}return true;}
static bool scheduler_enqueue(kernel_thread_t *t){(void)t;return stage_ok();}
/* SPAWN */
static void legacy_arguments(void)
{
    char line[256] = "/bin/fixture";
    char executable[256]; process_args_t args;
    const char *argv[41] = {"/bin/fixture"};
    for (usize i=1;i<41;i++) {strcat(line," x");argv[i]="x";}
    assert(parse_spawn_cmdline(line,executable,sizeof(executable),&args));
    assert(args.argc==41 && !strcmp(executable,argv[0]));
    process_t child={.user_stack_top=PROCESS_USER_STACK_TOP};
    assert(setup_user_stack_args(&child,&args)==VFS_OK);
    verify_child(&child,argv,41);destroy_child();process_arguments_destroy(&args);
    assert(!parse_spawn_cmdline("/bin/fixture 'unterminated",executable,sizeof(executable),&args));
    assert(!live_allocations);
    puts("legacy command-line spawn: more than sixteen tokens and bounded malformed input PASS");
}
static void lifecycle(void)
{
    process_t parent={.state=PROCESS_STATE_ACTIVE};process_arguments_t args;
    const char *argv[]={"/bin/fixture","","中文 space"};
    assert(process_arguments_collect(&args,(uintptr_t)argv,3,NULL)==MG_OK);
    for(unsigned failure=1;failure<=9;failure++) {
        stage=0;failed_stage=failure;process_handle_t handle=12345;
        assert(process_spawn_args_with_context_internal(&parent,&args,NULL,0,false,NULL,false,0,true,0,&handle)!=VFS_OK);
        assert(handle==12345 && !child_live && !thread_live && !parent_handle_live && !child_handles && !live_frames);
    }
    stage=failed_stage=0;process_handle_t handle=0;
    assert(process_spawn_args_with_context_internal(&parent,&args,NULL,0,false,NULL,false,0,true,0,&handle)==VFS_OK);
    assert(handle==77 && parent_handle_live && child_live && thread_live);
    verify_child(&spawned,argv,3);
    process_handle_close(&parent,handle);process_abort(&spawned);
    process_arguments_destroy(&args);assert(!live_allocations);
    const char *empty_executable[]={""};
    assert(process_arguments_collect(&args,(uintptr_t)empty_executable,1,NULL)==MG_OK);
    handle=12345;stage=0;
    assert(process_spawn_args_with_context_internal(&parent,&args,NULL,0,false,NULL,false,0,true,0,&handle)==VFS_ERR_INVALID_PARAM);
    assert(!stage && handle==12345 && !child_live && !thread_live && !parent_handle_live);
    process_arguments_destroy(&args);assert(!live_allocations);
    puts("spawn lookup/thread/process/ELF/child handles/parent handle/enqueue failure cleanup PASS");
}
int main(void){capture_and_map();boundaries();legacy_arguments();lifecycle();return 0;}
