#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Acceptance-runner evidence rejection, not a substitute for guest execution."""
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
sys.dont_write_bytecode = True
import guest_mgfs_test as guest

run='0123456789abcdef'
for phase,count in [('create',42),('verify',39)]:
    rows=guest.expected_operations(phase,run,221)
    assert len(rows)==count
    text=f'GUESTFS1 phase={phase} run={run} bytes=8193\n'
    text+=''.join(f'OP {i} {name} expected={value} observed={value}\n'
                  for i,(name,value) in enumerate(rows,1))
    text+=f'GUESTFS1 complete phase={phase} run={run} ops={count} identifier=221\n'
    guest.validate_transcript(text,phase,run,221)
    for wrong in ['',text.split('GUESTFS1 complete')[0],text.replace('observed=8193','observed=8192',1),
                  text.replace('seek-start','not-a-seek',1),text.replace('expected=8193 observed=8193','expected=8192 observed=8192',1),
                  text.replace('identifier=221','identifier=222'),text+'PASS\n']:
        try: guest.validate_transcript(wrong,phase,run,221)
        except ValueError: pass
        else: raise AssertionError('invalid transcript qualified')

# Process/QMP dependencies below are controlled only to test infrastructure
# failure handling. No filesystem or successful guest behavior is simulated.
class Process:
    def __init__(self,*unused,**kw):
        self.stdin=io.BytesIO(); self.stdout=io.BytesIO(); self.returncode=None
    def poll(self): return self.returncode
    def wait(self,**kw): self.returncode=0; return 0
    def kill(self): self.returncode=-9
class QMP:
    def __init__(self,process): self.process=process; self.events=[]
    def call(self,name,args=None):
        if name=='query-status': return {'status':'running'}
        if name=='quit': self.process.returncode=0
    def close(self): pass

with tempfile.TemporaryDirectory(prefix='mangrove-guest-evidence-') as temp:
    folder=Path(temp); firmware=folder/'vars'; firmware.write_bytes(b'fixture')
    args=SimpleNamespace(qemu=str(folder/'missing-qemu'),accel='tcg',ovmf_vars=firmware,
                         ovmf_code=firmware,timeout=30)
    try: guest.boot(folder,folder/'disk','missing',args)
    except FileNotFoundError: pass
    else: raise AssertionError('missing launcher qualified')
    ticks=iter([0,0,31])
    with patch.object(guest.subprocess,'Popen',Process), patch.object(guest,'QMP',QMP), \
         patch.object(guest,'screen_text',return_value=''),patch.object(guest.time,'sleep'), \
         patch.object(guest.time,'monotonic',side_effect=lambda:next(ticks)):
        try: guest.boot(folder,folder/'disk','timeout',args)
        except TimeoutError: pass
        else: raise AssertionError('timeout qualified')
    with patch.object(guest.subprocess,'Popen',Process),patch.object(guest,'QMP',QMP), \
         patch.object(guest,'screen_text',side_effect=['developer ~:',
             'developer ~: /bin/shoot /temp/unexpected-child-exit.shoot',
             'Could not run "fstest": exited with status 2.']), \
         patch.object(guest.time,'sleep'):
        try: guest.boot(folder,folder/'disk','unexpected-child-exit',args)
        except RuntimeError as error: assert 'native launch, exit or wait failure' in str(error)
        else: raise AssertionError('unexpected child exit qualified')
    class Finished(Process):
        def __init__(self,*a,**kw): super().__init__(*a,**kw); self.returncode=0
    with patch.object(guest.subprocess,'Popen',Finished):
        try: guest.boot(folder,folder/'disk','no-execution',args)
        except RuntimeError as error: assert 'no native launch evidence' in str(error)
        else: raise AssertionError('empty execution qualified')
print('Guest evidence: missing launch, timeout, empty execution, missing completion and wrong operations/counts rejected')
