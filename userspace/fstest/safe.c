/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public native APIs only. Expected bytes never depend on readback. */
#include <mangrove.h>
#include <mg/inspection.h>
#include <mg/time.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static unsigned char input[MG_FILE_CONTENT_MAX_BYTES];
static unsigned char output[MG_FILE_CONTENT_MAX_BYTES];
static unsigned sequence;
static u64 qualification_instance;
static const unsigned sizes[] = {0, 1, 511, 512, 4096, 8193, MG_FILE_CONTENT_MAX_BYTES};

__attribute__((noinline)) static double runtime_fp_add(double left, double right)
{
    return left + right;
}

__attribute__((noinline)) static long double runtime_long_double_add(
    long double left, long double right)
{
    return left + right;
}

static bool runtime_foundation_check(void)
{
    char text[160], short_text[4];
    int count = -1;
    const char expected[] = "4294967297|18446744073709551615|4096|-17|0x00002a";
    int length = snprintf(text, sizeof(text), "%ld|%lu|%zu|%zd|%#08x",
        4294967297L, ULONG_MAX, (size_t)4096, (ptrdiff_t)-17, 42U);
    if (length != (int)(sizeof(expected) - 1) || strcmp(text, expected)) return false;
    if (snprintf(short_text, sizeof(short_text), "%ld%n", 4294967297L,
                 &count) != 10 || count != 10 || strcmp(short_text, "429"))
        return false;
    if (snprintf(NULL, 0, "%hhu", (unsigned char)255) != 3) return false;
    errno = 0;
    if (snprintf(text, sizeof(text), "%q", 1) != -1 || errno != EINVAL)
        return false;
    errno = 0;
    if (snprintf(text, sizeof(text), "%f", 1.0) != -1 || errno != ENOSYS)
        return false;
    if (!isalpha('Q') || !isdigit('7') || isalpha(0xc3) ||
        tolower('Q') != 'q') return false;
    errno = 0;
    if (strtol("9223372036854775808", NULL, 10) != LONG_MAX ||
        errno != ERANGE) return false;
    volatile double fp_left = 1.25, fp_right = 3.5;
    if (runtime_fp_add(fp_left, fp_right) != 4.75) return false;
    volatile long double ld_left = 1.5L, ld_right = 2.25L;
    if (runtime_long_double_add(ld_left, ld_right) != 3.75L) return false;
    void *memory = malloc(32);
    if (!memory || ((uintptr_t)memory & 15U)) return false;
    memset(memory, 0x5a, 32);
    void *grown = realloc(memory, 96);
    if (!grown || ((unsigned char *)grown)[0] != 0x5a ||
        ((unsigned char *)grown)[31] != 0x5a) {
        free(grown ? grown : memory);
        return false;
    }
    free(grown);
    unsigned char *zeroed = calloc(8, 4);
    if (!zeroed) return false;
    for (unsigned i = 0; i < 32; ++i) {
        if (zeroed[i] != 0) { free(zeroed); return false; }
    }
    free(zeroed);
    return true;
}

/* Failures must not strand a raw-terminal child and conceal its parent's
 * nonzero exit. This uses normal process ownership and teardown only. */
static void child_cleanup(mg_handle_t *child)
{
    if(!*child)return;
    i32 status=43;
    if(process_poll(*child,&status)==MG_ERR_WOULD_BLOCK)
        (void)process_terminate(*child,43);
    (void)process_wait(*child,&status);
    (void)handle_close(*child);
}
static bool expect(const char *step, mg_result_t want, mg_result_t got)
{
    sequence++;
    printf("SOP %u %s expected=%lld observed=%lld\n",sequence,step,(long long)want,(long long)got);
    return want==got;
}
#define CHECK(step,want,got) do { if (!expect(step,want,got)) return 43; } while(0)
static void pattern(const char *run, unsigned length, unsigned version)
{
    for(unsigned i=0;i<length;i++) input[i]=(unsigned char)(i*29U+(i>>8)*7U+(unsigned char)run[i%16]+version*53U);
}
static bool bytes(unsigned length)
{
    for(unsigned i=0;i<length;i++) if(input[i]!=output[i]) {
        printf("FAIL safe-byte offset=%u expected=%u observed=%u\n",i,input[i],output[i]);return false;
    }
    return true;
}
static mg_result_t owners(void)
{
    mg_recovery_owner_info_t records[MG_RECOVERY_OWNER_MAX];u32 count=0;
    mg_recovery_owner_snapshot_request_t request={MG_RECOVERY_OWNER_MAX,records,&count};
    mg_result_t result=recovery_owners_snapshot(&request);
    if(result!=MG_OK)return result;
    if(count>MG_RECOVERY_OWNER_MAX || !qualification_instance)return MG_ERR_PROTOCOL;
    unsigned matched=0;
    for(u32 i=0;i<count;i++) if(records[i].instance_id==qualification_instance) {
        if(records[i].state!=MG_RECOVERY_OWNER_ACTIVE && records[i].state!=MG_RECOVERY_OWNER_UNRESOLVED)return MG_ERR_PROTOCOL;
        matched++;
    }
    return matched;
}
static int verify_file(const char *path,const char *run,unsigned length,unsigned version)
{
    mg_path_info_t info;mg_file_lines_state_t state={0};
    CHECK("stat",MG_OK,path_info(path,&info));
    CHECK("type",MG_PATH_TYPE_FILE,info.type);CHECK("length",length,info.size);
    mg_result_t h=file_open(path,MG_OPEN_READ);CHECK("open-read",1,h>0?1:h);
    CHECK("snapshot",MG_OK,file_snapshot((mg_handle_t)h,(char *)output,length?length:1,&state));
    CHECK("snapshot-length",length,state.length);CHECK("snapshot-identity",info.identifier,state.identifier);
    CHECK("snapshot-revision",1,state.revision!=0);
    pattern(run,length,version);CHECK("snapshot-bytes",1,bytes(length));
    CHECK("read",length,object_read((mg_handle_t)h,output,length));CHECK("read-bytes",1,bytes(length));
    CHECK("eof",0,object_read((mg_handle_t)h,output,1));
    CHECK("read-only-replace",MG_ERR_INVALID_HANDLE,file_replace((mg_handle_t)h,input,length));
    CHECK("close-read",MG_OK,handle_close((mg_handle_t)h));return 0;
}
int fstest_safe_phase(const char *mode,const char *run,const char *target,const char *backend,u64 instance)
{
    qualification_instance=instance;
    sequence=0;bool create=!strcmp(mode,"create");char dir[256],path[256],moved[256];
    snprintf(dir,sizeof(dir),"%s/safe-%s",target,run);
    printf("SAFE1 phase=%s run=%s filesystem=%s target=%s\n",mode,run,backend,target);
    if (!runtime_foundation_check()) {
        printf("FAIL runtime-foundations\n");
        return 43;
    }
    puts("RT1 PASS core-c17-runtime");
    CHECK("owners-before",0,owners());
    if(create) CHECK("mkdir",MG_OK,directory_create(dir));
    for(unsigned index=0;index<sizeof(sizes)/sizeof(sizes[0]);index++) {
        unsigned length=sizes[index];snprintf(path,sizeof(path),"%s/s%u.bin",dir,length);
        snprintf(moved,sizeof(moved),"%s/m%u.bin",dir,length);
        printf("SAFE1 size=%u\n",length);
        if(create) {
            mg_handle_t h=0,unexpected=0;mg_file_lines_state_t before={0},after={0};
            pattern(run,length,1);
            CHECK("exclusive",MG_OK,file_create_contents(path,input,length,&h));CHECK("handle",1,h!=0);
            CHECK("collision",MG_ERR_ALREADY_EXISTS,file_create_contents(path,input,length,&unexpected));
            CHECK("collision-handle",0,unexpected);
            if(strcmp(backend,"mgfs")) { /* Native ASCII equivalence, not a locale oracle. */
                char alias[256];snprintf(alias,sizeof(alias),"%s/S%u.BIN",dir,length);
                CHECK("case-collision",MG_ERR_ALREADY_EXISTS,file_create_contents(alias,input,length,&unexpected));
            }
            CHECK("snapshot-initial",MG_OK,file_snapshot(h,(char *)output,length?length:1,&before));
            CHECK("initial-length",length,before.length);CHECK("initial-bytes",1,bytes(length));
            if(length>1) {
                mg_file_lines_state_t unchanged={123,456,789,42};
                CHECK("short-snapshot",MG_ERR_BUFFER_TOO_SMALL,file_snapshot(h,(char *)output,length-1,&unchanged));
                CHECK("short-state",1,unchanged.identifier==123 && unchanged.revision==456 && unchanged.length==789 && unchanged.append_offset==42);
            }
            mg_result_t peer=file_open(path,MG_OPEN_RDWR);CHECK("peer-open",1,peer>0?1:peer);
            pattern(run,length,2);CHECK("peer-replace",MG_OK,file_replace((mg_handle_t)peer,input,length));
            CHECK("stale-refusal",MG_ERR_STALE_GENERATION,file_replace(h,input,length));
            CHECK("refresh",MG_OK,file_snapshot(h,(char *)output,length?length:1,&after));
            CHECK("peer-bytes",1,bytes(length));CHECK("stable-identity",before.identifier,after.identifier);
            CHECK("new-revision",1,after.revision!=before.revision);
            CHECK("seek",MG_OK,file_seek(h,0,MG_SEEK_SET));
            pattern(run,length,3);CHECK("replace",MG_OK,file_replace(h,input,length));
            pattern(run,length,4);CHECK("repeat-replace",MG_OK,file_replace(h,input,length));
            CHECK("ordinary-after-replace",length?1:0,object_write(h,input,length?1:0));
            CHECK("ordinary-rewind",MG_OK,file_seek(h,0,MG_SEEK_SET));
            CHECK("snapshot-final",MG_OK,file_snapshot(h,(char *)output,length?length:1,&after));
            CHECK("final-bytes",1,bytes(length));
            CHECK("offset-preserved",length,object_read(h,output,length));CHECK("offset-bytes",1,bytes(length));
            CHECK("rename-open",MG_OK,path_move(path,moved));
            CHECK("snapshot-moved",MG_OK,file_snapshot(h,(char *)output,length?length:1,&after));
            CHECK("moved-bytes",1,bytes(length));CHECK("move-back",MG_OK,path_move(moved,path));
            CHECK("peer-close",MG_OK,handle_close((mg_handle_t)peer));CHECK("close",MG_OK,handle_close(h));
        }
        if(verify_file(path,run,length,4))return 43;
    }
    mg_result_t cursor=directory_open(dir);CHECK("directory-open",1,cursor>0?1:cursor);
    bool seen[7]={0};unsigned count=0;mg_directory_entry_t entry;mg_result_t result;
    while((result=directory_read((mg_handle_t)cursor,&entry))==MG_OK) {
        bool found=false;
        for(unsigned i=0;i<7;i++) { char name[32];snprintf(name,sizeof(name),"s%u.bin",sizes[i]);
            if(!strcmp(entry.name,name)) { if(seen[i] || entry.type!=MG_PATH_TYPE_FILE)return 43;seen[i]=true;found=true;break; } }
        if(!found || ++count>7)return 43;
    }
    CHECK("directory-eof",MG_ERR_END_OF_FILE,result);CHECK("namespace",7,count);
    CHECK("directory-close",MG_OK,handle_close((mg_handle_t)cursor));
    if(create) {
        snprintf(path,sizeof(path),"%s/reused.bin",dir);mg_handle_t h=0;
        CHECK("reuse-create",MG_OK,file_create_contents(path,input,511,&h));CHECK("reuse-close",MG_OK,handle_close(h));
        CHECK("reuse-remove",MG_OK,path_remove(path));
        CHECK("reuse-recreate",MG_OK,file_create_contents(path,input,512,&h));CHECK("reuse-reclose",MG_OK,handle_close(h));
        CHECK("reuse-reremove",MG_OK,path_remove(path));
        snprintf(path,sizeof(path),"%s/Résumé-safe.txt",dir);
        CHECK("unicode-create",MG_OK,file_create_contents(path,input,3,&h));CHECK("unicode-close",MG_OK,handle_close(h));
        snprintf(moved,sizeof(moved),"%s/RÉSUMÉ-SAFE.TXT",dir);
        if(strcmp(backend,"mgfs")) CHECK("unicode-collision",MG_ERR_ALREADY_EXISTS,file_create_contents(moved,input,3,&h));
        else { CHECK("unicode-distinct",MG_OK,file_create_contents(moved,input,3,&h));CHECK("unicode-peer-close",MG_OK,handle_close(h));CHECK("unicode-peer-remove",MG_OK,path_remove(moved)); }
        CHECK("unicode-remove",MG_OK,path_remove(path));
        snprintf(path,sizeof(path),"%s/missing/file",dir);
        CHECK("missing-parent",MG_ERR_NOT_FOUND,file_create_contents(path,input,1,&h));
        CHECK("permission",MG_ERR_ACCESS_DENIED,file_create_contents("/core/forbidden-safe",input,1,&h));
        CHECK("too-large",MG_ERR_REPLACEMENT_TOO_LARGE,file_create_contents(path,input,MG_FILE_CONTENT_MAX_BYTES+1U,&h));
    }
    CHECK("owners-after",0,owners());
    printf("SAFE1 complete phase=%s run=%s ops=%u\n",mode,run,sequence);return 0;
}

static int text_file(const char *path,const char *expected)
{
    mg_result_t h=file_open(path,MG_OPEN_READ);CHECK("text-open",1,h>0?1:h);
    mg_file_lines_state_t state={0};CHECK("text-snapshot",MG_OK,file_snapshot((mg_handle_t)h,(char *)output,128,&state));
    CHECK("text-bytes",1,state.length==strlen(expected) && !memcmp(output,expected,state.length));
    CHECK("text-close",MG_OK,handle_close((mg_handle_t)h));return 0;
}
/* Bounded delivery setup for a telemetry datagram, before entering Reed.
 * Native UDP can initially issue ARP and report NETWORK_UNAVAILABLE. Explicit
 * request/acknowledgement handles that transport condition without retrying any
 * filesystem operation. The peer and nonce are checked in both directions. */
static bool channel_ready(mg_handle_t channel,const mg_net_endpoint_t *peer,const char *run)
{
    char request[80],reply[32];mg_datagram_result_t result;
    int length=snprintf(request,sizeof(request),"REED1 channel run=%s\n",run);
    for(unsigned attempt=0;attempt<64;attempt++) {
        mg_result_t sent=mg_datagram_send(channel,peer,request,(usize)length,100);
        if(sent!=length && sent!=MG_ERR_NETWORK_UNAVAILABLE)return false;
        mg_result_t got=mg_datagram_receive(channel,reply,sizeof(reply),100,&result);
        printf("REED1 channel-attempt=%u sent=%lld received=%lld\n",attempt+1,(long long)sent,(long long)got);
        if(got==16)return !memcmp(reply,run,16) && result.source.port==peer->port && !memcmp(result.source.address.octet,peer->address.octet,4);
        if(got!=MG_ERR_TIMEOUT)return false;
    }
    return false;
}

/* Launch unmodified Reed; only the host keyboard drives its editor. The parent
 * observes the first real save, then changes that same file through an independent
 * native handle. A deadline bounds condition polling; elapsed time never proves a save. */
int fstest_reed_phase(const char *mode,const char *run,const char *target,u16 signal_port)
{
    sequence=0;char path[256],occupied[256],copy[256];mg_handle_t h=0;
    snprintf(path,sizeof(path),"%s/reed.txt",target);snprintf(occupied,sizeof(occupied),"%s/occupied.txt",target);
    snprintf(copy,sizeof(copy),"%s/copied.txt",target);
    printf("REED1 phase=%s run=%s\n",mode,run);
    char cwd[256];usize needed=0;
    mg_result_t cwd_result=process_getcwd(cwd,sizeof(cwd),&needed);
    CHECK("reed-getcwd",1,cwd_result>0 && needed==strlen(cwd)+1);
    CHECK("reed-chdir",MG_OK,process_chdir(target));
    if(!strcmp(mode,"create")) {
        CHECK("reed-source",MG_OK,file_create_contents(path,"base\n",5,&h));CHECK("reed-source-close",MG_OK,handle_close(h));
        CHECK("reed-occupied",MG_OK,file_create_contents(occupied,"occupied\n",9,&h));CHECK("reed-occupied-close",MG_OK,handle_close(h));
        mg_net_info_t network;
        CHECK("reed-network",MG_OK,mg_net_info(&network));CHECK("reed-network-ready",1,network.configured);
        mg_handle_t channel=0;mg_net_endpoint_t endpoint={.address={{10,0,2,2}},.port=signal_port};
        CHECK("reed-signal-open",MG_OK,mg_datagram_open(0,&channel));
        CHECK("reed-channel-ready",1,channel_ready(channel,&endpoint,run));
        const char *args[]={"/bin/reed","reed.txt"};mg_result_t child=process_spawn_argv(args,2);CHECK("reed-spawn",1,child>0?1:child);
        mg_handle_t guard __attribute__((cleanup(child_cleanup)))=(mg_handle_t)child;
        mg_monotonic_time_t start,now;CHECK("reed-clock",MG_OK,mg_clock_monotonic(&start));
        for(;;) {
            mg_result_t f=file_open(path,MG_OPEN_READ);if(f<=0) {printf("FAIL reed-observe-open result=%lld\n",(long long)f);return 43;}
            mg_file_lines_state_t state={0};mg_result_t result=file_snapshot((mg_handle_t)f,(char *)output,128,&state);
            mg_result_t closed=handle_close((mg_handle_t)f);
            if(closed!=MG_OK || result!=MG_OK) {printf("FAIL reed-observe snapshot=%lld close=%lld\n",(long long)result,(long long)closed);return 43;}
            if(state.length==11 && !memcmp(output,"editedbase\n",11))break;
            i32 status=0;if(process_poll((mg_handle_t)child,&status)==MG_OK)return 43;
            if(mg_clock_monotonic(&now)!=MG_OK || now.milliseconds<start.milliseconds || now.milliseconds-start.milliseconds>90000)return 43;
            if(process_sleep_ms(20)!=MG_OK)return 43;
        }
        CHECK("reed-first-save",1,1);
        mg_result_t peer=file_open(path,MG_OPEN_RDWR);CHECK("reed-peer",1,peer>0?1:peer);
        CHECK("reed-external",MG_OK,file_replace((mg_handle_t)peer,"external\n",9));CHECK("reed-peer-close",MG_OK,handle_close((mg_handle_t)peer));
        char marker[80];int n=snprintf(marker,sizeof(marker),"REED1 external run=%s\n",run);
        /* Alternate-screen ownership correctly excludes parent console I/O.
         * Use the existing native datagram API only for a run-bound test
         * notification; it carries no expected data or filesystem authority. */
        CHECK("reed-external-witness",n,mg_datagram_send(channel,&endpoint,marker,(usize)n,5000));
        CHECK("reed-signal-close",MG_OK,mg_datagram_close(channel));
        i32 status=-1;CHECK("reed-wait",MG_OK,process_wait((mg_handle_t)child,&status));CHECK("reed-exit",0,status);
        mg_result_t closed=handle_close((mg_handle_t)child);guard=0;CHECK("reed-child-close",MG_OK,closed);
    } else {
        const char *args[]={"/bin/reed","copied.txt"};mg_result_t child=process_spawn_argv(args,2);CHECK("reed-reopen",1,child>0?1:child);
        i32 status=-1;CHECK("reed-rewait",MG_OK,process_wait((mg_handle_t)child,&status));CHECK("reed-reexit",0,status);
        CHECK("reed-reclose",MG_OK,handle_close((mg_handle_t)child));
    }
    if(text_file(path,"external\n") || text_file(copy,"editedbase\n") || text_file(occupied,"occupied\n"))return 43;
    CHECK("reed-restore-cwd",MG_OK,process_chdir(cwd));
    CHECK("reed-no-owner",0,owners());printf("REED1 complete phase=%s run=%s ops=%u\n",mode,run,sequence);return 0;
}

int fstest_shoot_phase(const char *run)
{
    sequence=0;printf("SHOOT1 phase=create run=%s\n",run);
    mg_result_t child=process_spawn_with_output("/bin/shoot",0);
    CHECK("shoot-spawn",1,child>0?1:child);i32 status=-1;
    CHECK("shoot-wait",MG_OK,process_wait((mg_handle_t)child,&status));CHECK("shoot-status",0,status);
    CHECK("shoot-close",MG_OK,handle_close((mg_handle_t)child));
    if(text_file("/temp/shoot.txt","shoot\n"))return 43;
    const char *good="/bin/type /temp/shoot.txt > /temp/shoot-copy.txt\n";
    const char *bad="/bin/type /temp/no-such-safe-input\n/bin/say forbidden-continuation > /temp/forbidden\n";
    mg_handle_t h=0;
    CHECK("shoot-script-create",MG_OK,file_create_contents("/temp/safe-good.shoot",good,strlen(good),&h));
    CHECK("shoot-script-close",MG_OK,handle_close(h));
    CHECK("shoot-bad-create",MG_OK,file_create_contents("/temp/safe-bad.shoot",bad,strlen(bad),&h));
    CHECK("shoot-bad-close",MG_OK,handle_close(h));
    const char *good_args[]={"/bin/shoot","/temp/safe-good.shoot"};
    child=process_spawn_argv(good_args,2);CHECK("shoot-script-spawn",1,child>0?1:child);
    CHECK("shoot-script-wait",MG_OK,process_wait((mg_handle_t)child,&status));CHECK("shoot-script-status",0,status);
    CHECK("shoot-script-child-close",MG_OK,handle_close((mg_handle_t)child));
    if(text_file("/temp/shoot-copy.txt","shoot\n"))return 43;
    const char *bad_args[]={"/bin/shoot","/temp/safe-bad.shoot"};
    child=process_spawn_argv(bad_args,2);CHECK("shoot-bad-spawn",1,child>0?1:child);
    CHECK("shoot-bad-wait",MG_OK,process_wait((mg_handle_t)child,&status));CHECK("shoot-bad-status",1,status);
    CHECK("shoot-bad-child-close",MG_OK,handle_close((mg_handle_t)child));
    mg_path_info_t info;CHECK("shoot-no-continuation",MG_ERR_NOT_FOUND,path_info("/temp/forbidden",&info));
    printf("SHOOT1 complete phase=create run=%s ops=%u\n",run,sequence);return 0;
}

int fstest_safe_readonly(const char *run,const char *target,u64 instance)
{
    qualification_instance=instance;
    sequence=0;printf("READSAFE1 phase=readonly run=%s\n",run);
    char path[256];snprintf(path,sizeof(path),"%s/forbidden",target);mg_handle_t h=123;
    CHECK("readonly-exclusive",MG_ERR_ACCESS_DENIED,file_create_contents(path,input,8193,&h));
    CHECK("readonly-output-unchanged",123,h);mg_path_info_t info;
    CHECK("readonly-absent",MG_ERR_NOT_FOUND,path_info(path,&info));
    snprintf(path,sizeof(path),"%s/safe-%s/s8193.bin",target,run);
    if(verify_file(path,run,8193,4))return 43;
    char cwd[256];usize needed=0;mg_result_t result=process_getcwd(cwd,sizeof(cwd),&needed);
    CHECK("readonly-getcwd",1,result>0 && needed==strlen(cwd)+1);
    CHECK("readonly-chdir",MG_OK,process_chdir(target));
    const char *args[]={"/bin/reed","readonly-new.txt"};mg_result_t child=process_spawn_argv(args,2);
    CHECK("readonly-reed-spawn",1,child>0?1:child);i32 status=-1;
    CHECK("readonly-reed-wait",MG_OK,process_wait((mg_handle_t)child,&status));CHECK("readonly-reed-exit",0,status);
    CHECK("readonly-reed-close",MG_OK,handle_close((mg_handle_t)child));
    CHECK("readonly-reed-absent",MG_ERR_NOT_FOUND,path_info("forbidden",&info));
    CHECK("readonly-restore",MG_OK,process_chdir(cwd));CHECK("readonly-no-owner",0,owners());
    printf("READSAFE1 complete phase=readonly run=%s ops=%u\n",run,sequence);return 0;
}
