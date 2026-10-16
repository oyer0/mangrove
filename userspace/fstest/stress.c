/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Service/child supervision only. The standalone fsstress owns its own model. */
#include <mangrove.h>
#include <mg/inspection.h>
#include <stdio.h>
#include <string.h>
#include "../common/volume_client.h"
static unsigned sequence;
static int expect(const char *step,mg_result_t want,mg_result_t got)
{
    sequence++;
    if(printf("TSTEP %u %s expected=%lld observed=%lld\n",sequence,step,(long long)want,(long long)got)<0)return 0;
    return want==got;
}
#define T(step,want,got) do{if(!expect(step,want,got))return 43;}while(0)
static int child(const char **args,usize count,int expected)
{
    mg_result_t h=process_spawn_argv(args,count);T("spawn",1,h>0?1:h);
    i32 status=-1;T("wait",0,process_wait((mg_handle_t)h,&status));
    mg_result_t closed=handle_close((mg_handle_t)h);T("process-close",0,closed);
    T("status",expected,status);return 0;
}
static int owners(u64 instance,u64 parent,bool uncertain)
{
    mg_recovery_owner_info_t entries[MG_RECOVERY_OWNER_MAX];u32 count=0,matched=0;
    mg_recovery_owner_snapshot_request_t request={MG_RECOVERY_OWNER_MAX,entries,&count};
    T("owner-snapshot",0,recovery_owners_snapshot(&request));
    if(count>MG_RECOVERY_OWNER_MAX)return 43;
    for(u32 i=0;i<count;i++)if(entries[i].instance_id==instance) {
        matched++;
        if(uncertain) {
            T("owner-state",MG_RECOVERY_OWNER_UNRESOLVED,entries[i].state);
            T("owner-parent",parent,entries[i].physical_instance_id);
            printf("STRESSOWNER id=%llu instance=%llu parent=%llu state=%u flags=%u\n",(unsigned long long)entries[i].owner_id,(unsigned long long)instance,(unsigned long long)parent,entries[i].state,entries[i].flags);
        }
    }
    T("owners",uncertain?1:0,matched);return 0;
}
int fstest_stress_phase(const char *mode,const char *run,const char *backend,
    const char *target,const char *operations,u64 instance,u64 parent)
{
    sequence=0;char root[256],replayed[256],trace[128],prefix[128];
    const char *name=(!strcmp(mode,"model") || !strcmp(mode,"status") || !strcmp(mode,"fault"))?mode:"stress";
    snprintf(root,sizeof(root),"%s/%s-%s",target,name,run);
    snprintf(replayed,sizeof(replayed),"%s/%s-%s",target,!strcmp(mode,"prefix")?"prefix":"replay",run);
    snprintf(trace,sizeof(trace),"/temp/%s-%s.trace",name,run);
    snprintf(prefix,sizeof(prefix),"/temp/prefix-%s.trace",run);
    printf("STRESSCHILD phase=%s run=%s instance=%llu parent=%llu\n",mode,run,(unsigned long long)instance,(unsigned long long)parent);
    if(owners(instance,parent,false))return 43;
    if(!strcmp(mode,"create") || !strcmp(mode,"model") || !strcmp(mode,"status") || !strcmp(mode,"fault")) {
        const char *args[]={"/bin/fsstress","run",backend,run,operations,root,trace,mode};
        bool negative=!strcmp(mode,"model") || !strcmp(mode,"status");
        if(child(args,negative?8:7,!strcmp(mode,"fault")?44:0))return 43;
    } else if(!strcmp(mode,"prefix")) {
        const char *args[]={"/bin/fsstress","replay",replayed,prefix};
        if(child(args,4,0))return 43;
    } else {
        const char *args[]={"/bin/fsstress","verify",root,trace};
        if(child(args,4,0))return 43;
        if(!strcmp(mode,"verify")) {
            const char *again[]={"/bin/fsstress","replay",replayed,trace};
            if(child(again,4,0))return 43;
            const char *check[]={"/bin/fsstress","verify",replayed,trace};
            if(child(check,4,0))return 43;
        }
    }
    if(!strcmp(mode,"fault")) {
        if(owners(instance,parent,true))return 43;
        char mount[128];snprintf(mount,sizeof(mount),"/vol/%s",!strcmp(backend,"mgfs")?"QUAL_MGFS":!strcmp(backend,"fat32")?"QUAL_FAT":"QUAL_EXFAT");
        mg_volume_operation_response_t response;
        T("uncertain-unmount",MG_ERR_BUSY,volume_client_call(MG_VOLUME_OP_UNMOUNT,mount,&response));
    } else if(owners(instance,parent,false))return 43;
    printf("STRESSCHILD complete phase=%s run=%s steps=%u\n",mode,run,sequence);return 0;
}
