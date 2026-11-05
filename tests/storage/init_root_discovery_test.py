#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production root publication predicate and bounded discovery admission."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'kernel/src/init.c').read_text()
def function(name):
    m=re.search(r'^static (?:bool|init_result_t) '+name+r'\(',s,re.M)
    a=s.index('{',m.end());b=a+1;depth=1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[m.start():b]+'\n'
fixture=r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <init.h>
#define x250_boot_trace_stage(...) ((void)0)
#define x250_boot_trace_result(...) ((void)0)
#define xhci_trace_boot_discovery(...) ((void)0)
#define GPT_MANGROVE_ROOT_NAME "MANGROVE_ROOT"
#define XHCI_SUCCESS 0
#define XHCI_ERR_TIMEOUT 2
#define XHCI_ERR_CONTROLLER_BAD 6
typedef int xhci_status_t;
typedef struct {bool partition;char name[32];} block_device_t;
typedef struct {char name[32];} gpt_partition_info_t;
static block_device_t devices[3];
static unsigned device_count,waits,mode;
static void *g_xhc=(void*)1;
static init_state_t state=INIT_READY;
static unsigned block_device_count(void){return device_count;}
static block_device_t *block_get_device(unsigned i){assert(i<device_count);return &devices[i];}
static bool gpt_get_partition_info(block_device_t*d,gpt_partition_info_t*i){
 if(!d->partition)return false;
 strcpy(i->name,d->name);return true;
}
init_state_t kernel_init_state(init_id_t id){assert(id==INIT_XHCI);return state;}
static xhci_status_t xhci_wait_for_boot_discovery(void*x,bool(*ready)(void*),void*context){
 assert(x==g_xhc&&context==NULL);waits++;
 if(mode==1)return XHCI_ERR_TIMEOUT;
 if(mode==2)return XHCI_ERR_CONTROLLER_BAD;
 if(mode==3){assert(ready(context));return XHCI_SUCCESS;}
 assert(!ready(context));
 /* A whole disk and an unrelated partition must not end root discovery. */
 device_count=2;devices[1].partition=true;strcpy(devices[1].name,"OTHER_ROOT");
 assert(!ready(context));
 device_count=3;devices[2].partition=true;strcpy(devices[2].name,GPT_MANGROVE_ROOT_NAME);
 assert(ready(context));return XHCI_SUCCESS;
}
'''
checks=r'''
int main(void){
 const char *reason=NULL;device_count=1;
 assert(init_await_boot_discovery(true,&reason)==INIT_RESULT_OK&&waits==1&&device_count==3);
 mode=3;assert(init_await_boot_discovery(true,&reason)==INIT_RESULT_OK&&waits==2); /* fence even for known role */
 memset(devices,0,sizeof(devices));device_count=1;mode=1;
 assert(init_await_boot_discovery(true,&reason)==INIT_RESULT_FAILED);
 assert(strstr(reason,"USB discovery deadline"));
 mode=2;assert(init_await_boot_discovery(true,&reason)==INIT_RESULT_FAILED);
 assert(strcmp(reason,"native USB root discovery failed")==0);
 /* Empty storage admission awaits any block; role matching remains separate. */
 mode=1;device_count=0;
 assert(init_await_boot_discovery(false,&reason)==INIT_RESULT_UNAVAILABLE);
 assert(strcmp(reason,"no persistent block device (USB discovery deadline)")==0);
 mode=2;assert(init_await_boot_discovery(false,&reason)==INIT_RESULT_FAILED);
 assert(strcmp(reason,"native USB storage discovery failed")==0);
 mode=3;device_count=1;assert(init_await_boot_discovery(false,&reason)==INIT_RESULT_OK);
 assert(!init_root_role_registered(NULL));
 unsigned before=waits;g_xhc=NULL;
 assert(init_await_boot_discovery(true,&reason)==INIT_RESULT_OK&&waits==before);
 g_xhc=(void*)1;state=INIT_FAILED;
 assert(init_await_boot_discovery(true,&reason)==INIT_RESULT_OK&&waits==before);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-init-root-discovery-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test'
    c.write_text(fixture+function('init_root_role_registered')+function('init_block_registered')+function('init_await_boot_discovery')+checks)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iinclude','-Ikernel/include',str(c),'-o',str(binary)],cwd=ROOT,check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: root role publication, unrelated disk/partition rejection, completion fence, deadline and controller failure')
