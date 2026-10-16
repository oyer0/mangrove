/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent model/plan: never reads an adapter result or on-disk metadata. */
#include "fsstress.h"
#include <string.h>
#include <stdio.h>
uint64_t fs_random(uint64_t *state)
{
    uint64_t x=(*state+=0x9e3779b97f4a7c15ULL);
    x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL;
    return x^(x>>31);
}
uint32_t fs_pick(uint64_t *state,uint32_t bound)
{
    uint64_t threshold=(0ULL-bound)%bound,value;
    do {value=fs_random(state);} while(value<threshold);
    return (uint32_t)(value%bound);
}
void fs_payload(uint64_t seed,unsigned char *out,uint32_t count)
{for(uint32_t i=0;i<count;i++)out[i]=(unsigned char)fs_random(&seed);}
uint32_t fs_caps(uint32_t profile)
{
    static const uint32_t caps[]={FS_SAFE,FS_SAFE|FS_GAPS|FS_ASCII_CI,
        FS_SAFE|FS_GAPS|FS_ASCII_CI,FS_GAPS|FS_RESIZE};
    return profile<4?caps[profile]:0;
}
static int parent_live(const fs_shape *s,uint32_t parent)
{return parent==FS_DIRS?s->root:parent<FS_DIRS && s->dirs[parent].live;}
int fs_predict(const fs_shape *s,const fs_op *o,uint32_t caps)
{
    if(o->action>=FS_ACTIONS || o->id>=FS_FILES || o->arg>FS_DIRS ||
       o->length>FS_BYTES || o->offset>FS_BYTES)return -100;
    const fs_file *f=&s->files[o->id];
    switch(o->action) {
    case ROOT:return s->root?FS_EXISTS:FS_OK;
    case MKDIR:
        if(o->id>=FS_DIRS || !parent_live(s,o->arg))return -100;
        if(s->dirs[o->id].live && s->dirs[o->id].parent!=o->arg)return -100;
        return s->dirs[o->id].live?FS_EXISTS:FS_OK;
    case CREATE:case CONTENTS:
        if(!parent_live(s,o->arg))return -100;
        if(o->action==CONTENTS && !(caps&FS_SAFE))return FS_UNSUPPORTED;
        if(f->live && (f->parent!=o->arg || f->variant))return -100;
        return f->live?FS_EXISTS:FS_OK;
    case WRITE:
        if(!f->live || o->offset>FS_BYTES-o->length ||
           (o->length && o->offset>f->size && !(caps&FS_GAPS)))return -100;
        return FS_OK;
    case CASE:return !f->live?-100:(caps&FS_ASCII_CI)?FS_OK:FS_MISSING;
    case READ:case REOPEN:case TRUNCATE:case UNLINK:
        return f->live?FS_OK:-100;
    case SNAPSHOT:case REPLACE:case STALE:
        if(!f->live)return -100;
        return !(caps&FS_SAFE)?FS_UNSUPPORTED:o->action==STALE?FS_STALE:FS_OK;
    case RESIZE:return !f->live?-100:!o->length || (caps&FS_RESIZE)?FS_OK:FS_UNSUPPORTED;
    case MOVE:return f->live && parent_live(s,o->arg)?FS_OK:-100;
    case DMOVE: {
        if(o->id>=FS_DIRS || !s->dirs[o->id].live || !parent_live(s,o->arg))return -100;
        uint32_t parent=o->arg;
        for(uint32_t depth=0;parent!=FS_DIRS;depth++) {
            if(parent==o->id || parent>=FS_DIRS || depth==FS_DIRS)return -100;
            parent=s->dirs[parent].parent;
        }
        return s->dirs[o->id].parent==o->arg?-100:FS_OK;}
    case RMDIR:
        if(o->id>=FS_DIRS)return -100;
        if(!s->dirs[o->id].live)return -100;
        for(uint32_t i=0;i<FS_FILES;i++)if(s->files[i].live && s->files[i].parent==o->id)return FS_NOTEMPTY;
        for(uint32_t i=0;i<FS_DIRS;i++)if(s->dirs[i].live && s->dirs[i].parent==o->id)return FS_NOTEMPTY;
        return FS_OK;
    default:return -100;
    }
}
void fs_transition(fs_shape *s,const fs_op *o,int status)
{
    fs_file *f=&s->files[o->id];
    if(status && status!=FS_STALE)return;
    switch(o->action) {
    case ROOT:s->root=1;break;
    case MKDIR:s->dirs[o->id]=(fs_dir){1,o->arg};break;
    case CREATE:case CONTENTS:*f=(fs_file){1,o->arg,0,o->action==CONTENTS?o->length:0};break;
    case WRITE:if(o->length && o->offset+o->length>f->size)f->size=o->offset+o->length;break;
    case TRUNCATE:f->size=0;break;
    case RESIZE:case REPLACE:case STALE:f->size=o->length;break;
    case MOVE:f->parent=o->arg;f->variant^=1;break;
    case DMOVE:s->dirs[o->id].parent=o->arg;break;
    case UNLINK:f->live=0;break;
    case RMDIR:s->dirs[o->id].live=0;break;
    default:break;
    }
}
int fs_path(const fs_shape *s,uint32_t directory,uint32_t id,uint32_t variant,char *out,size_t cap)
{
    uint32_t chain[FS_DIRS],count=0,p=directory;
    while(p!=FS_DIRS) {
        if(p>=FS_DIRS || count==FS_DIRS || !s->dirs[p].live)return 0;
        chain[count++]=p;p=s->dirs[p].parent;
    }
    size_t at=0;out[0]=0;
    while(count) {
        int n=snprintf(out+at,cap-at,"/directory-%u",chain[--count]);
        if(n<0 || (size_t)n>=cap-at)return 0;at+=(size_t)n;
    }
    if(id<FS_FILES) {
        int n=snprintf(out+at,cap-at,"/%s-entry-%u.bin",variant?"moved":"ordinary",id);
        if(n<0 || (size_t)n>=cap-at)return 0;
    }
    return 1;
}
static int add(fs_plan *p,fs_shape *s,uint32_t action,uint32_t id,uint32_t arg,uint32_t at,uint32_t len,uint64_t data)
{
    fs_op o={action,id,arg,at,len,0,data};o.expected=fs_predict(s,&o,p->caps);
    if(o.expected==-100 || p->count>=FS_OPS)return 0;
    p->ops[p->count++]=o;fs_transition(s,&o,o.expected);return 1;
}
int fs_generate(fs_plan *p,uint32_t profile,uint64_t seed,uint32_t count,uint32_t control)
{
    if(profile>3 || count<64 || count>FS_OPS || control>2)return 0;
    memset(p,0,sizeof(*p));p->seed=seed;p->profile=profile;p->caps=fs_caps(profile);p->control=control;
    fs_shape s={0};uint64_t rng=seed;
#define A(op,id,arg,at,len) do{if(!add(p,&s,op,id,arg,at,len,fs_random(&rng)))return 0;}while(0)
    A(ROOT,0,FS_DIRS,0,0);A(MKDIR,0,FS_DIRS,0,0);A(MKDIR,1,FS_DIRS,0,0);A(MKDIR,2,0,0,0);
    A(CREATE,0,0,0,0);A(WRITE,0,0,0,513);A(WRITE,0,0,512,513);
    A(WRITE,0,0,1,257);A(READ,0,0,511,513);A(WRITE,0,0,1025,1);
    A(WRITE,0,0,1,1);A(WRITE,0,0,2,1);
    A(READ,0,0,1026,1);A(REOPEN,0,0,0,0);A(WRITE,0,0,1026,0);
    A(TRUNCATE,0,0,0,0);A(WRITE,0,0,0,4097);A(WRITE,0,0,4097,513);
    A(MOVE,0,1,0,0);A(MOVE,0,1,0,0);A(CASE,0,0,0,0);
    A(SNAPSHOT,0,0,0,0);A(REPLACE,0,0,0,512);A(STALE,0,0,0,513);
    A(CONTENTS,3,1,0,8193);A(CONTENTS,3,1,0,1);A(CREATE,1,0,0,0);A(CREATE,1,0,0,0);
    A(WRITE,1,0,0,FS_BYTES);A(RMDIR,0,0,0,0);A(UNLINK,1,0,0,0);
    A(CREATE,1,0,0,0);A(WRITE,1,0,0,511);
    if(p->caps&FS_GAPS)A(WRITE,1,0,1025,1);
    A(RESIZE,1,0,0,4097);A(READ,1,0,511,2);A(UNLINK,1,0,0,0);
    A(CREATE,2,2,0,0);A(WRITE,2,0,0,8193);A(DMOVE,2,1,0,0);
    A(REOPEN,2,0,0,0);A(UNLINK,2,0,0,0);A(RMDIR,2,0,0,0);A(RMDIR,0,0,0,0);A(MKDIR,0,FS_DIRS,0,0);
    while(p->count<count) {
        uint32_t id=fs_pick(&rng,FS_FILES);
        uint32_t kind=fs_pick(&rng,16);
        /* Root and directories 0/1 stay available as parents. Directories 2/3
         * have randomized create/remove/move lifetimes, including descendants
         * and live file handles. Selection depends only on the model graph. */
        uint32_t parents[FS_DIRS+1],parents_count=0;
        for(uint32_t d=0;d<=FS_DIRS;d++)if(parent_live(&s,d))parents[parents_count++]=d;
        uint32_t dir=parents[fs_pick(&rng,parents_count)];
        if(kind>=14) {
            uint32_t d=2+fs_pick(&rng,2);
            if(!s.dirs[d].live){A(MKDIR,d,dir,0,0);continue;}
            if(kind==14){A(RMDIR,d,0,0,0);continue;}
            uint32_t destinations[FS_DIRS+1],destinations_count=0;
            for(uint32_t at=0;at<parents_count;at++) {
                fs_op move={DMOVE,d,parents[at],0,0,0,0};
                if(fs_predict(&s,&move,p->caps)==FS_OK)destinations[destinations_count++]=parents[at];
            }
            if(!destinations_count)return 0;
            uint32_t destination=destinations[fs_pick(&rng,destinations_count)];
            A(DMOVE,d,destination,0,0);continue;
        }
        fs_file *f=&s.files[id];
        if(!f->live){A(CREATE,id,dir,0,0);continue;}
        uint32_t at=fs_pick(&rng,(p->caps&FS_GAPS)?FS_BYTES+1:f->size+1);
        if(fs_pick(&rng,2)) {static const uint32_t bounds[]={0,1,511,512,513,4095,4096,4097,8191,8192,8193,65535,65536};
            at=bounds[fs_pick(&rng,sizeof(bounds)/sizeof(bounds[0]))];
            if(!(p->caps&FS_GAPS) && at>f->size)at=f->size;}
        uint32_t len=fs_pick(&rng,FS_BYTES-at+1);
        if(fs_pick(&rng,2) && len>8193)len=fs_pick(&rng,8194);
        switch(kind) {
        case 0:case 1:case 2:case 3:A(WRITE,id,0,at,len);break;
        case 4:case 5:A(READ,id,0,at,len);break;
        case 6:A(TRUNCATE,id,0,0,0);break;
        case 7:A(MOVE,id,dir,0,0);break;
        case 8:A(UNLINK,id,0,0,0);break;
        case 9:A(REOPEN,id,0,0,0);break;
        case 10:A(SNAPSHOT,id,0,0,0);break;
        case 11:A(REPLACE,id,0,0,len);break;
        case 12:A(STALE,id,0,0,len);break;
        default:A(RESIZE,id,0,0,len);break;
        }
    }
#undef A
    return fs_validate(p);
}
int fs_validate(const fs_plan *p)
{
    if(p->profile>3 || p->caps!=fs_caps(p->profile) || !p->count || p->count>FS_OPS || p->control>2)return 0;
    fs_shape s={0};
    for(uint32_t i=0;i<p->count;i++) {
        const fs_op *o=&p->ops[i];int status=fs_predict(&s,o,p->caps);
        if(status==-100 || status!=o->expected)return 0;
        fs_transition(&s,o,status);
    }
    return 1;
}
static void put(unsigned char *p,uint64_t n,unsigned bytes)
{for(unsigned i=0;i<bytes;i++)p[i]=(unsigned char)(n>>(8*i));}
static uint64_t get(const unsigned char *p,unsigned bytes)
{uint64_t n=0;for(unsigned i=0;i<bytes;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
static uint64_t digest(const unsigned char *p,size_t n)
{uint64_t h=14695981039346656037ULL;for(size_t i=0;i<n;i++)h=(h^p[i])*1099511628211ULL;return h;}
size_t fs_encode(const fs_plan *p,unsigned char *out)
{
    size_t n=48+32*p->count;memset(out,0,n);memcpy(out,"MGFSX001",8);
    put(out+8,p->seed,8);put(out+16,p->profile,4);put(out+20,p->caps,4);put(out+24,p->count,4);put(out+28,p->control,4);
    for(uint32_t i=0;i<p->count;i++) {const fs_op *o=&p->ops[i];unsigned char *b=out+48+32*i;
        put(b,o->action,4);put(b+4,o->id,4);put(b+8,o->arg,4);put(b+12,o->offset,4);put(b+16,o->length,4);put(b+20,(uint32_t)o->expected,4);put(b+24,o->data,8);}
    put(out+32,digest(out,n),8);return n;
}
int fs_decode(fs_plan *p,const unsigned char *data,size_t length)
{
    if(length<48 || length>FS_TRACE_BYTES || memcmp(data,"MGFSX001",8) || get(data+40,8))return 0;
    uint32_t count=(uint32_t)get(data+24,4);if(!count || count>FS_OPS || length!=48+32*count)return 0;
    uint64_t h=14695981039346656037ULL;
    for(size_t i=0;i<length;i++)h=(h^(i>=32 && i<40?0:data[i]))*1099511628211ULL;
    if(h!=get(data+32,8))return 0;
    memset(p,0,sizeof(*p));p->seed=get(data+8,8);p->profile=(uint32_t)get(data+16,4);p->caps=(uint32_t)get(data+20,4);p->count=count;p->control=(uint32_t)get(data+28,4);
    for(uint32_t i=0;i<count;i++){fs_op *o=&p->ops[i];const unsigned char *b=data+48+32*i;
        o->action=(uint32_t)get(b,4);o->id=(uint32_t)get(b+4,4);o->arg=(uint32_t)get(b+8,4);o->offset=(uint32_t)get(b+12,4);o->length=(uint32_t)get(b+16,4);
        uint32_t status=(uint32_t)get(b+20,4);o->expected=status<=0x7fffffffU?(int32_t)status:-(int32_t)(0xffffffffU-status)-1;
        o->data=get(b+24,8);}
    return fs_validate(p);
}
