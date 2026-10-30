#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production completion collector; scheduler/notification dispatch is modeled."""
import ast
import os
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/shared'))
from host_fixture import HostFixture
CC = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
p = ROOT / 'tests/kernel/process_reap_test.py'
tree = ast.parse(p.read_text())
tree.body = [node for node in tree.body if not isinstance(node, ast.With)]
ns = {'__file__':str(p), '__name__':'definitions_only'}
exec(compile(tree, str(p), 'exec'), ns)
code = ns['code'].split('int main(void)')[0]
code = code.replace('static void object_release(kernel_object_t *object)',
    'static void process_object_destroy(kernel_object_t *);\nstatic void object_release(kernel_object_t *object)')
code = code.replace('if(object==&child.object && !object->ref_count)all_processes=NULL;',
    'if(object==&child.object && !object->ref_count)process_object_destroy(object);')
code += ns['function'](ns['source'], 'process_cleanup_worker_retry_session_collections_once')
code += r'''
static void worker_collection_event(void) {
 assert(process_cleanup_fixture_work & PROCESS_CLEANUP_COLLECTION);
 process_cleanup_fixture_work=0;
 process_session_collection_retry_pending=true;
 assert(process_cleanup_worker_retry_session_collections_once());
 assert(!process_session_collection_retry_pending);
}
static void orphan(void) {
 child.parent=NULL;parent.first_child=NULL;child.session_id=0;
}
static void exact_cleanup(void) {
 assert(!child.owner_reference_held && !child.object.ref_count && !all_processes);
 assert(child.thread_count==0 && thread_reaps==1 && exit_notifications==1);
 assert(destroyed_roots==1 && released_memory==1 && user_region_metadata_releases==1);
 assert(owner_releases==1);
 for(unsigned i=0;i<3;i++) {
  process_collect_completed(0,true);
  assert(destroyed_roots==1 && owner_releases==1);
 }
}
int main(void) {
 /* Same terminal no-session orphan as Sol's frozen counterexample. */
 reset(false);orphan();process_cleanup_request(PROCESS_CLEANUP_COLLECTION);
 worker_collection_event();exact_cleanup();
 /* Terminal child of an ACTIVE parent still has legitimate wait ownership.
  * Parent EXITING is the irreversible abandonment boundary. */
 reset(false);child.session_id=0;
 process_cleanup_request(PROCESS_CLEANUP_COLLECTION);worker_collection_event();
 assert(child.owner_reference_held && child.object.ref_count==2 && !destroyed_roots);
 parent.state=PROCESS_STATE_EXITING;process_cleanup_request(PROCESS_CLEANUP_COLLECTION);
 worker_collection_event();exact_cleanup();
 /* An open child handle or a delayed operation pin keeps the exact-count
  * gate closed. Dropping it publishes a retry, including session zero. */
 reset(false);orphan();child.object.ref_count++;
 process_cleanup_request(PROCESS_CLEANUP_COLLECTION);worker_collection_event();
 assert(child.thread_count==1 && child.owner_reference_held);
 process_handle_reference_release(&child.object);worker_collection_event();exact_cleanup();
 /* An admitted wait claim cannot be stolen even after parent EXITING. */
 reset(false);orphan();child.wait_claimed=true;
 process_cleanup_request(PROCESS_CLEANUP_COLLECTION);worker_collection_event();
 assert(child.owner_reference_held && child.thread_count==1);
 child.wait_claimed=false;process_cleanup_request(PROCESS_CLEANUP_COLLECTION);
 worker_collection_event();exact_cleanup();
 /* Temporary pin overlaps final thread removal, then zero-thread retry. */
 reset(false);orphan();assert(process_finalize_exit(&child));child.object.ref_count++;
 process_cleanup_request(PROCESS_CLEANUP_COLLECTION);worker_collection_event();
 assert(child.thread_count==0 && child.object.ref_count==2 && child.owner_reference_held);
 process_handle_reference_release(&child.object);worker_collection_event();exact_cleanup();
 /* ACTIVE/EXITING, readers, active admissions and cleanup claims never
  * become prematurely collectible. Resolve each real dependency and retry. */
 for(unsigned mode=0;mode<5;mode++) {
  reset(false);orphan();
  if(mode==0)child.state=PROCESS_STATE_EXITING;
  if(mode==1)child.thread_list_readers=1;
  if(mode==2)child.active_syscalls=1;
  if(mode==3)child.worker_reapers=1;
  if(mode==4)child.exit_cleanup_complete=false;
  process_cleanup_request(PROCESS_CLEANUP_COLLECTION);worker_collection_event();
  assert(child.owner_reference_held && !destroyed_roots);
  child.state=PROCESS_STATE_TERMINATED;child.thread_list_readers=0;
  child.active_syscalls=0;child.worker_reapers=0;child.exit_cleanup_complete=true;
  process_cleanup_request(PROCESS_CLEANUP_COLLECTION);worker_collection_event();exact_cleanup();
 }
 puts("orphan collection: explicit abandonment, handle/pin retries, wait claims, zero-thread retry and exact owner/root destruction PASS");
}
'''
code = '#include "' + str(ROOT/'tests/kernel/process_cleanup_fixture.h') + '"\n' + code
with HostFixture('orphan-collection') as fixture:
    source = fixture.path/'orphan.c'; source.write_text(code)
    binary = fixture.path/'orphan'
    fixture.run([CC,'-std=gnu17','-O1','-g','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-fsanitize=address,undefined',str(source),'-o',str(binary)])
    print(fixture.run([str(binary)], env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}).stdout,end='')
