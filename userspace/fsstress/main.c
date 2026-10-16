/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fsstress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int number(const char *s,uint64_t *n,unsigned base)
{
    *n=0;if(!*s)return 0;
    for(;*s;s++){unsigned d=*s>='0' && *s<='9'?(unsigned)(*s-'0'):*s>='a' && *s<='f'?(unsigned)(*s-'a'+10):99;
        if(d>=base || *n>(~0ULL-d)/base)return 0;*n=*n*base+d;}
    return 1;
}
static int root_valid(const char *s)
{
    if(*s!='/' || strlen(s)>160 || strlen(s)<4 || s[strlen(s)-1]=='/')return 0;
    for(const char *p=s;*p;p++)if(!((*p>='a' && *p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='/'||*p=='-'||*p=='_'))return 0;
    for(const char *p=s;*p;p++)if(p[0]=='/' && p[1]=='/')return 0;
    return 1;
}
int fs_main(int argc,char **argv)
{
    fs_plan *p=calloc(1,sizeof(*p));unsigned char *wire=malloc(FS_TRACE_BYTES);
    if(!p || !wire){free(p);free(wire);puts("FSINCOMPLETE kind=memory");return 2;}
    int result=2;const char *root=NULL,*trace=NULL;
    if(argc>=2 && (!strcmp(argv[1],"run") || !strcmp(argv[1],"plan"))) {
        int plan=!strcmp(argv[1],"plan");uint64_t seed,count;uint32_t profile=4,control=0;
        if((plan?argc!=6:argc!=7 && argc!=8))goto usage;
        static const char *profiles[]={"mgfs","fat32","exfat","posix"};
        for(uint32_t i=0;i<4;i++)if(!strcmp(argv[2],profiles[i]))profile=i;
        if(!plan && ((io_native() && profile==3) || (!io_native() && profile!=3)))goto usage;
        if(!number(argv[3],&seed,16) || !number(argv[4],&count,10) || count<64 || count>FS_OPS)goto usage;
        if(!plan && argc==8){control=!strcmp(argv[7],"model")?1:!strcmp(argv[7],"status")?2:99;}
        if(!fs_generate(p,profile,seed,(uint32_t)count,control))goto usage;
        trace=argv[plan?5:6];root=plan?NULL:argv[5];
        if(root && !root_valid(root))goto usage;
        size_t bytes=fs_encode(p,wire);
        int status=io_trace_save(trace,wire,(uint32_t)bytes);
        if(status){printf("FSINCOMPLETE kind=trace-save observed=%d\n",status);goto done;}
        if(plan){printf("FSPLAN seed=%016llx ops=%u bytes=%u\n",(unsigned long long)seed,p->count,(unsigned)bytes);result=0;goto done;}
    } else if(argc==4 && (!strcmp(argv[1],"replay") || !strcmp(argv[1],"verify"))) {
        uint32_t bytes=0;root=argv[2];trace=argv[3];
        if(!root_valid(root))goto usage;
        int status=io_trace_load(trace,wire,FS_TRACE_BYTES,&bytes);
        if(status || !fs_decode(p,wire,bytes)){printf("FSINCOMPLETE kind=trace-load observed=%d\n",status);goto done;}
        if((io_native() && p->profile==3) || (!io_native() && p->profile!=3)){puts("FSINCOMPLETE kind=platform-capabilities");goto done;}
    } else goto usage;
    result=fs_execute(p,root,!strcmp(argv[1],"verify"));goto done;
 usage:
    puts("usage: fsstress run PROFILE HEXSEED OPERATIONS ROOT TRACE [model|status]");
    puts("       fsstress replay|verify ROOT TRACE; fsstress plan PROFILE HEXSEED OPERATIONS TRACE");
    puts("limits: 64..1024 operations, 8 files, 4 directories, 65536 bytes/file; native profiles mgfs/fat32/exfat; host posix");
 done:free(wire);free(p);return result;
}
int main(int argc,char **argv){return fs_main(argc,argv);}
