/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/volume_service.h>
#include <mg/time.h>
#include <stdio.h>
#include <string.h>
#include "../common/volume_client.h"
#include "../common/storage_snapshot.h"

/* No filesystem mutation implementation lives here: every operation is native. */
int fstest_file_phase(const char *, const char *, const char *);
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

static int fstest_hotplug(const char *run, const char *label, const char *target, bool references)
{
    char mount[MG_VOLUME_MOUNT_MAX], path[256];
    snprintf(mount,sizeof(mount),"/vol/%s",label);
    snprintf(path,sizeof(path),"%s/mgfs-%s.bin",target,run);
    printf("HOTPLUG1 phase=idle run=%s label=%s target=%s\n",run,label,target);
    char expected_target[192];
    snprintf(expected_target,sizeof(expected_target),"%s/work",mount);
    REQUIRE("target-volume",0,strcmp(target,expected_target));
    REQUIRE("initial-volume",MG_OK,find_volume(label,"mgfs"));
    REQUIRE("initial-mounted",1,mounted() && !strcmp(selected.mount_point,mount));
    REQUIRE("initial-policy",1,(selected.flags&MG_VOLUME_FLAG_REMOVABLE) &&
        !(selected.flags&(MG_VOLUME_FLAG_SYSTEM_MANAGED|MG_VOLUME_FLAG_READ_ONLY)));
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
    if (fstest_file_phase("create",run,target)) return 43;
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
            result=find_volume(label,"mgfs");
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
    }
    /* Exercise the independent root and its services while secondary is absent. */
    if (fstest_file_phase("create",run,"/temp")) return 43;
    REQUIRE("reattach-handshake",1,hotplug_handshake("removed",run));
    REQUIRE("reattach-clock",MG_OK,mg_clock_monotonic(&started));
    for (;;) {
        mg_result_t result=find_volume(label,"mgfs");
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
        REQUIRE("retained-file-close",MG_OK,handle_close(retained_file));
        REQUIRE("retained-cursor-close",MG_OK,handle_close(retained_cursor));
    }
    if (fstest_file_phase("verify",run,target)) return 43;
    mg_volume_operation_response_t response;
    REQUIRE("replacement-unmount",MG_OK,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
    REQUIRE("replacement-detached",MG_ERR_NOT_FOUND,path_info(path,&info));
    printf("HOTPLUG1 complete run=%s ops=%u\n",run,sequence);
    return 0;
}

int fstest_volume_phase(int argc, char **argv)
{
    if (argc != 6 || strlen(argv[2]) != 16 || strlen(argv[3]) > 32 ||
        (strcmp(argv[4],"mgfs") && strcmp(argv[4],"fat32") && strcmp(argv[4],"exfat")) ||
        strncmp(argv[5],"/vol/",5) || !fstest_target_valid(argv[5])) return 2;
    for (usize i=0;i<16;i++) if (!((argv[2][i]>='0' && argv[2][i]<='9') ||
        (argv[2][i]>='a' && argv[2][i]<='f'))) return 2;
    if (!strcmp(argv[1],"volume-hotplug") || !strcmp(argv[1],"volume-hotplug-handles"))
        return strcmp(argv[4],"mgfs") ? 2 : fstest_hotplug(argv[2],argv[3],argv[5],
            !strcmp(argv[1],"volume-hotplug-handles"));
    const bool create=!strcmp(argv[1],"volume-create");
    const bool verify=!strcmp(argv[1],"volume-verify");
    const bool readonly=!strcmp(argv[1],"volume-readonly");
    const bool denied=!strcmp(argv[1],"volume-denied");
    const bool child=!strcmp(argv[1],"volume-child");
    if (!create && !verify && !readonly && !denied && !child) return 2;
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
    if (denied) {
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
