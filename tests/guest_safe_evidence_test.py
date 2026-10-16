#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Evidence rejection controls, not simulated guest or filesystem qualification."""
from pathlib import Path
import sys
import tempfile
sys.dont_write_bytecode=True
import guest_safe_test as safe

run='0123456789abcdef'
assert safe.pattern(run,8,4)==bytes.fromhex('0422405e7c9ab8d6')
identities={s:100+i for i,s in enumerate(safe.SIZES)}
assert safe.fat_listing_names('::/work/REED.TXT\n::/work/safe-0123456789abcdef/\n')==['REED.TXT','safe-'+run]
assert safe.work_names('fat32',run)==['seed.txt','safe-'+run,'REED.TXT','COPIED.TXT','OCCUPIED.TXT']
controls=0
for backend in ('mgfs','fat32','exfat'):
    for phase in ('create','verify'):
        rows=safe.rows_safe(phase,backend,identities)
        body=''.join(f'SOP {i} {n} expected={v} observed={v}\n' for i,(n,v) in enumerate(rows,1))
        safe.check_rows(body,rows)
        failures=['',body.splitlines()[0]+'\n',body+'SOP malformed\n',body+body.splitlines()[0]+'\n',
                  body.replace('observed=0','observed=-12',1),
                  body.replace('expected=0 observed=0','expected=-12 observed=-12',1),
                  body.replace('snapshot-bytes','unrecognized-bytes',1),
                  body.replace('SOP 1 ','SOP 2 ',1)]
        for line in body.splitlines():
            if any(n in line for n in ('stale-refusal','owners-after','snapshot-identity','offset-preserved','ordinary-after-replace')):
                failures.append(body.replace(line,line.replace('observed=', 'observed=9'),1))
        for wrong in failures:
            try:safe.check_rows(wrong,rows)
            except ValueError:controls+=1
            else:raise AssertionError('incorrect operation qualified')
class QMP:
    def __init__(self):self.keys=[]
    def call(self,name,args):self.keys.append(args['command-line']);return ''
with tempfile.TemporaryDirectory(prefix='mangrove-safe-evidence-') as temp:
    part=Path(temp);qmp=QMP();control=safe.ReedControl(run,'create')
    for text in ('','PASS','Saved opened file.','NORMAL  copied.txt'):
        control.observe(qmp,text,part,0)
    assert not qmp.keys
    try:control.finish(False,False)
    except ValueError:controls+=1
    else:raise AssertionError('missing actual editor qualified')
    # Exact prefix required; incomplete or unrelated editor screens cannot
    # advance keyboard injection or accidentally submit a save command.
    control.shoot_stage=3;control.stage=6
    control.observe(qmp,'/saveas wrong',part,0);assert not qmp.keys
    control.observe(qmp,'/',part,0);assert qmp.keys==['sendkey s 30']
    control.observe(qmp,'/',part,0);assert len(qmp.keys)==1
    control.observe(qmp,'/s',part,0);assert len(qmp.keys)==2
    safe.key(qmp,'>');assert qmp.keys[-1]=='sendkey shift-dot 30'
    for phase in ('create','verify','readonly'):
        incomplete=safe.ReedControl(run,phase)
        for expected_failure,failed in ((False,False),(True,True),(True,False),(False,True)):
            try:incomplete.finish(expected_failure,failed)
            except ValueError:controls+=1
            else:raise AssertionError('incomplete application qualified')
print(f'Safe-file evidence: {controls} incorrect/missing result, identity, count, offset, ownership and application controls rejected')
class Receiver:
    def __init__(self,message,peer=('127.0.0.1',12345)):self.message=message;self.peer=peer
    def recvfrom(self,limit):
        if self.message is None:raise BlockingIOError
        message=self.message;self.message=None;return message,self.peer
with tempfile.TemporaryDirectory(prefix='mangrove-safe-notification-') as temp:
    part=Path(temp);qmp=QMP()
    for message,peer in ((b'PASS',('127.0.0.1',2)),(b'REED1 external run=0000000000000000\n',('127.0.0.1',2)),(f'REED1 external run={run}\n'.encode(),('192.0.2.1',2))):
        c=safe.ReedControl(run,'create',Receiver(message,peer));c.stage=3;c.shoot_stage=3
        try:c.observe(qmp,'Saved opened file.',part,0)
        except ValueError:pass
        else:raise AssertionError('incorrect notification accepted')
    c=safe.ReedControl(run,'create',Receiver(None));c.stage=3;c.shoot_stage=3
    c.observe(qmp,'Saved opened file.',part,0);assert not qmp.keys
print('External-change notification: absent, wrong run/source and printed-save-only cannot advance the stale-save attempt')

# Full keyboard-controller rehearsal, including trailing-space rendering. This
# tests the harness only; actual applications and persistence require QEMU.
class ReplyReceiver(Receiver):
    def sendto(self,message,peer):return len(message)
with tempfile.TemporaryDirectory(prefix='mangrove-safe-keyboard-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create',ReplyReceiver(None))
    for command in ('/bin/say shoot > /temp/shoot.txt','history -a','exit'):
        for i in range(len(command)+1):c.observe(qmp,('developer ~: '+command[:i]).rstrip(),part,0)
    assert c.shoot_stage==3
    c.observe(qmp,'base\nNORMAL  reed.txt  1:5',part,0)
    c.observe(qmp,'base\nNORMAL  reed.txt  1:1',part,0)
    for i in range(7):c.observe(qmp,'edited'[:i]+'base\nINSERT  reed.txt [+]',part,0)
    c.observe(qmp,'NORMAL  reed.txt [+]',part,0)
    c.receiver.message=f'REED1 external run={run}\n'.encode()
    c.observe(qmp,'NORMAL  reed.txt  Saved opened file.',part,0)
    c.observe(qmp,'Changed: [c] Cancel [a] Save As [o] Overwrite:',part,0)
    c.observe(qmp,'Save cancelled; external change unresolved',part,0)
    def command(text):
        for i in range(len(text)+1):c.observe(qmp,('/'+text[:i]).rstrip(),part,0)
    command('saveas occupied.txt');c.observe(qmp,'Save refused: destination exists',part,0)
    command('saveas copied.txt');c.observe(qmp,'NORMAL  copied.txt  Saved new file.',part,0)
    command('q');c.finish(False,False)
    assert c.external_seen and c.stage==11
print('Complete keyboard rehearsal: Shoot, edited save, external change, conflict, cancellation and exclusive Save As are ordered by rendered witnesses')

# A dict must not hide two identical on-disk names from the independent oracle.
import struct
entry=bytearray(96);entry[0]=0x85;entry[1]=2;entry[32]=0xc0;entry[33]=1;entry[35]=1;entry[64]=0xc1
entry[66:68]='a'.encode('utf-16-le')
checksum=0
for i,b in enumerate(entry):
    if i not in (2,3):checksum=(((checksum>>1)|((checksum&1)<<15))+b)&65535
struct.pack_into('<H',entry,2,checksum)
reader=object.__new__(safe.secondary.ExfatReader)
assert list(reader.entries(bytes(entry)+bytes(32)))==['a']
try:reader.entries(bytes(entry)*2+bytes(32))
except ValueError as error:assert str(error)=='duplicate independent namespace'
else:raise AssertionError('duplicate media namespace hidden by dict')
print('Independent exFAT oracle rejects duplicate complete checksummed entry sets instead of overwriting a dict entry')
