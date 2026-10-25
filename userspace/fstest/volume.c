/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/volume_service.h>
#include <mg/time.h>
#include <mg/inspection.h>
#include <stdio.h>
#include <string.h>
#include "../common/volume_client.h"
#include "../common/storage_snapshot.h"

/* No filesystem mutation implementation lives here: every operation is native. */
int fstest_file_phase(const char *, const char *, const char *);
int fstest_safe_phase(const char *,const char *,const char *,const char *,u64);
int fstest_reed_phase(const char *,const char *,const char *,u16);
int fstest_shoot_phase(const char *,const char *,bool);
int fstest_safe_readonly(const char *,const char *,u64);
int fstest_stress_phase(const char *,const char *,const char *,const char *,const char *,u64,u64);
bool fstest_target_valid(const char *);
static bool terminated(const char *s, usize n)
{ for (usize i=0;i<n;i++) if (!s[i]) return true; return false; }
static unsigned sequence;
static mg_device_info_t devices[MG_STORAGE_SNAPSHOT_MAX];
static mg_volume_info_t selected;

static bool expect(const char *step, mg_result_t want, mg_result_t got)
{
    sequence++;
    if (printf("VOP %u %s expected=%lld observed=%lld\n", sequence, step,
               (long long)want, (long long)got) < 0) return false;
    return want == got;
}
#define REQUIRE(step,want,got) do { if (!expect(step,want,got)) return 43; } while (0)

static mg_result_t find_volume(const char *label, const char *filesystem)
{
    mg_handle_t endpoint;
    mg_result_t result = service_lookup("volume", &endpoint);
    if (result != MG_OK) { printf("FAIL volume-lookup result=%lld\n",(long long)result); return result; }
    u32 offset = 0, matches = 0;
    do {
        mg_volume_request_t query = {0};
        mg_ipc_message_t message = {0}, reply = {0};
        mg_volume_response_t response;
        query.version = MG_VOLUME_PROTOCOL_VERSION;
        query.operation = MG_VOLUME_OP_SNAPSHOT;
        query.offset = offset;
        query.limit = MG_VOLUME_RESPONSE_MAX;
        message.version = MG_IPC_PROTOCOL_VERSION;
        message.type = MG_VOLUME_REQUEST;
        message.payload_length = sizeof(query);
        memcpy(message.payload, &query, sizeof(query));
        result = ipc_request(endpoint, &message, &reply);
        if (result != MG_OK) { printf("FAIL volume-request result=%lld\n",(long long)result); break; }
        if (reply.version != MG_IPC_PROTOCOL_VERSION || reply.type != MG_VOLUME_RESPONSE ||
            reply.payload_length != sizeof(response)) { printf("FAIL volume-reply version=%u type=%u length=%u required=%u\n",reply.version,reply.type,reply.payload_length,(unsigned)sizeof(response)); result=MG_ERR_PROTOCOL; break; }
        memcpy(&response, reply.payload, sizeof(response));
        result = response.result;
        if (result != MG_OK) { printf("FAIL volume-server result=%lld\n",(long long)result); break; }
        if (response.total > MG_VOLUME_MAX_RECORDS || response.count > MG_VOLUME_RESPONSE_MAX ||
            offset > response.total || response.count > response.total-offset ||
            (response.next_offset && response.next_offset != offset+response.count) ||
            (response.next_offset && !response.count)) { printf("FAIL volume-page offset=%u count=%u total=%u next=%u\n",offset,response.count,response.total,response.next_offset); result=MG_ERR_PROTOCOL; break; }
        for (u32 i=0; i<response.count; i++) {
            const mg_volume_info_t *v=&response.volumes[i];
            if (!terminated(v->label,sizeof(v->label)) ||
                !terminated(v->filesystem,sizeof(v->filesystem)) ||
                !terminated(v->mount_point,sizeof(v->mount_point)) ||
                !terminated(v->name,sizeof(v->name))) { printf("FAIL volume-string slot=%u\n",i); result=MG_ERR_PROTOCOL; break; }
            if (!strcmp(v->label,label) && !strcmp(v->filesystem,filesystem)) {
                selected=*v; matches++;
            }
        }
        if (result != MG_OK) break;
        offset=response.next_offset;
    } while (offset);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (matches>1) printf("FAIL duplicate-volumes count=%u\n",matches);
    return matches==1 ? MG_OK : matches ? MG_ERR_PROTOCOL : MG_ERR_NOT_FOUND;
}

static bool mounted(void) { return (selected.flags & MG_VOLUME_FLAG_MOUNTED)!=0; }

/* Console input remains a console even when Shoot redirects stdout to a log.
 * Secondary I/O is idle here; the reference profile retains only idle handles. */
static bool hotplug_handshake(const char *stage, const char *run)
{
    char marker[96], input = 0;
    int length=snprintf(marker,sizeof(marker),"HOTPLUG1 %s run=%s\n",stage,run);
    return length>0 && (usize)length<sizeof(marker) &&
        object_write_all(MG_STDIN_HANDLE,marker,(usize)length)==length &&
        object_read(MG_STDIN_HANDLE,&input,1)==1 && input=='\n';
}

static mg_result_t hotplug_devices(u64 instance, u64 parent, bool *absent)
{
    u32 count=0;
    memset(devices,0,sizeof(devices));
    mg_result_t result=storage_snapshot_read(devices,MG_STORAGE_SNAPSHOT_MAX,&count);
    if (result!=MG_OK) return result;
    *absent=true;
    for (u32 i=0;i<count;i++) {
        if (!devices[i].id) return MG_ERR_PROTOCOL;
        for (u32 j=0;j<i;j++) if (devices[i].id==devices[j].id) return MG_ERR_PROTOCOL;
        if (devices[i].id==instance || devices[i].id==parent) *absent=false;
    }
    return MG_OK;
}

static int prepare_workspace(const char *target, bool create)
{
    mg_path_info_t info;
    if (create) {
        mg_result_t state=path_info(target,&info);
        if (state==MG_ERR_NOT_FOUND) REQUIRE("workspace-create",MG_OK,directory_create(target));
        else REQUIRE("workspace-existing",MG_OK,state);
    }
    char seed_path[256]; snprintf(seed_path,sizeof(seed_path),"%s/seed.txt",target);
    static const char seed[]="independent-secondary\n";
    mg_result_t seed_handle=file_open(seed_path,MG_OPEN_READ);
    if (create && seed_handle==MG_ERR_NOT_FOUND) {
        REQUIRE("seed-create",MG_OK,file_create(seed_path));
        seed_handle=file_open(seed_path,MG_OPEN_RDWR);
        REQUIRE("seed-write-open",1,seed_handle>0 ? 1:seed_handle);
        REQUIRE("seed-write",sizeof(seed)-1,object_write((mg_handle_t)seed_handle,seed,sizeof(seed)-1));
        REQUIRE("seed-write-close",MG_OK,handle_close((mg_handle_t)seed_handle));
        seed_handle=file_open(seed_path,MG_OPEN_READ);
    }
    REQUIRE("seed-open",1,seed_handle>0 ? 1:seed_handle);
    char seed_bytes[sizeof(seed)];
    REQUIRE("seed-read",sizeof(seed)-1,object_read((mg_handle_t)seed_handle,seed_bytes,sizeof(seed_bytes)));
    REQUIRE("seed-bytes",0,memcmp(seed_bytes,seed,sizeof(seed)-1));
    REQUIRE("seed-close",MG_OK,handle_close((mg_handle_t)seed_handle));
    return 0;
}

/* A silent successful child is checked by its parent through native wait.
 * Intentionally open objects test exit cleanup, never a special kernel path. */
static int cwd_child(const char *target, bool offline)
{
    char cwd[256], byte[32]; usize needed = 0;
    mg_path_info_t info;
    if (offline) {
        if (process_getcwd(cwd,sizeof(cwd),&needed)!=MG_ERR_DEVICE_GONE ||
            path_info("seed.txt",&info)!=MG_ERR_DEVICE_GONE ||
            path_info("/temp",&info)!=MG_OK) return 44;
        return 0;
    }
    if (process_getcwd(cwd,sizeof(cwd),&needed)<0 || strcmp(cwd,target)) return 44;
    mg_result_t file=file_open("seed.txt",MG_OPEN_READ), cursor=directory_open(".");
    if (file<=0 || cursor<=0) return 44;
    static const char seed[]="independent-secondary\n";
    if (object_read((mg_handle_t)file,byte,sizeof(byte))!=sizeof(seed)-1 ||
        memcmp(byte,seed,sizeof(seed)-1)) return 44;
    return 0;
}

static int cwd_spawn(const char *run, const char *label, const char *filesystem,
                       const char *target, bool offline)
{
    const char *argv[]={"/bin/fstest",offline ? "volume-cwd-offline-child" : "volume-cwd-child",
                        run,label,filesystem,target};
    mg_result_t child=process_spawn_argv(argv,6);
    REQUIRE(offline ? "cwd-offline-child-spawn" : "cwd-child-spawn",1,child>0 ? 1:child);
    i32 status=-1;
    REQUIRE(offline ? "cwd-offline-child-wait" : "cwd-child-wait",MG_OK,process_wait((mg_handle_t)child,&status));
    REQUIRE(offline ? "cwd-offline-child-status" : "cwd-child-status",0,status);
    REQUIRE(offline ? "cwd-offline-child-close" : "cwd-child-close",MG_OK,handle_close((mg_handle_t)child));
    return 0;
}

static int cwd_prepare(const char *target, const char *run, const char *label,
                         const char *filesystem, const char *mount)
{
    char source[256], destination[256], moved[256], cwd[256]; usize needed=0;
    snprintf(source,sizeof(source),"%s/cwd-source",target);
    snprintf(destination,sizeof(destination),"%s/cwd-parent",target);
    snprintf(moved,sizeof(moved),"%s/cwd-parent/moved",target);
    REQUIRE("cwd-source-mkdir",MG_OK,directory_create(source));
    REQUIRE("cwd-parent-mkdir",MG_OK,directory_create(destination));
    REQUIRE("cwd-source-chdir",MG_OK,process_chdir(source));
    REQUIRE("cwd-move",MG_OK,path_move(source,moved));
    REQUIRE("cwd-moved-getcwd",1,process_getcwd(cwd,sizeof(cwd),&needed)>0 && !strcmp(cwd,moved));
    REQUIRE("cwd-remove-busy",MG_ERR_BUSY,path_remove(moved));
    REQUIRE("cwd-ancestry",MG_OK,process_chdir("../.."));
    REQUIRE("cwd-parent-getcwd",1,process_getcwd(cwd,sizeof(cwd),&needed)>0 && !strcmp(cwd,target));
    REQUIRE("cwd-moved-remove",MG_OK,path_remove(moved));
    REQUIRE("cwd-parent-remove",MG_OK,path_remove(destination));
    REQUIRE("cwd-mount-root",MG_OK,process_chdir(".."));
    REQUIRE("cwd-mount-getcwd",1,process_getcwd(cwd,sizeof(cwd),&needed)>0 && !strcmp(cwd,mount));
    REQUIRE("cwd-mount-crossing",MG_OK,process_chdir(".."));
    REQUIRE("cwd-mount-parent-getcwd",1,process_getcwd(cwd,sizeof(cwd),&needed)>0 && !strcmp(cwd,"/vol"));
    REQUIRE("cwd-return-target",MG_OK,process_chdir(target));
    if (cwd_spawn(run,label,filesystem,target,false)) return 43;
    mg_volume_operation_response_t response;
    REQUIRE("cwd-unmount-busy",MG_ERR_BUSY,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
    return 0;
}

static int cwd_offline(const char *target, const char *run, const char *label,
                         const char *filesystem)
{
    char cwd[256], byte=0;usize needed=0;mg_path_info_t info;
    REQUIRE("cwd-offline-getcwd",MG_ERR_DEVICE_GONE,process_getcwd(cwd,sizeof(cwd),&needed));
    REQUIRE("cwd-offline-info",MG_ERR_DEVICE_GONE,path_info("seed.txt",&info));
    REQUIRE("cwd-offline-open",MG_ERR_DEVICE_GONE,file_open("seed.txt",MG_OPEN_READ));
    REQUIRE("cwd-offline-create",MG_ERR_DEVICE_GONE,file_create("new"));
    mg_handle_t created=123;
    REQUIRE("cwd-offline-exclusive",MG_ERR_DEVICE_GONE,file_create_contents("new",&byte,1,&created));
    REQUIRE("cwd-offline-exclusive-output",123,created);
    REQUIRE("cwd-offline-mkdir",MG_ERR_DEVICE_GONE,directory_create("new"));
    REQUIRE("cwd-offline-remove",MG_ERR_DEVICE_GONE,path_remove("seed.txt"));
    REQUIRE("cwd-offline-move",MG_ERR_DEVICE_GONE,path_move("seed.txt","new"));
    const char *offline_argv[]={"seed.txt"};
    REQUIRE("cwd-offline-spawn",MG_ERR_DEVICE_GONE,
            process_spawn_argv(offline_argv,
                sizeof(offline_argv)/sizeof(offline_argv[0])));
    const char *relative[]={"seed.txt"};
    REQUIRE("cwd-offline-spawn-argv",MG_ERR_DEVICE_GONE,process_spawn_argv(relative,1));
    REQUIRE("cwd-absolute-root",MG_OK,path_info("/temp",&info));
    (void)byte;
    return cwd_spawn(run,label,filesystem,target,true);
}

static int cwd_replacement(const char *target)
{
    mg_path_info_t info;char cwd[256], bytes[32];usize needed=0;
    REQUIRE("cwd-replacement-refusal",MG_ERR_DEVICE_GONE,path_info("seed.txt",&info));
    mg_handle_t created=123;
    REQUIRE("cwd-replacement-exclusive",MG_ERR_DEVICE_GONE,file_create_contents("new","x",1,&created));
    REQUIRE("cwd-replacement-exclusive-output",123,created);
    REQUIRE("cwd-explicit-rebind",MG_OK,process_chdir(target));
    REQUIRE("cwd-rebound-getcwd",1,process_getcwd(cwd,sizeof(cwd),&needed)>0 && !strcmp(cwd,target));
    mg_result_t handle=file_open("seed.txt",MG_OPEN_READ);
    REQUIRE("cwd-fresh-relative-open",1,handle>0 ? 1:handle);
    static const char seed[]="independent-secondary\n";
    REQUIRE("cwd-fresh-relative-read",sizeof(seed)-1,object_read((mg_handle_t)handle,bytes,sizeof(bytes)));
    REQUIRE("cwd-fresh-relative-content",0,memcmp(bytes,seed,sizeof(seed)-1));
    REQUIRE("cwd-fresh-relative-close",MG_OK,handle_close((mg_handle_t)handle));
    REQUIRE("cwd-root-recovery",MG_OK,process_chdir("/"));
    return 0;
}

static int removed_safe_file(mg_handle_t file, const char *phase)
{
    char byte='x';mg_file_lines_state_t state, original;
    memset(&state,0x5a,sizeof(state));original=state;
    char step[64];snprintf(step,sizeof(step),"%s-stale-snapshot",phase);
    REQUIRE(step,MG_ERR_DEVICE_GONE,file_snapshot(file,&byte,1,&state));
    snprintf(step,sizeof(step),"%s-stale-snapshot-output",phase);
    REQUIRE(step,1,byte=='x' && !memcmp(&state,&original,sizeof(state)));
    snprintf(step,sizeof(step),"%s-stale-replace",phase);
    REQUIRE(step,MG_ERR_DEVICE_GONE,file_replace(file,&byte,1));
    return 0;
}

static int fstest_hotplug(const char *run, const char *label, const char *filesystem,
                            const char *target, bool references, bool cwd)
{
    char mount[MG_VOLUME_MOUNT_MAX], path[256];
    snprintf(mount,sizeof(mount),"/vol/%s",label);
    snprintf(path,sizeof(path),"%s/mgfs-%s.bin",target,run);
    printf("HOTPLUG1 phase=idle run=%s label=%s target=%s\n",run,label,target);
    char expected_target[192];
    snprintf(expected_target,sizeof(expected_target),"%s/work",mount);
    REQUIRE("target-volume",0,strcmp(target,expected_target));
    REQUIRE("initial-volume",MG_OK,find_volume(label,filesystem));
    REQUIRE("initial-mounted",1,mounted() && !strcmp(selected.mount_point,mount));
    REQUIRE("initial-policy",1,(selected.flags&MG_VOLUME_FLAG_REMOVABLE) &&
        !(selected.flags&(MG_VOLUME_FLAG_SYSTEM_MANAGED|MG_VOLUME_FLAG_READ_ONLY)));
    mg_mangrove_time_t realtime;
    REQUIRE("hotplug-utc-clock",MG_OK,mg_clock_realtime(&realtime));
    REQUIRE("hotplug-whole-second-clock",0,realtime.nanoseconds);
    const u64 instance=selected.instance_id, parent=selected.parent_instance_id;
    REQUIRE("initial-identity",1,instance && parent && instance!=parent);
    bool absent=false;
    REQUIRE("initial-devices",MG_OK,hotplug_devices(instance,parent,&absent));
    bool leaf=false, usb=false;
    for (u32 i=0;i<MG_STORAGE_SNAPSHOT_MAX && devices[i].id;i++) {
        if (devices[i].id==instance) leaf=devices[i].parent_id==parent &&
            devices[i].category==MG_DEVICE_CATEGORY_PARTITION;
        if (devices[i].id==parent) usb=!strcmp(devices[i].connection,"usb");
    }
    REQUIRE("initial-gpt-usb",1,leaf && usb && !absent);
    printf("HOTPLUG1 original instance=%llu parent=%llu\n",
           (unsigned long long)instance,(unsigned long long)parent);
    if (prepare_workspace(target,true)) return 43;
    if (fstest_file_phase("create",run,target)) return 43;
    if (cwd && cwd_prepare(target,run,label,filesystem,mount)) return 43;
    mg_handle_t retained_file=0, retained_cursor=0;
    if (references) {
        mg_result_t result=file_open(path,MG_OPEN_RDWR);
        REQUIRE("retained-file-open",1,result>0 ? 1:result);retained_file=(mg_handle_t)result;
        result=directory_open(target);
        REQUIRE("retained-cursor-open",1,result>0 ? 1:result);retained_cursor=(mg_handle_t)result;
        mg_volume_operation_response_t response;
        REQUIRE("retained-busy",MG_ERR_BUSY,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
        mg_path_info_t current;
        REQUIRE("busy-mount-preserved",MG_OK,path_info(path,&current));
    }
    REQUIRE("idle-handshake",1,hotplug_handshake("ready",run));
    mg_monotonic_time_t started, now;
    REQUIRE("removal-clock",MG_OK,mg_clock_monotonic(&started));
    mg_path_info_t info;
    for (;;) {
        mg_result_t result=hotplug_devices(instance,parent,&absent);
        if (result!=MG_OK && result!=MG_ERR_RETRY) { REQUIRE("removal-devices",MG_OK,result); }
        if (result==MG_OK && absent) {
            result=find_volume(label,filesystem);
            if (result==MG_ERR_NOT_FOUND && path_info(path,&info)==MG_ERR_NOT_FOUND) break;
            if (result!=MG_OK && result!=MG_ERR_NOT_FOUND) { REQUIRE("removal-volume",MG_OK,result); }
        }
        if (mg_clock_monotonic(&now)!=MG_OK || now.milliseconds<started.milliseconds ||
            now.milliseconds-started.milliseconds>=30000) { REQUIRE("removal-deadline",1,0); }
        if (process_sleep_ms(10)!=MG_OK) return 43; /* Condition/deadline polling. */
    }
    REQUIRE("offline-namespace",MG_ERR_NOT_FOUND,path_info(path,&info));
    REQUIRE("offline-open",MG_ERR_NOT_FOUND,file_open(path,MG_OPEN_READ));
    if (references) {
        char byte=0;mg_directory_entry_t entry;
        REQUIRE("offline-stale-read",MG_ERR_DEVICE_GONE,object_read(retained_file,&byte,1));
        REQUIRE("offline-stale-write",MG_ERR_DEVICE_GONE,object_write(retained_file,&byte,1));
        REQUIRE("offline-stale-seek",MG_ERR_DEVICE_GONE,file_seek(retained_file,0,MG_SEEK_SET));
        REQUIRE("offline-stale-cursor",MG_ERR_DEVICE_GONE,directory_read(retained_cursor,&entry));
        if (removed_safe_file(retained_file,"offline")) return 43;
    }
    if (cwd && cwd_offline(target,run,label,filesystem)) return 43;
    /* Exercise the independent root and its services while secondary is absent. */
    if (fstest_file_phase("create",run,"/temp")) return 43;
    REQUIRE("reattach-handshake",1,hotplug_handshake("removed",run));
    REQUIRE("reattach-clock",MG_OK,mg_clock_monotonic(&started));
    for (;;) {
        mg_result_t result=find_volume(label,filesystem);
        if (result==MG_OK && mounted()) break;
        if (result!=MG_OK && result!=MG_ERR_NOT_FOUND) { REQUIRE("reattach-volume",MG_OK,result); }
        if (mg_clock_monotonic(&now)!=MG_OK || now.milliseconds<started.milliseconds ||
            now.milliseconds-started.milliseconds>=30000) { REQUIRE("reattach-deadline",1,0); }
        if (process_sleep_ms(10)!=MG_OK) return 43;
    }
    REQUIRE("replacement-identity",1,selected.instance_id && selected.parent_instance_id &&
        selected.instance_id!=instance && selected.parent_instance_id!=parent &&
        selected.instance_id!=selected.parent_instance_id);
    REQUIRE("replacement-mount",0,strcmp(selected.mount_point,mount));
    REQUIRE("replacement-devices",MG_OK,hotplug_devices(instance,parent,&absent));
    REQUIRE("old-identities-absent",1,absent);
    leaf=false;usb=false;
    for (u32 i=0;i<MG_STORAGE_SNAPSHOT_MAX && devices[i].id;i++) {
        if (devices[i].id==selected.instance_id) leaf=devices[i].parent_id==selected.parent_instance_id &&
            devices[i].category==MG_DEVICE_CATEGORY_PARTITION;
        if (devices[i].id==selected.parent_instance_id) usb=!strcmp(devices[i].connection,"usb");
    }
    REQUIRE("replacement-gpt-usb",1,leaf && usb);
    printf("HOTPLUG1 replacement instance=%llu parent=%llu\n",
           (unsigned long long)selected.instance_id,(unsigned long long)selected.parent_instance_id);
    if (references) {
        char byte=0;mg_directory_entry_t entry;
        REQUIRE("replacement-stale-read",MG_ERR_DEVICE_GONE,object_read(retained_file,&byte,1));
        REQUIRE("replacement-stale-write",MG_ERR_DEVICE_GONE,object_write(retained_file,&byte,1));
        REQUIRE("replacement-stale-cursor",MG_ERR_DEVICE_GONE,directory_read(retained_cursor,&entry));
        if (removed_safe_file(retained_file,"replacement")) return 43;
        REQUIRE("retained-file-close",MG_OK,handle_close(retained_file));
        REQUIRE("retained-cursor-close",MG_OK,handle_close(retained_cursor));
    }
    mg_volume_operation_response_t stale_response;
    REQUIRE("stale-lifecycle-unmount",MG_ERR_DEVICE_GONE,
        volume_client_call_expected(MG_VOLUME_OP_UNMOUNT,mount,instance,&stale_response));
    REQUIRE("stale-lifecycle-preserved",MG_OK,path_info(path,&info));
    if (cwd && cwd_replacement(target)) return 43;
    if (fstest_file_phase("verify",run,target)) return 43;
    mg_volume_operation_response_t response;
    REQUIRE("replacement-unmount",MG_OK,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
    REQUIRE("replacement-detached",MG_ERR_NOT_FOUND,path_info(path,&info));
    printf("HOTPLUG1 complete run=%s ops=%u\n",run,sequence);
    return 0;
}

static bool recovery_handshake(const char *stage, const char *run)
{
    char marker[96], input=0;
    int length=snprintf(marker,sizeof(marker),"RECOVERY1 %s run=%s\n",stage,run);
    return length>0 && (usize)length<sizeof(marker) &&
        object_write_all(MG_STDIN_HANDLE,marker,(usize)length)==length &&
        object_read(MG_STDIN_HANDLE,&input,1)==1 && input=='\n';
}

static mg_result_t recovery_owner(u64 instance, u64 physical, const char *stage,
                                    const char *run, mg_recovery_owner_info_t *out)
{
    mg_recovery_owner_info_t owners[MG_RECOVERY_OWNER_MAX];
    u32 count=0,matched=0;
    mg_recovery_owner_snapshot_request_t request={MG_RECOVERY_OWNER_MAX,owners,&count};
    mg_result_t result=recovery_owners_snapshot(&request);
    if (result!=MG_OK) return result;
    if (count>MG_RECOVERY_OWNER_MAX) return MG_ERR_PROTOCOL;
    for (u32 i=0;i<count;i++) if (owners[i].instance_id==instance) {
        if (owners[i].physical_instance_id!=physical || ++matched>1) return MG_ERR_PROTOCOL;
        *out=owners[i];
    }
    if (matched) printf("ROWNER stage=%s run=%s owner=%llu instance=%llu physical=%llu state=%u flags=%u\n",
        stage,run,(unsigned long long)out->owner_id,(unsigned long long)instance,
        (unsigned long long)physical,out->state,out->flags);
    return matched;
}

static int recovery_child(const char *run, const char *label, const char *backend, const char *target, bool active, bool replace, const char *filename)
{
    printf("RECOVERY1 child-start run=%s\n",run);
    REQUIRE("recovery-child-volume",MG_OK,find_volume(label,backend));
    u64 instance=selected.instance_id,physical=selected.parent_instance_id;
    char path[256];snprintf(path,sizeof(path),"%s/%s",target,filename);
    mg_result_t handle=file_open(path,MG_OPEN_RDWR);
    REQUIRE("recovery-child-open",1,handle>0 ? 1:handle);
    mg_recovery_owner_info_t owner={0};
    REQUIRE("recovery-child-owner-empty",0,recovery_owner(instance,physical,"before",run,&owner));
    if(replace) {
        char data[8193];mg_file_lines_state_t state={0};
        REQUIRE("recovery-child-bind",MG_OK,file_snapshot((mg_handle_t)handle,data,sizeof(data),&state));
        REQUIRE("recovery-child-length",8193,state.length);
    }
    REQUIRE("recovery-child-ready",1,recovery_handshake("ready",run));
    static const char data[]="controlled-fault";
    if(replace) REQUIRE("recovery-child-replace",MG_ERR_UNCERTAIN,file_replace((mg_handle_t)handle,data,sizeof(data)-1));
    else REQUIRE("recovery-child-write",MG_ERR_UNCERTAIN,object_write((mg_handle_t)handle,data,sizeof(data)-1));
    REQUIRE("recovery-child-close",MG_OK,handle_close((mg_handle_t)handle));
    REQUIRE("recovery-child-owner-online",1,recovery_owner(instance,physical,active ? "returned" : "online",run,&owner));
    REQUIRE("recovery-child-unresolved",1,owner.owner_id && owner.state==MG_RECOVERY_OWNER_UNRESOLVED &&
        (active ? !(owner.flags & MG_RECOVERY_PRIVATE) :
         owner.flags==(MG_RECOVERY_DEVICE_ONLINE|MG_RECOVERY_PHYSICAL_ONLINE)));
    u64 owner_id=owner.owner_id;
    mg_volume_operation_response_t response;
    if (!active) {
        REQUIRE("recovery-child-unmount-busy",MG_ERR_BUSY,
            volume_client_call(MG_VOLUME_OP_UNMOUNT,selected.mount_point,&response));
    }
    REQUIRE("recovery-child-uncertain",1,recovery_handshake("uncertain",run));
    mg_monotonic_time_t start,now;
    REQUIRE("recovery-child-clock",MG_OK,mg_clock_monotonic(&start));
    mg_result_t snapshot_result;
    for (;;) {
        u32 count=0;mg_recovery_owner_info_t owners[MG_RECOVERY_OWNER_MAX];
        mg_recovery_owner_snapshot_request_t request={MG_RECOVERY_OWNER_MAX,owners,&count};
        snapshot_result=recovery_owners_snapshot(&request);
        if(snapshot_result!=MG_OK) { REQUIRE("recovery-child-snapshot",MG_OK,snapshot_result); }
        bool found=false;
        for(u32 i=0;i<count;i++)if(owners[i].owner_id==owner_id) {
            if(owners[i].instance_id!=instance || owners[i].physical_instance_id!=physical ||
                owners[i].state!=MG_RECOVERY_OWNER_UNRESOLVED) return 43;
            found=(owners[i].flags==MG_RECOVERY_OFFLINE_RETAINED);
        }
        if(found)break;
        if(mg_clock_monotonic(&now)!=MG_OK || now.milliseconds<start.milliseconds ||
            now.milliseconds-start.milliseconds>=30000)return 43;
        if(process_sleep_ms(10)!=MG_OK)return 43;
    }
    REQUIRE("recovery-child-snapshot",MG_OK,snapshot_result);
    REQUIRE("recovery-child-owner-offline",1,recovery_owner(instance,physical,"offline",run,&owner));
    REQUIRE("recovery-child-owner-preserved",1,owner.owner_id==owner_id &&
        owner.state==MG_RECOVERY_OWNER_UNRESOLVED && owner.flags==MG_RECOVERY_OFFLINE_RETAINED);
    mg_path_info_t info;
    REQUIRE("recovery-child-detached",MG_ERR_NOT_FOUND,path_info(path,&info));
    bool absent=false;
    REQUIRE("recovery-child-devices",MG_OK,hotplug_devices(instance,physical,&absent));
    REQUIRE("recovery-child-absent",1,absent);
    REQUIRE("recovery-child-retained",1,recovery_handshake("retained",run));
    printf("RECOVERY1 child-complete run=%s\n",run);
    return 0;
}

static int read_failure(const char *run, const char *label, const char *target)
{
    printf("READFAIL1 start run=%s\n",run);
    REQUIRE("readfault-volume",MG_OK,find_volume(label,"mgfs"));
    u64 instance=selected.instance_id,physical=selected.parent_instance_id;
    char path[256];snprintf(path,sizeof(path),"%s/fault.bin",target);
    mg_result_t handle=file_open(path,MG_OPEN_READ);
    REQUIRE("readfault-open",1,handle>0 ? 1:handle);
    REQUIRE("readfault-ready",1,recovery_handshake("ready",run));
    char byte=0;
    REQUIRE("readfault-zero",0,object_read((mg_handle_t)handle,&byte,0));
    REQUIRE("readfault-online-error",MG_ERR_IO,object_read((mg_handle_t)handle,&byte,1));
    REQUIRE("readfault-retry-error",MG_ERR_IO,object_read((mg_handle_t)handle,&byte,1));
    REQUIRE("readfault-seek-eof",MG_OK,file_seek((mg_handle_t)handle,8193,MG_SEEK_SET));
    REQUIRE("readfault-eof",0,object_read((mg_handle_t)handle,&byte,1));
    REQUIRE("readfault-close",MG_OK,handle_close((mg_handle_t)handle));
    REQUIRE("readfault-reopen",1,(handle=file_open(path,MG_OPEN_READ))>0 ? 1:handle);
    REQUIRE("readfault-fresh-error",MG_ERR_IO,object_read((mg_handle_t)handle,&byte,1));
    const char *load_arguments[]={path};
    REQUIRE("readfault-load-error",MG_ERR_IO,process_spawn_argv(load_arguments,1));
    REQUIRE("readfault-reclose",MG_OK,handle_close((mg_handle_t)handle));
    mg_recovery_owner_info_t owner={0};
    REQUIRE("readfault-no-owner",0,recovery_owner(instance,physical,"read",run,&owner));
    mg_path_info_t info;
    REQUIRE("readfault-online-stat",MG_OK,path_info(path,&info));
    REQUIRE("readfault-length",8193,info.size);
    mg_volume_operation_response_t response;
    REQUIRE("readfault-unmount",MG_OK,volume_client_call(MG_VOLUME_OP_UNMOUNT,selected.mount_point,&response));
    REQUIRE("readfault-detached",MG_ERR_NOT_FOUND,path_info(path,&info));
    printf("READFAIL1 complete run=%s ops=%u\n",run,sequence);
    return 0;
}

/* Metadata-read failure must not become absence or authorize mutation. */
static int lookup_failure(const char *run, const char *label, const char *target)
{
    printf("LOOKUPFAIL1 start run=%s\n",run);
    REQUIRE("lookupfault-volume",MG_OK,find_volume(label,"mgfs"));
    u64 instance=selected.instance_id,physical=selected.parent_instance_id;
    char path[256],missing[256];
    snprintf(path,sizeof(path),"%s/fault.bin",target);
    snprintf(missing,sizeof(missing),"%s/absent",target);
    mg_path_info_t info;
    char cache_path[256];snprintf(cache_path,sizeof(cache_path),"%s/cache-read.bin",target);
    mg_result_t cache=file_open(cache_path,MG_OPEN_READ);
    REQUIRE("lookupfault-cache-open",1,cache>0 ? 1:cache);
    /* Bounded genuine reads displace the directory sector from the 64-entry
     * block cache. No privileged cache invalidation or metadata write. */
    u8 buffer[4096];u64 total=0;
    while(total<524288) {
        mg_result_t count=object_read((mg_handle_t)cache,buffer,sizeof(buffer));
        if(count!=(mg_result_t)sizeof(buffer))return 43;
        for(usize i=0;i<sizeof(buffer);i++)if(buffer[i]!=(u8)(total+i))return 43;
        total+=(u64)count;
    }
    REQUIRE("lookupfault-cache-read",524288,total);
    REQUIRE("lookupfault-cache-close",MG_OK,handle_close((mg_handle_t)cache));
    REQUIRE("lookupfault-ready",1,recovery_handshake("ready",run));
    REQUIRE("lookupfault-open",MG_ERR_IO,file_open(path,MG_OPEN_READ));
    REQUIRE("lookupfault-stat",MG_ERR_IO,path_info(path,&info));
    REQUIRE("lookupfault-create",MG_ERR_IO,file_create(missing));
    REQUIRE("lookupfault-mkdir",MG_ERR_IO,directory_create(missing));
    REQUIRE("lookupfault-remove",MG_ERR_IO,path_remove(path));
    REQUIRE("lookupfault-move",MG_ERR_IO,path_move(path,missing));
    const char *arguments[]={path};
    REQUIRE("lookupfault-load",MG_ERR_IO,process_spawn_argv(arguments,1));
    mg_recovery_owner_info_t owner={0};
    REQUIRE("lookupfault-no-owner",0,recovery_owner(instance,physical,"lookup",run,&owner));
    mg_volume_operation_response_t response;
    REQUIRE("lookupfault-unmount",MG_OK,volume_client_call(MG_VOLUME_OP_UNMOUNT,selected.mount_point,&response));
    REQUIRE("lookupfault-detached",MG_ERR_NOT_FOUND,path_info(path,&info));
    printf("LOOKUPFAIL1 complete run=%s ops=%u\n",run,sequence);
    return 0;
}

static int recovery_parent(const char *run, const char *label, const char *backend, const char *target, bool active, bool replace, const char *filename)
{
    printf("RECOVERY1 parent-start run=%s\n",run);
    REQUIRE("recovery-parent-volume",MG_OK,find_volume(label,backend));
    u64 instance=selected.instance_id,physical=selected.parent_instance_id;
    const char *argv[]={"/bin/fstest",replace ? "volume-replace-recovery-child" : active ? "volume-active-child" : "volume-recovery-child",run,label,backend,target,filename};
    mg_result_t child=process_spawn_argv(argv,replace ? 7 : 6);
    REQUIRE("recovery-parent-spawn",1,child>0 ? 1:child);
    i32 status=-1;
    REQUIRE("recovery-parent-wait",MG_OK,process_wait((mg_handle_t)child,&status));
    REQUIRE("recovery-parent-status",0,status);
    REQUIRE("recovery-parent-close",MG_OK,handle_close((mg_handle_t)child));
    mg_recovery_owner_info_t owner={0};
    REQUIRE("recovery-parent-owner",1,recovery_owner(instance,physical,"exited",run,&owner));
    REQUIRE("recovery-parent-retained",1,owner.state==MG_RECOVERY_OWNER_UNRESOLVED &&
        owner.flags==MG_RECOVERY_OFFLINE_RETAINED);
    if(fstest_file_phase("create",run,"/temp"))return 43;
    printf("RECOVERY1 parent-complete run=%s\n",run);
    return 0;
}

int fstest_volume_phase(int argc, char **argv)
{
    const bool safe_mode=argc>=2 && (!strcmp(argv[1],"volume-safe-create") || !strcmp(argv[1],"volume-safe-verify") || !strcmp(argv[1],"volume-safe-readonly"));
    const bool replace_mode=argc>=2 && (!strcmp(argv[1],"volume-replace-recovery") || !strcmp(argv[1],"volume-replace-recovery-child"));
    const bool stress_mode=argc>=2 && !strncmp(argv[1],"volume-stress-",14);
    if ((argc != 6 && !((safe_mode || replace_mode || stress_mode) && argc==7)) || strlen(argv[2]) != 16 || strlen(argv[3]) > 32 ||
        (strcmp(argv[4],"mgfs") && strcmp(argv[4],"fat32") && strcmp(argv[4],"exfat")) ||
        strncmp(argv[5],"/vol/",5) || !fstest_target_valid(argv[5])) return 2;
    for (usize i=0;i<16;i++) if (!((argv[2][i]>='0' && argv[2][i]<='9') ||
        (argv[2][i]>='a' && argv[2][i]<='f'))) return 2;
    if(stress_mode) {
        if(argc!=7)return 2;
        const char *mode=argv[1]+14;
        if(strcmp(mode,"create") && strcmp(mode,"verify") && strcmp(mode,"check") && strcmp(mode,"prefix") && strcmp(mode,"model") && strcmp(mode,"status") && strcmp(mode,"fault"))return 2;
        unsigned count=0;
        for(const char *p=argv[6];*p;p++){if(*p<'0'||*p>'9'||count>1024)return 2;count=count*10U+(unsigned)(*p-'0');}
        if(count<64 || count>1024)return 2;
    }
    u32 signal_port=0;
    if(safe_mode) {
        if(argc!=7 || !*argv[6])return 2;
        for(const char *p=argv[6];*p;p++) { if(*p<'0'||*p>'9'||signal_port>6553U)return 2;signal_port=signal_port*10U+(u32)(*p-'0'); }
        if(signal_port<1024U || signal_port>65535U)return 2;
    }
    if (!strcmp(argv[1],"volume-lookup-failure"))
        return strcmp(argv[4],"mgfs") ? 2 : lookup_failure(argv[2],argv[3],argv[5]);
    if (!strcmp(argv[1],"volume-read-failure"))
        return strcmp(argv[4],"mgfs") ? 2 : read_failure(argv[2],argv[3],argv[5]);
    if (!strcmp(argv[1],"volume-recovery") || !strcmp(argv[1],"volume-recovery-child") ||
        !strcmp(argv[1],"volume-active") || !strcmp(argv[1],"volume-active-child") ||
        !strcmp(argv[1],"volume-replace-recovery") || !strcmp(argv[1],"volume-replace-recovery-child")) {
        if(!replace_mode && strcmp(argv[4],"mgfs"))return 2;
        const char *filename=argc==7 ? argv[6]:"fault.bin";
        if(strlen(filename)>96 || !*filename || *filename=='/')return 2;
        for(const char *p=filename;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='.'||*p=='/'))return 2;
        for(const char *p=filename;*p;p++)if(p[0]=='.' && p[1]=='.')return 2;
        bool active=!strncmp(argv[1],"volume-active",13);
        bool replace=!strcmp(argv[1],"volume-replace-recovery") || !strcmp(argv[1],"volume-replace-recovery-child");
        return (!strcmp(argv[1],"volume-recovery") || !strcmp(argv[1],"volume-active") || !strcmp(argv[1],"volume-replace-recovery"))
            ? recovery_parent(argv[2],argv[3],argv[4],argv[5],active,replace,filename)
            : recovery_child(argv[2],argv[3],argv[4],argv[5],active,replace,filename);
    }
    if (!strcmp(argv[1],"volume-cwd-child") || !strcmp(argv[1],"volume-cwd-offline-child"))
        return cwd_child(argv[5],!strcmp(argv[1],"volume-cwd-offline-child"));
    if (!strcmp(argv[1],"volume-hotplug") || !strcmp(argv[1],"volume-hotplug-handles") ||
        !strcmp(argv[1],"volume-hotplug-cwd"))
        return fstest_hotplug(argv[2],argv[3],argv[4],argv[5],
            strcmp(argv[1],"volume-hotplug")!=0, !strcmp(argv[1],"volume-hotplug-cwd"));
    const bool safe_create=!strcmp(argv[1],"volume-safe-create");
    const bool safe_verify=!strcmp(argv[1],"volume-safe-verify");
    const bool safe_readonly=!strcmp(argv[1],"volume-safe-readonly");
    const bool create=!strcmp(argv[1],"volume-create") || safe_create || (stress_mode && (!strcmp(argv[1]+14,"create") || !strcmp(argv[1]+14,"model") || !strcmp(argv[1]+14,"status")));
    const bool verify=!strcmp(argv[1],"volume-verify");
    const bool readonly=!strcmp(argv[1],"volume-readonly") || safe_readonly;
    const bool denied=!strcmp(argv[1],"volume-denied");
    const bool child=!strcmp(argv[1],"volume-child");
    if (!create && !verify && !readonly && !denied && !child && !safe_verify && !stress_mode) return 2;
    char path[256];
    snprintf(path,sizeof(path),"%s/mgfs-%s.bin",argv[5],argv[2]);
    if (child) {
        if (file_open(path,MG_OPEN_READ)<=0 || directory_open(argv[5])<=0) return 43;
        puts("VOLUME1 child-open-handles-exit");
        return 0; /* Intentionally rely on production process teardown. */
    }
    printf("VOLUME1 phase=%s run=%s label=%s filesystem=%s target=%s\n",
           argv[1],argv[2],argv[3],argv[4],argv[5]);
    REQUIRE("volume-snapshot",MG_OK,find_volume(argv[3],argv[4]));
    REQUIRE("automounted",1,mounted());
    REQUIRE("removable-leaf",1,selected.instance_id && selected.parent_instance_id &&
        selected.instance_id!=selected.parent_instance_id &&
        (selected.flags&MG_VOLUME_FLAG_REMOVABLE) &&
        !(selected.flags&MG_VOLUME_FLAG_SYSTEM_MANAGED));
    char mount[MG_VOLUME_MOUNT_MAX];
    snprintf(mount,sizeof(mount),"/vol/%s",argv[3]);
    REQUIRE("mountpoint",0,strcmp(selected.mount_point,mount));
    char target[192]; snprintf(target,sizeof(target),"%s/work",mount);
    REQUIRE("target",0,strcmp(target,argv[5]));
    REQUIRE("readonly-state",readonly || denied,
            (selected.flags&MG_VOLUME_FLAG_READ_ONLY)!=0);
    const u64 instance=selected.instance_id, parent=selected.parent_instance_id;
    printf("VOLUME1 identity instance=%llu parent=%llu name=%s mount=%s\n",
        (unsigned long long)instance,(unsigned long long)parent,selected.name,mount);
    u32 count=0;
    REQUIRE("device-snapshot",MG_OK,storage_snapshot_read(devices,MG_STORAGE_SNAPSHOT_MAX,&count));
    bool leaf=false, usb=false, device_readonly=false;
    for (u32 i=0;i<count;i++) {
        if (devices[i].id==instance) { device_readonly=(devices[i].flags&MG_DEVICE_FLAG_READ_ONLY)!=0;
            leaf=devices[i].parent_id==parent &&
            devices[i].category==MG_DEVICE_CATEGORY_PARTITION &&
            !strcmp(devices[i].label,argv[3]) && !strcmp(devices[i].filesystem,argv[4]); }
        if (devices[i].id==parent) usb=devices[i].category==MG_DEVICE_CATEGORY_BLOCK &&
            !strcmp(devices[i].connection,"usb");
    }
    REQUIRE("gpt-leaf",1,leaf);
    REQUIRE("usb-parent",1,usb);
    REQUIRE("physical-readonly",readonly || denied,device_readonly);
    mg_mangrove_time_t time;
    REQUIRE("utc-clock",MG_OK,mg_clock_realtime(&time));
    REQUIRE("whole-second-clock",0,time.nanoseconds);
    mg_volume_mount_request_t raw_mount={0};
    mg_volume_unmount_request_t raw_unmount={0};
    mg_volume_suppression_request_t raw_suppression={0};
    raw_mount.instance_id=raw_unmount.instance_id=raw_suppression.instance_id=instance;
    raw_mount.parent_instance_id=raw_unmount.parent_instance_id=raw_suppression.parent_instance_id=parent;
    strncpy(raw_mount.filesystem,argv[4],sizeof(raw_mount.filesystem)-1);
    strncpy(raw_mount.mount_point,mount,sizeof(raw_mount.mount_point)-1);
    REQUIRE("direct-mount-denied",MG_ERR_PRIVILEGE_REQUIRED,volume_mount(&raw_mount));
    REQUIRE("direct-unmount-denied",MG_ERR_PRIVILEGE_REQUIRED,volume_unmount(&raw_unmount));
    REQUIRE("direct-suppression-denied",MG_ERR_PRIVILEGE_REQUIRED,volume_set_suppressed(&raw_suppression));
    mg_volume_operation_response_t response;
    mg_path_info_t info;
    if (prepare_workspace(target,create)) return 43;
    if(stress_mode) {
        const char *mode=argv[1]+14;
        snprintf(path,sizeof(path),"%s/stress-%s",target,argv[2]);
        if(!strcmp(mode,"fault") && !hotplug_handshake("stress-ready",argv[2]))return 43;
        if(fstest_stress_phase(mode,argv[2],argv[4],target,argv[6],instance,parent))return 43;
        if(!strcmp(mode,"fault")){printf("VOLUME1 complete phase=%s run=%s ops=%u\n",argv[1],argv[2],sequence);return 0;}
    } else if(safe_readonly) {
        snprintf(path,sizeof(path),"%s/safe-%s",target,argv[2]);
        if(fstest_safe_readonly(argv[2],target,instance))return 43;
    } else if (safe_create || safe_verify) {
        snprintf(path,sizeof(path),"%s/safe-%s",target,argv[2]);
#ifdef REED_GUEST_NO_CONFLICT
        if(safe_create && fstest_shoot_phase(argv[2],target,true))return 43;
#else
        if(safe_create && fstest_shoot_phase(argv[2],target,false))return 43;
#endif
        if(fstest_safe_phase(safe_create ? "create":"verify",argv[2],target,argv[4],instance))return 43;
        if(fstest_reed_phase(safe_create ? "create":"verify",argv[2],target,(u16)signal_port))return 43;
    } else if (denied) {
        REQUIRE("unauthorized-unmount",MG_ERR_PRIVILEGE_REQUIRED,
            volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
        REQUIRE("denied-mount-still-present",MG_OK,path_info(target,&info));
        if (fstest_file_phase("verify",argv[2],target)) return 43;
    } else if (readonly) {
        char absent[256]; snprintf(absent,sizeof(absent),"%s/forbidden",target);
        REQUIRE("readonly-create",MG_ERR_ACCESS_DENIED,file_create(absent));
        REQUIRE("readonly-mkdir",MG_ERR_ACCESS_DENIED,directory_create(absent));
        REQUIRE("readonly-absent",MG_ERR_NOT_FOUND,path_info(absent,&info));
        /* SYSCALL_OPEN currently collapses VFS open admission errors to IO.
         * It must return no writable handle; create/mkdir retain ACCESS_DENIED. */
        REQUIRE("readonly-open",MG_ERR_IO,file_open(path,MG_OPEN_RDWR));
        if (fstest_file_phase("verify",argv[2],target)) return 43;
    } else {
        if (fstest_file_phase(create ? "create" : "verify",argv[2],target)) return 43;
        if (create) {
            char scratch[256]; snprintf(scratch,sizeof(scratch),"%s/scratch-%s",target,argv[2]);
            REQUIRE("mkdir",MG_OK,directory_create(scratch));
            REQUIRE("directory-stat",MG_OK,path_info(scratch,&info));
            REQUIRE("directory-type",MG_PATH_TYPE_DIRECTORY,info.type);
            REQUIRE("rmdir",MG_OK,path_remove(scratch));
            REQUIRE("removed-directory",MG_ERR_NOT_FOUND,path_info(scratch,&info));
            REQUIRE("confirmation-denied",MG_ERR_CANCELLED,
                volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
            REQUIRE("cancelled-mount-present",MG_OK,path_info(path,&info));
            mg_result_t file=file_open(path,MG_OPEN_READ);
            REQUIRE("busy-file-open",1,file>0 ? 1:file);
            REQUIRE("busy-file",MG_ERR_BUSY,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
            REQUIRE("busy-file-preserved",MG_OK,path_info(path,&info));
            REQUIRE("busy-file-close",MG_OK,handle_close((mg_handle_t)file));
            mg_result_t cursor=directory_open(target);
            REQUIRE("busy-cursor-open",1,cursor>0 ? 1:cursor);
            REQUIRE("busy-cursor",MG_ERR_BUSY,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
            REQUIRE("busy-cursor-preserved",MG_OK,path_info(path,&info));
            REQUIRE("busy-cursor-close",MG_OK,handle_close((mg_handle_t)cursor));
            char cwd[256]; usize needed=0;
            mg_result_t cwd_result=process_getcwd(cwd,sizeof(cwd),&needed);
            REQUIRE("getcwd",1,cwd_result>0 ? 1:cwd_result);
            REQUIRE("cwd-size",1,(usize)cwd_result+1==needed && needed==strlen(cwd)+1);
            REQUIRE("chdir-volume",MG_OK,process_chdir(target));
            REQUIRE("busy-cwd",MG_ERR_BUSY,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
            REQUIRE("restore-cwd",MG_OK,process_chdir(cwd));
            const char *child_argv[]={"/bin/fstest","volume-child",argv[2],argv[3],argv[4],argv[5]};
            mg_result_t process=process_spawn_argv(child_argv,6);
            REQUIRE("child-spawn",1,process>0 ? 1:process);
            i32 status=-1;
            REQUIRE("child-wait",MG_OK,process_wait((mg_handle_t)process,&status));
            REQUIRE("child-status",0,status);
            REQUIRE("child-close",MG_OK,handle_close((mg_handle_t)process));
            REQUIRE("unmount",MG_OK,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
            REQUIRE("unmount-identity",1,response.instance_id==instance);
            REQUIRE("detached",MG_ERR_NOT_FOUND,path_info(path,&info));
            REQUIRE("suppressed-snapshot",MG_OK,find_volume(argv[3],argv[4]));
            REQUIRE("not-automounted",0,mounted());
            REQUIRE("explicit-remount",MG_OK,volume_client_call(MG_VOLUME_OP_MOUNT,selected.name,&response));
            REQUIRE("remount-identity",1,response.instance_id==instance && !strcmp(response.mount_point,mount));
            if (fstest_file_phase("verify",argv[2],target)) return 43;
        }
    }
    if (!denied) {
        REQUIRE("final-unmount",MG_OK,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
        REQUIRE("final-detached",MG_ERR_NOT_FOUND,path_info(path,&info));
        REQUIRE("final-snapshot",MG_OK,find_volume(argv[3],argv[4]));
        REQUIRE("final-suppressed",0,mounted());
    }
    printf("VOLUME1 complete phase=%s run=%s ops=%u\n",argv[1],argv[2],sequence);
    return 0;
}
