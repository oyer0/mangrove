#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production USB storage removal with injected lifecycle failures.
Gate and GPT/registry outcomes are fixtures; hardware teardown is tested apart.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
s = (ROOT/'kernel/src/usb/xhci_storage.c').read_text()
def function(name):
    m = re.search(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\([^;]*?\n\{',s,re.M)
    b=s.index('{',m.start());end=b+1;depth=1
    while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[m.start():end]+'\n'
pre=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef unsigned u32;typedef unsigned char u8;typedef uint64_t u64;
typedef struct {unsigned unused;} xhci_controller_t;
typedef struct {u64 id;} block_device_t;
typedef struct {xhci_controller_t*xhc;u8 slot_id;bool active,teardown_pending,teardown_gate_held,block_registered;block_device_t block;} usb_mass_storage_device_t;
static usb_mass_storage_device_t devices[4];static u32 device_count;
static unsigned gate_depth, marks;
static bool busy, fail_gpt, fail_unregister;
static void storage_mark_block_gone(usb_mass_storage_device_t*d){(void)d;marks++;}
static void gpt_mark_partitions_gone_for_parent(block_device_t*d){(void)d;}
static bool block_io_begin_quiesce(void){if(busy)return false;gate_depth++;return true;}
static void block_io_end_quiesce(void){assert(gate_depth);gate_depth--;}
static bool gpt_remove_partitions_for_parent(block_device_t*d){(void)d;return !fail_gpt;}
static bool block_unregister_by_id(u64 id){(void)id;return !fail_unregister;}
'''
post=r'''
static xhci_controller_t controller;
static void setup(void){assert(!gate_depth);memset(devices,0,sizeof(devices));devices[0]=(usb_mass_storage_device_t){.xhc=&controller,.slot_id=1,.active=true,.block_registered=true,.block={7}};device_count=1;busy=fail_gpt=fail_unregister=false;}
int main(void){
 setup();busy=true;assert(!xhci_storage_remove_device(&controller,1));assert(devices[0].xhc && devices[0].teardown_pending && device_count==1 && !gate_depth);
 busy=false;assert(xhci_storage_remove_device(&controller,1));assert(!devices[0].xhc && !device_count && !gate_depth);
 setup();assert(xhci_storage_prepare_teardown(&controller,1));assert(gate_depth==1);
 fail_gpt=true;assert(!xhci_storage_remove_device(&controller,1));assert(devices[0].xhc && devices[0].teardown_pending && device_count==1 && !gate_depth);
 assert(!devices[0].teardown_gate_held);fail_gpt=false;assert(xhci_storage_remove_device(&controller,1));assert(!gate_depth && !devices[0].xhc);
 setup();assert(xhci_storage_prepare_teardown(&controller,1));fail_unregister=true;
 assert(!xhci_storage_remove_device(&controller,1));assert(devices[0].xhc && devices[0].block_registered && device_count==1 && !gate_depth);
 fail_unregister=false;assert(xhci_storage_prepare_teardown(&controller,1));assert(xhci_storage_remove_device(&controller,1));assert(!device_count && !gate_depth);
 assert(!xhci_storage_remove_device(&controller,1));assert(!gate_depth);
 puts("USB teardown: busy gate, failed GPT/unregister, retained backing, balanced admission, retry and repeated removal passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-usb-teardown-') as d:
    c=Path(d)/'test.c';c.write_text(pre+function('xhci_storage_prepare_teardown')+function('xhci_storage_remove_device')+post);exe=c.with_suffix('')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
