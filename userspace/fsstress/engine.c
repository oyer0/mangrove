/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fsstress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    fs_shape shape;
    unsigned char bytes[FS_FILES][FS_BYTES],input[FS_BYTES],observed[FS_BYTES];
    uint64_t identities[FS_FILES],checks,start,witness;
    uint32_t sequence,offset,opened,first_write,mutations,ranges,resource,uncertain;
    int64_t handle;
    const char *root;
} execution;
static const char *names[]={"root","mkdir","create","write","read","reopen","truncate","move",
    "unlink","rmdir","snapshot","replace","stale","contents","case","resize","directory-move"};
static int check(execution *e,const char *phase,int want,int got,uint32_t count,uint32_t actual)
{
    e->checks++;
    if(want==got && count==actual)return 1;
    if(got==-6 || got==-36)e->resource=1;
    if(got==-30)e->resource=2;
    if(got==FS_UNCERTAIN)e->uncertain=1;
    printf("FSFAIL op=%u kind=result phase=%s expected=%d observed=%d count=%u observed_count=%u\n",e->sequence,phase,want,got,count,actual);
    return 0;
}
static int data_check(execution *e,uint32_t id,uint32_t offset,uint32_t count)
{
    for(uint32_t i=0;i<count;i++)if(e->observed[i]!=e->bytes[id][offset+i]) {
        printf("FSFAIL op=%u kind=content file=%u offset=%u expected=%u observed=%u\n",e->sequence,id,offset+i,e->bytes[id][offset+i],e->observed[i]);return 0;
    }
    e->checks++;return 1;
}
static int path(execution *e,uint32_t dir,uint32_t id,uint32_t variant,char *out)
{
    char suffix[128];if(!fs_path(&e->shape,dir,id,variant,suffix,sizeof(suffix)))return 0;
    int n=snprintf(out,256,"%s%s",e->root,suffix);return n>0 && n<256;
}
static int file_path(execution *e,uint32_t id,char *out)
{fs_file *f=&e->shape.files[id];return path(e,f->parent,id,f->variant,out);}
static int close_opened(execution *e)
{
    if(e->handle<0)return 1;
    int result=io_close(e->handle);e->handle=-1;e->opened=FS_FILES;
    return check(e,"close",FS_OK,result,0,0);
}
static int opened(execution *e,uint32_t id)
{
    if(e->handle>=0 && e->opened==id)return 1;
    char p[256];if(!close_opened(e) || !file_path(e,id,p))return 0;
    int result=io_open(p,&e->handle);
    if(!check(e,"open",FS_OK,result,0,0))return 0;
    e->opened=id;e->offset=0;return 1;
}
static int seek(execution *e,uint32_t offset)
{
    if(!check(e,"seek",0,io_seek(e->handle,offset),0,0))return 0;
    e->offset=offset;return 1;
}
static int offset_check(execution *e)
{
    if(e->handle<0)return 1;
    uint32_t size=e->shape.files[e->opened].size,count=e->offset<size?1:0;
    fs_result r=io_read(e->handle,e->observed,1);
    if(!check(e,"offset",0,r.status,count,r.count) || !data_check(e,e->opened,e->offset,count))return 0;
    return check(e,"offset-restore",0,io_seek(e->handle,e->offset),0,0);
}
static int verify_file(execution *e,uint32_t id)
{
    fs_file *f=&e->shape.files[id];char p[256];if(!file_path(e,id,p))return 0;
    fs_info info={0};int status=io_stat(p,&info);
    if(!check(e,"stat",0,status,0,0) || !check(e,"type",1,(int)info.type,0,0) || info.size!=f->size || !info.identity) {
        printf("FSFAIL op=%u kind=metadata file=%u expected_size=%u observed_size=%llu\n",e->sequence,id,f->size,(unsigned long long)info.size);return 0;
    }
    if(e->identities[id] && e->identities[id]!=info.identity){printf("FSFAIL op=%u kind=identity file=%u\n",e->sequence,id);return 0;}
    e->identities[id]=info.identity;
    int64_t h=-1;if(!check(e,"verify-open",0,io_open(p,&h),0,0))return 0;
    memset(e->observed,0xa5,FS_BYTES);
    fs_result r=io_read(h,e->observed,f->size);
    int ok=check(e,"verify-read",0,r.status,f->size,r.count) && data_check(e,id,0,f->size);
    if(ok){r=io_read(h,e->observed,1);ok=check(e,"eof",0,r.status,0,r.count);}
    if(ok){r=io_read(h,e->observed,0);ok=check(e,"zero-read",0,r.status,0,r.count);}
    int closed=io_close(h);return check(e,"verify-close",0,closed,0,0) && ok;
}
static int verify_namespace(execution *e)
{
    for(uint32_t d=0;d<=FS_DIRS;d++) {
        if(d<FS_DIRS && !e->shape.dirs[d].live)continue;
        char p[256];if(!path(e,d,FS_FILES,0,p))return 0;
        fs_entry entries[FS_FILES+FS_DIRS];uint32_t count=0;
        if(!check(e,"list",0,io_list(p,entries,FS_FILES+FS_DIRS,&count),0,0))return 0;
        uint32_t expected=0,seen=0;
        for(uint32_t i=0;i<FS_FILES;i++)if(e->shape.files[i].live && e->shape.files[i].parent==d)expected|=1U<<i;
        for(uint32_t i=0;i<FS_DIRS;i++)if(e->shape.dirs[i].live && e->shape.dirs[i].parent==d)expected|=1U<<(FS_FILES+i);
        for(uint32_t k=0;k<count;k++) {
            uint32_t bit=0;
            for(uint32_t i=0;i<FS_FILES;i++){char name[32];snprintf(name,sizeof(name),"%s-entry-%u.bin",e->shape.files[i].variant?"moved":"ordinary",i);
                if(!strcmp(name,entries[k].name) && entries[k].type==1)bit=1U<<i;}
            for(uint32_t i=0;i<FS_DIRS;i++){char name[32];snprintf(name,sizeof(name),"directory-%u",i);
                if(!strcmp(name,entries[k].name) && entries[k].type==2)bit=1U<<(FS_FILES+i);}
            if(!bit || !(expected&bit) || (seen&bit)) {
                printf("FSFAIL op=%u kind=namespace directory=%u name=%s\n",e->sequence,d,entries[k].name);return 0;
            }
            seen|=bit;
        }
        if(seen!=expected){printf("FSFAIL op=%u kind=namespace directory=%u expected=%u observed=%u\n",e->sequence,d,expected,seen);return 0;}
        e->checks++;
    }
    return 1;
}
static void model_data(execution *e,const fs_op *o,int expected)
{
    if(expected && expected!=FS_STALE)return;
    uint32_t old=e->shape.files[o->id].size;
    switch(o->action) {
    case WRITE:
        if(o->length){if(o->offset>old)memset(e->bytes[o->id]+old,0,o->offset-old);fs_payload(o->data,e->bytes[o->id]+o->offset,o->length);}break;
    case RESIZE:if(o->length>old)memset(e->bytes[o->id]+old,0,o->length-old);break;
    case CONTENTS:case REPLACE:case STALE:fs_payload(o->data,e->bytes[o->id],o->length);break;
    default:break;
    }
    fs_transition(&e->shape,o,expected);
    if(o->action==CREATE || o->action==CONTENTS)e->identities[o->id]=0;
}
static int sweep(execution *e)
{
    if(!verify_namespace(e))return 0;
    for(uint32_t i=0;i<FS_FILES;i++)if(e->shape.files[i].live && !verify_file(e,i))return 0;
    return offset_check(e);
}
int fs_execute(const fs_plan *p,const char *root,int verify)
{
    execution *e=calloc(1,sizeof(*e));if(!e){puts("FSINCOMPLETE kind=memory");return 2;}
    e->root=root;e->handle=-1;e->opened=FS_FILES;e->start=io_milliseconds();int result=43;
    if(e->start==~0ULL){puts("FSINCOMPLETE kind=clock");free(e);return 2;}
    if(printf("FSBEGIN version=1 mode=%s profile=%u caps=%u seed=%016llx ops=%u control=%u max_file=%u model_bytes=%u\n",verify?"verify":"execute",p->profile,p->caps,(unsigned long long)p->seed,p->count,p->control,FS_BYTES,(unsigned)sizeof(*e))<0){result=2;goto done;}
    if(!check(e,"parent-admission",0,io_enter(root),0,0))goto done;
    if(verify) {
        if(p->control){puts("FSINCOMPLETE kind=verify-control");result=2;goto done;}
        for(uint32_t i=0;i<p->count;i++)model_data(e,&p->ops[i],p->ops[i].expected);
        if(!sweep(e))goto done;
        result=printf("FSVERIFIED ops=%u checks=%llu\n",p->count,(unsigned long long)e->checks)<0?2:0;goto done;
    }
    for(uint32_t i=0;i<p->count;i++) {
        const fs_op *o=&p->ops[i];e->sequence=i+1;
        uint64_t now=io_milliseconds();
        if(now==~0ULL || now<e->start || now-e->start>180000){puts("FSINCOMPLETE kind=deadline");result=2;goto done;}
        char source[256],dest[256];source[0]=dest[0]=0;
        int expected=o->expected,status=0;uint32_t count=0,wantcount=0;
        fs_payload(o->data,e->input,o->length);
        if(o->action==ROOT)snprintf(source,sizeof(source),"%s",root);
        else if(o->action==MKDIR){if(!path(e,o->arg,FS_FILES,0,source))goto done;
            size_t n=strlen(source);snprintf(source+n,sizeof(source)-n,"/directory-%u",o->id);}
        else if(o->action==RMDIR || o->action==DMOVE){if(!path(e,o->id,FS_FILES,0,source))goto done;}
        else if(o->action==CREATE || o->action==CONTENTS){if(!path(e,o->arg,o->id,0,source))goto done;}
        else if(!file_path(e,o->id,source))goto done;
        switch(o->action) {
        case ROOT:case MKDIR:status=io_mkdir(source);break;
        case CREATE:status=io_create(source);break;
        case CONTENTS:
            if(!(p->caps&FS_SAFE)){status=FS_UNSUPPORTED;break;}
            if(!close_opened(e))goto done;
            status=io_contents(source,e->input,o->length,&e->handle);
            if(status && e->handle!=-1){puts("FSFAIL kind=output-handle");goto done;}
            if(!status){e->opened=o->id;e->offset=0;}break;
        case WRITE:case READ: {
            if(!opened(e,o->id) || (e->offset!=o->offset && !seek(e,o->offset)))goto done;
            fs_result r=o->action==WRITE?io_write(e->handle,e->input,o->length):io_read(e->handle,e->observed,o->length);
            status=r.status;count=r.count;wantcount=o->length;
            if(o->action==READ){uint32_t size=e->shape.files[o->id].size;wantcount=o->offset<size?size-o->offset:0;if(wantcount>o->length)wantcount=o->length;}
            if(!status)e->offset+=count;
            if(o->action==WRITE && !e->first_write++){if(p->control==2)expected=FS_EXISTS;}
            break;}
        case REOPEN:if(!close_opened(e) || !opened(e,o->id))goto done;break;
        case TRUNCATE:case RESIZE:
            if(!opened(e,o->id))goto done;
            status=io_resize(e->handle,o->action==TRUNCATE?0:o->length);break;
        case MOVE:
            if(!path(e,o->arg,o->id,e->shape.files[o->id].variant^1,dest))goto done;
            status=io_move(source,dest);break;
        case DMOVE: {
            if(!path(e,o->arg,FS_FILES,0,dest))goto done;
            size_t n=strlen(dest);snprintf(dest+n,sizeof(dest)-n,"/directory-%u",o->id);
            status=io_move(source,dest);break;}
        case UNLINK:if(!close_opened(e))goto done;status=io_remove(source,0);break;
        case RMDIR:status=io_remove(source,1);break;
        case SNAPSHOT:case REPLACE:case STALE: {
            if(!(p->caps&FS_SAFE)){status=FS_UNSUPPORTED;break;}
            if(!opened(e,o->id))goto done;
            fs_version version={0};status=io_snapshot(e->handle,e->observed,FS_BYTES,&version);
            if(!check(e,"snapshot",0,status,e->shape.files[o->id].size,(uint32_t)version.length) || version.length!=e->shape.files[o->id].size || !version.identity || !version.revision || !data_check(e,o->id,0,(uint32_t)version.length))goto done;
            if(o->action==SNAPSHOT)break;
            if(o->action==REPLACE){status=io_replace(e->handle,e->input,o->length);break;}
            int64_t peer=-1;
            if(!check(e,"peer-open",0,io_open(source,&peer),0,0))goto done;
            fs_version second={0};int saved=io_snapshot(peer,e->observed,FS_BYTES,&second);
            if(!check(e,"peer-snapshot",0,saved,0,0)){io_close(peer);goto done;}
            if(second.identity!=version.identity || second.revision!=version.revision || second.length!=version.length || !data_check(e,o->id,0,(uint32_t)version.length)){io_close(peer);goto done;}
            saved=io_replace(peer,e->input,o->length);
            int closed=io_close(peer);
            if(!check(e,"peer-replace",0,saved,0,0) || !check(e,"peer-close",0,closed,0,0))goto done;
            /* A refusal must preserve newer bytes even if a broken callback
             * writes before returning STALE. Never use identical save intents. */
            if(o->length)e->input[0]^=1;else e->input[0]=0xa5;
            status=io_replace(e->handle,e->input,o->length?o->length:1);break;}
        case CASE: {
            for(char *c=source+strlen(root);*c;c++)if(*c>='a' && *c<='z')*c=(char)(*c-'a'+'A');
            fs_info info={0};status=io_stat(source,&info);
            expected=(p->caps&FS_ASCII_CI)?FS_OK:FS_MISSING;
            if(!status && (info.type!=1 || info.size!=e->shape.files[o->id].size || info.identity!=e->identities[o->id])) {
                printf("FSFAIL op=%u kind=case-identity file=%u\n",e->sequence,o->id);goto done;}
            break;}
        default:goto done;
        }
        if(printf("FSOP %u action=%s id=%u parent=%u offset=%u length=%u data=%016llx path=%s destination=%s expected=%d observed=%d count=%u observed_count=%u\n",i+1,names[o->action],o->id,o->arg,o->offset,o->length,(unsigned long long)o->data,source,dest[0]?dest:"-",expected,status,wantcount,count)<0){result=2;goto done;}
        if(!check(e,"operation",expected,status,wantcount,count))goto done;
        if(o->action==READ && !data_check(e,o->id,o->offset,count))goto done;
        uint32_t old=e->shape.files[o->id].size;
        if(o->action==WRITE && o->length) {
            if(o->offset<old)e->ranges|=1;
            if(o->offset==old && old)e->ranges|=2;
            if(o->offset<old && o->offset+o->length>old)e->ranges|=4;
            if(o->offset/512!=(o->offset+o->length-1)/512)e->ranges|=8;
            if(o->offset/4096!=(o->offset+o->length-1)/4096)e->ranges|=16;
            if(o->offset+o->length==FS_BYTES)e->ranges|=32;
            if(o->offset>old)e->ranges|=64;
            if(o->offset && o->offset+o->length<old)e->ranges|=128;
        }
        if(o->action==READ && o->length && !count)e->ranges|=256;
        if(o->action==WRITE && !o->length)e->ranges|=512;
        if(o->action==RMDIR && status==FS_NOTEMPTY)e->ranges|=1024;
        if(o->action==CREATE && status==FS_EXISTS)e->ranges|=2048;
        if(o->action==RESIZE && !status && o->length)e->ranges|=4096;
        model_data(e,o,o->expected);
        if(p->control==1 && o->action==WRITE && e->first_write==1 && o->length)e->bytes[o->id][o->offset+o->length/2]^=1;
        if(!o->expected || o->expected==FS_STALE || o->action==CASE) {
            e->witness|=1ULL<<o->action;
            if((o->action==WRITE && o->length) || (o->action==TRUNCATE && old) || o->action==REPLACE || o->action==STALE || o->action==CONTENTS || (o->action==RESIZE && old!=o->length))e->mutations++;
        }
        if(!status && (o->action==UNLINK || o->action==RMDIR || o->action==MOVE || o->action==DMOVE)) {
            fs_info gone;if(!check(e,"retired-path",FS_MISSING,io_stat(source,&gone),0,0))goto done;}
        if(!sweep(e))goto done;
    }
    if(!close_opened(e) || !sweep(e))goto done;
    {uint64_t required=(1ULL<<ROOT)|(1ULL<<MKDIR)|(1ULL<<CREATE)|(1ULL<<WRITE)|(1ULL<<READ)|(1ULL<<REOPEN)|(1ULL<<TRUNCATE)|(1ULL<<MOVE)|(1ULL<<UNLINK)|(1ULL<<RMDIR)|(1ULL<<CASE)|(1ULL<<DMOVE);
     uint32_t required_ranges=4095U & ((p->caps&FS_GAPS)?4095U:~64U);
     if(p->caps&FS_RESIZE){required|=1ULL<<RESIZE;required_ranges|=4096;}
     if(p->caps&FS_SAFE)required|=(1ULL<<SNAPSHOT)|(1ULL<<REPLACE)|(1ULL<<STALE)|(1ULL<<CONTENTS);
     if((e->witness&required)!=required || (e->ranges&required_ranges)!=required_ranges || p->count<64 || e->mutations<12){printf("FSINCOMPLETE kind=coverage witnesses=%llu required=%llu mutations=%u ranges=%u required_ranges=%u\n",(unsigned long long)e->witness,(unsigned long long)required,e->mutations,e->ranges,required_ranges);result=2;goto done;}}
    {uint64_t finished=io_milliseconds();
     if(finished==~0ULL || finished<e->start || finished-e->start>180000){puts("FSINCOMPLETE kind=deadline");result=2;goto done;}
     result=printf("FSPASS ops=%u checks=%llu mutations=%u witnesses=%llu ranges=%u elapsed_ms=%llu\n",p->count,(unsigned long long)e->checks,e->mutations,(unsigned long long)e->witness,e->ranges,(unsigned long long)(finished-e->start))<0?2:0;}
 done:
    if(e->resource){puts(e->resource==2?"FSINCOMPLETE kind=clock":"FSINCOMPLETE kind=resource");result=2;}
    if(e->uncertain){puts("FSUNCERTAIN retained-by-filesystem=required");result=44;}
    if(result==43)printf("FSFAILED first=%u checks=%llu prefix_ops=%u\n",e->sequence,(unsigned long long)e->checks,e->sequence);
    /* Close is lifetime cleanup only. Never unlink, retry or repair on failure. */
    if(e->handle>=0){int closed=io_close(e->handle);if(!closed)e->handle=-1;else if(!result)result=43;}
    free(e);return result;
}
