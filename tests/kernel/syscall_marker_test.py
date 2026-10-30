#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native assembly/C nesting protects blockable syscall continuations."""
import ast
from pathlib import Path
import os
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests/shared'))
from host_fixture import HostFixture
CC = sys.argv[1] if len(sys.argv)>1 else 'gcc'
p=ROOT/'tests/kernel/termination_lifecycle_test.py'
t=ast.parse(p.read_text())
t.body=[n for n in t.body if not (isinstance(n,ast.Expr) and isinstance(n.value,ast.Call) and isinstance(n.value.func,ast.Name) and n.value.func.id=='run')]
ns={'__file__':str(p),'__name__':'definitions_only'}
exec(compile(t,str(p),'exec'),ns)
fn=ns['function']
code=ns['scheduler_prefix']+ns['scheduler_functions']+ns['scheduler_suffix'].split('int main(void)')[0]
# Replace the modeled group preflight with the actual scheduler implementation.
start=code.rindex('static bool scheduler_terminate_thread_group(kernel_thread_t *head,\n')
end=code.index('static void process_cleanup_exit_resources',start)
code=code[:start]+fn('kernel/src/scheduler.c','scheduler_terminate_thread_group')+code[end:]
code+='static bool host_if=true;\nstatic bool scheduler_interrupts_enabled(void){return host_if;}\n'
for name in ('scheduler_syscall_enter','scheduler_syscall_leave'):
    code+=fn('kernel/src/scheduler.c',name)+'\n'
entry=(ROOT/'kernel/src/syscall_entry.s').read_text()
assert entry.index('call scheduler_syscall_enter') < entry.index('call syscall_dispatch') < entry.index('call scheduler_syscall_leave')
code+=r'''
int main(void){process_t p={.state=PROCESS_STATE_ACTIVE};kernel_thread_t w={.state=THREAD_STATE_READY,.running_cpu=THREAD_CPU_NONE};target_object.process=&p;target_object.references=1;p.object.references=1;assert(process_attach_thread(&p,&w));p.launch_ready=true;
 current_thread=&w;scheduler_syscall_enter();scheduler_syscall_enter();scheduler_syscall_leave();
 assert(w.syscall_nesting==1&&w.syscall_active);
 current_thread=&selected_thread;w.state=THREAD_STATE_BLOCKED;w.entry_started=true;w.waiting_mutex=(void*)1;
 assert(!process_terminate_external(&p,-1));assert(w.state==THREAD_STATE_BLOCKED&&p.state==PROCESS_STATE_ACTIVE);
 current_thread=&w;host_if=false;scheduler_syscall_leave();assert(!w.syscall_active&&!w.syscall_nesting);
 puts("native outer syscall nesting survives C epilogue; production group preflight preserves blocked continuation PASS");return 0;}
'''
with HostFixture('syscall-marker') as fixture:
    source=fixture.path/'marker.c';binary=fixture.path/'marker'
    source.write_text('#include "'+str(ROOT/'tests/kernel/process_cleanup_fixture.h')+'"\n'+code)
    fixture.run([CC,'-std=gnu17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined',str(source),'-o',str(binary)])
    print(fixture.run([str(binary)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}).stdout,end='')
