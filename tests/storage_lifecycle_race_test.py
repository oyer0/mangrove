#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic production-path assertions for three old lifecycle races.
Optional repository path and --expect-failure verify the committed baseline.
IRQ/gate/notification decisions are controlled fixtures, not hardware tests.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
expect_failure = '--expect-failure' in sys.argv

def function(path, name):
    s = (ROOT / path).read_text()
    for m in re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(', s, re.M):
        b = s.index('{', m.end())
        if ';' in s[m.end():b]:continue
        end, depth = b + 1, 1
        while depth:
            depth += (s[end] == '{') - (s[end] == '}');end += 1
        return s[m.start():end]+'\n'
    raise ValueError(name)

common=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;typedef unsigned u32;typedef unsigned spinlock_t;
#define BLOCK_MAX_DEVICES 32
#define BLOCK_DEVICE_ID_BASE 0
#define MG_EVENT_CLASS_DEVICE 1
#define MG_EVENT_CLASS_BLOCK 2
#define MG_EVENT_BLOCK_REMOVED 3
static bool gate_available=true;
/* No recovery owners in this isolated failed-quiescence fixture. */
static bool vfs_registration_retained(u64 id){(void)id;return false;}
static bool block_io_begin_quiesce(void){return gate_available;}
static void block_io_end_quiesce(void){}
static u64 spin_lock_irqsave(spinlock_t*p){(void)p;return 0;}
static void spin_unlock_irqrestore(spinlock_t*p,u64 f){(void)p;(void)f;}
'''
unregister=common+r'''
typedef struct {u64 id;unsigned type;bool registered,online;} block_device_t;
static block_device_t devices[32];static unsigned device_count=1;
static spinlock_t block_registry_lock;static bool retirement_notifying[32];
static u32 block_find_slot_by_id(u64 id){(void)id;return 0;}
static const char *block_type_name(unsigned t){(void)t;return "USB";}
static bool block_device_mark_gone(u64 id){(void)id;devices[0].online=false;return true;}
static void block_cache_invalidate_all(void){}
static void ipc_publish_event(unsigned c,unsigned e,u64 id,const char*n){(void)c;(void)e;(void)id;(void)n;}
'''+function('kernel/src/block.c','block_unregister_by_id')+r'''
int main(void){devices[0].registered=devices[0].online=true;gate_available=false;
assert(!block_unregister_by_id(0));assert(devices[0].registered && !devices[0].online);}
'''
gpt=common+r'''
typedef struct {u64 id;unsigned type;void *driver_data;} block_device_t;
typedef struct {block_device_t *parent;} gpt_partition_info_t;
#define BLOCK_DEVICE_PARTITION 1
static gpt_partition_info_t partitions[1];static unsigned partition_count=1;
static spinlock_t partition_state_lock;static bool partition_registered[1]={true};static u64 partition_ids[1];
static void gpt_mark_partitions_gone_for_parent(block_device_t*p){(void)p;}
static bool gpt_partition_parent_matches(const gpt_partition_info_t*p,const block_device_t*d){return p->parent==d;}
static bool block_unregister_by_id(u64 id){(void)id;return false;}
static bool block_device_mark_gone(u64 id){(void)id;return true;}
static unsigned enumerations;
static block_device_t child={.type=BLOCK_DEVICE_PARTITION};
static unsigned block_device_count(void){return 1;}
static block_device_t*block_get_device(unsigned n){(void)n;return enumerations++==0?&child:NULL;}
static bool gpt_get_partition_info(block_device_t*d,gpt_partition_info_t*out){(void)d;*out=partitions[0];return true;}
'''+function('kernel/src/storage/gpt.c','gpt_remove_partitions_for_parent')+r'''
int main(void){block_device_t parent={0};partitions[0].parent=&parent;
(void)gpt_remove_partitions_for_parent(&parent);assert(partitions[0].parent==&parent);}
'''
begin=function('kernel/src/storage/format.c','storage_mutation_begin')
# Inject another CPU exactly between the old check and assignment. For the
# corrected CAS, inject immediately after successful reservation instead.
begin=begin.replace('    storage_operation_in_progress = true;', '    inject_other_cpu();\n    storage_operation_in_progress = true;')
mutation=common+r'''
static bool storage_operation_in_progress,inject=true,other_entered;
static bool storage_mutation_begin(void);
static void inject_other_cpu(void){if(inject){inject=false;other_entered=storage_mutation_begin();}}
static bool compare(bool*p,bool*expected,bool desired,bool weak,int success,int failure){(void)weak;(void)success;(void)failure;
bool result=__atomic_compare_exchange_n(p,expected,desired,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
if(result)inject_other_cpu();return result;}
#define __atomic_compare_exchange_n(p,e,d,w,s,f) compare(p,e,d,w,s,f)
'''+begin+r'''
int main(void){assert(storage_mutation_begin());assert(!other_entered);}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-lifecycle-races-') as d:
    failures=0
    for name,source in [('unregister-drain',unregister),('gpt-retained-backing',gpt),('mutation-check-set',mutation)]:
        c=Path(d)/(name+'.c');c.write_text(source);exe=c.with_suffix('')
        # Fixtures differ across revisions; unused fixture symbols are allowed.
        subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
        failed=result.returncode!=0
        if failed:failures+=1
        print(name+': '+('baseline safety assertion failed as expected' if failed and expect_failure else 'FAILED' if failed else 'passed'))
        if failed and not expect_failure:print(result.stderr)
    if (expect_failure and failures!=3) or (not expect_failure and failures):raise SystemExit(1)
