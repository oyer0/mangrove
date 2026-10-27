#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Evidence rejection controls, not simulated guest or filesystem qualification."""
from pathlib import Path
import sys
import tempfile
import time
sys.dont_write_bytecode=True
import guest_safe_test as safe


run='0123456789abcdef'
assert safe.pattern(run,8,4)==bytes.fromhex('0422405e7c9ab8d6')
identities={s:100+i for i,s in enumerate(safe.SIZES)}
assert safe.fat_listing_names('::/work/REED.TXT\n::/work/safe-0123456789abcdef/\n')==['REED.TXT','safe-'+run]
assert safe.work_names('fat32',run)==['seed.txt','safe-'+run,'REED.TXT','COPIED.TXT','OCCUPIED.TXT']
controls=0
expected_shoot_error='Could not read "/temp/no-such-safe-input": not found.\n'
safe.validate_deliberate_shoot_failure(expected_shoot_error)
for incomplete in ('',
                   expected_shoot_error.replace('not found.', 'permission denied.'),
                   expected_shoot_error+'unexpected output\n'):
    try:safe.validate_deliberate_shoot_failure(incomplete)
    except ValueError:controls+=1
    else:raise AssertionError('incomplete or altered Shoot failure evidence accepted')
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

# Keep the cross-section counter aligned with the interactive Shoot path. The
# individual operation rows are checked separately against production output.
with tempfile.TemporaryDirectory(prefix='mangrove-shoot-section-count-') as temp:
    run='0123456789abcdef'
    def sop_rows(count):
        return ''.join(f'SOP {i} fixture expected=0 observed=0\n'
                       for i in range(1,count+1))
    shoot=('SHOOT1 phase=create run='+run+'\n'+sop_rows(41)+
           expected_shoot_error+
           'SHOOT1 complete phase=create run='+run+' ops=41\n')
    sizes=''.join(f'SAFE1 size={size}\n' for size in safe.SIZES)
    safe_block=(f'SAFE1 phase=create run={run} filesystem=mgfs '
                'target=/vol/QUAL_MGFS/work\n'+sop_rows(333)+
                'RT1 PASS core-c17-runtime\n'+sizes+
                f'SAFE1 complete phase=create run={run} ops=333\n')
    reed=(f'REED1 phase=create run={run}\n'+sop_rows(18)+
          f'REED1 complete phase=create run={run} ops=18\n')
    volume_rows=safe.secondary.expected_vops('create','mgfs')
    volume_rows=volume_rows[:volume_rows.index(('seed-close',0))+1]
    volume_rows += [('final-unmount',0),('final-detached',-2),
                    ('final-snapshot',0),('final-suppressed',0)]
    volume=''.join(f'VOP {i} {name} expected={value} observed={value}\n'
                   for i,(name,value) in enumerate(volume_rows,1))
    header=(f'VOLUME1 phase=volume-safe-create run={run} label=QUAL_MGFS '
            'filesystem=mgfs target=/vol/QUAL_MGFS/work\n')
    identity='VOLUME1 identity instance=2 parent=1 name=QUAL_MGFS mount=/vol/QUAL_MGFS\n'
    complete='CRT1 atexit\n'
    transcript=header+shoot+safe_block+reed+identity+volume+complete
    safe.validate_sections(transcript,'create','mgfs',run,333,18,True,True)
    malformed=transcript.replace(identity,identity+'SOP 1 stray expected=0 observed=0\n')
    try:safe.validate_sections(malformed,'create','mgfs',run,333,18,True,True)
    except ValueError as error:
        assert str(error)=='out-of-section native operation'
        controls+=1
    else:raise AssertionError('out-of-section operation accepted')
    malformed=transcript.replace('ops=41\n','ops=42\n')
    try:safe.validate_sections(malformed,'create','mgfs',run,333,18,True,True)
    except ValueError as error:
        assert str(error)=='Shoot operation count differs from completion'
        controls+=1
    else:raise AssertionError('incorrect Shoot completion count accepted')
class QMP:
    def __init__(self):self.keys=[];self.calls=[]
    def call(self,name,args):
        self.calls.append((name,args))
        if name=='human-monitor-command':self.keys.append(args['command-line'])
        if name=='input-send-event':
            pressed=[event['data']['key']['data'] for event in args['events']
                     if event['data']['down']]
            if pressed:
                modifiers=[key for key in pressed if key in ('shift','ctrl','alt')]
                keys=[key for key in pressed if key not in ('shift','ctrl','alt')]
                code='-'.join(modifiers+keys)
                self.keys.append('sendkey '+code+' 100')
        return {}
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
    safe.key(qmp,'L');assert qmp.keys[-1]=='sendkey shift-l 30'
    safe.key(qmp,'_');assert qmp.keys[-1]=='sendkey shift-minus 30'
    key_count=len(qmp.keys)
    safe.key(qmp,'shift-u')
    assert qmp.keys[-1]=='sendkey shift-u 30' and len(qmp.keys)==key_count+1
    assert qmp.calls[-1] == (
        'human-monitor-command', {'command-line':'sendkey shift-u 30'})
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

# The integrated shell controller acts only on the exact observed prompt line.
with tempfile.TemporaryDirectory(prefix='mangrove-shoot-acceptance-') as temp:
    part=Path(temp);qmp=QMP();c=safe.ReedControl(run,'create')
    c.reed_from_shoot=True;c.reed_path=f'/temp/reed-launch-{run}.shoot'
    c.observe_interactive_shell(qmp,'unrelated output',part)
    assert not qmp.keys and c.shell_stage==0
    c.observe_interactive_shell(qmp,'developer ~: /bin/shoot /temp/create.shoot',part)
    assert not qmp.keys and c.shell_stage==0 and not c.shell_active
    c.observe_interactive_shell(qmp,'developer ~:',part)
    assert not qmp.keys and c.shell_stage==0 and c.shell_active
    c.shell_typed=len('/bin/say shot')
    c.observe_interactive_shell(qmp,'developer ~: /bin/say shot',part)
    assert not qmp.keys and c.shell_stage==1
    c.observe_interactive_shell(qmp,'developer ~: /bin/say shot',part)
    assert qmp.keys==['sendkey left 30'] and c.shell_stage==2
    c.observe_interactive_shell(qmp,'developer ~: /bin/say shot',part)
    assert qmp.keys[-1]=='sendkey o 30' and c.shell_stage==3
    c.observe_interactive_shell(qmp,'developer ~: /bin/say shoot',part)
    assert qmp.keys[-1]=='sendkey end 30' and c.shell_stage==4
    c.shell_typed=1
    c.observe_interactive_shell(qmp,'developer ~: /bin/say shoot ',part)
    assert qmp.keys[-1]=='sendkey shift-dot 30' and c.shell_stage==4
    c.shell_stage=19;c.shell_typed=1;c.shell_prompt='developer ~:'
    c.observe_interactive_shell(qmp,'developer ~: /',part)
    assert qmp.keys[-1]=='sendkey b 30' and c.shell_typed==2
print('Interactive Shoot controller: unrelated output cannot trigger input; exact command state drives cursor editing')

# The filename-listing probe uses shell stages 27 and 28 after completing the
# directory-completion sequence. These stages must remain in the integrated
# shell controller even though Reed handoff begins at stage 20.
with tempfile.TemporaryDirectory(prefix='mangrove-shoot-post-cd-') as temp:
    part=Path(temp);qmp=QMP();c=safe.ReedControl(run,'create')
    c.reed_from_shoot=True;c.shell_active=True;c.shell_stage=17
    c.shell_prompt='developer /vol/QUAL_MGFS/work:'
    c.shell_progress=time.monotonic();c.volume_label='QUAL_MGFS'
    prompt=c.shell_prompt+'\n'+'\n'*20
    c.observe(qmp,prompt,part,0)
    assert c.shell_stage==27 and not qmp.keys
    c.observe(qmp,prompt,part,0)
    assert c.shell_stage==27 and qmp.keys==['sendkey l 30']
    c.shell_stage=20
    qmp.keys.clear()
    c.observe(qmp,prompt,part,0)
    assert not qmp.keys
print('Integrated shell controller: post-directory stages remain active before Reed handoff')

# Waiting after the final keyboard action must be bounded even after the
# controller has entered the post-editor shell stage. A login screen or lost
# child transition must fail with the current state instead of using the full
# guest timeout.
with tempfile.TemporaryDirectory(prefix='mangrove-shoot-stall-') as temp:
    part=Path(temp);qmp=QMP();c=safe.ReedControl(run,'create')
    c.reed_from_shoot=True;c.shell_active=True;c.shell_stage=23
    c.stage=9;c.filename_probe_stage=6
    c.shell_progress=time.monotonic()-26
    try:
        c.observe(qmp,'Username:',part,0)
    except TimeoutError as error:
        assert 'shell stage 23' in str(error) and 'editor stage 9' in str(error)
    else:
        raise AssertionError('stalled post-Reed shell stage was not bounded')
print('Integrated shell controller: post-editor stalls fail with state diagnostics')

# Reed quit is a slash command, not a Normal-mode `q` binding. Give the two
# pathname probes distinct titles and contents so the next editor is a durable
# rendered witness that `/q` returned to Shoot and the next command ran.
with tempfile.TemporaryDirectory(prefix='mangrove-shoot-space-name-probes-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create')
    c.reed_from_shoot=True;c.shoot_stage=3;c.shell_stage=20
    c.filename_probe_stage=0
    editor='NORMAL  /vol/QUAL_MGFS/work/hello world!\nargv-probe'
    quoted='NORMAL  /vol/QUAL_MGFS/work/hello quoted!\nquoted-probe'
    main='NORMAL  /vol/QUAL_MGFS/work/reed.txt\nbase'
    c.observe(qmp,editor,part,0)
    assert c.filename_probe_stage==1 and qmp.keys[-1]=='sendkey slash 30'
    c.observe(qmp,'/',part,0)
    assert c.filename_probe_stage==1 and qmp.keys[-1]=='sendkey q 30'
    c.observe(qmp,'/q',part,0)
    assert c.filename_probe_stage==2 and qmp.keys[-1]=='sendkey ret 30'
    c.observe(qmp,editor,part,0)
    assert c.filename_probe_stage==2 and qmp.keys[-1]=='sendkey ret 30'
    c.observe(qmp,quoted,part,0)
    assert c.filename_probe_stage==3
    c.observe(qmp,quoted,part,0)
    assert c.filename_probe_stage==4 and qmp.keys[-1]=='sendkey slash 30'
    c.observe(qmp,'/',part,0);c.observe(qmp,'/q',part,0)
    assert c.filename_probe_stage==5 and qmp.keys[-1]=='sendkey ret 30'
    c.observe(qmp,quoted,part,0)
    assert c.filename_probe_stage==5
    c.observe(qmp,main,part,0)
    assert c.filename_probe_stage==6
print('Reed argv probes: both editor sessions close through the documented /q command')

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
    c.observe(qmp,'NORMAL  reed.txt [+]\neditedbase',part,0)
    c.observe(qmp,'NORMAL  reed.txt\nbase',part,0)
    c.observe(qmp,'NORMAL  reed.txt [+]\neditedbase',part,0)
    c.observe(qmp,'NORMAL  reed.txt\nbase',part,0)
    for i in range(7):
        c.observe(qmp,'edited'[:i]+'base\nINSERT  reed.txt [+]',part,0)
    c.observe(qmp,'NORMAL  reed.txt [+]\neditedbase',part,0)
    def command(text):
        for i in range(len(text)+1):c.observe(qmp,('/'+text[:i]).rstrip(),part,0)
    command('s')
    c.receiver.message=f'REED1 external run={run}\n'.encode()
    c.observe(qmp,'NORMAL  reed.txt [+]\nSaved opened file.',part,0)
    command('s')
    c.observe(qmp,'Changed: [c] Cancel [a] Save As [o] Overwrite:',part,0)
    c.observe(qmp,'Save cancelled; external change unresolved',part,0)
    command('saveas occupied.txt');c.observe(qmp,'Save refused: destination exists',part,0)
    command('saveas copied.txt');c.observe(qmp,'NORMAL  copied.txt  Saved new file.',part,0)
    command('q');c.observe(qmp,'developer ~:',part,0);c.finish(False,False)
    assert c.external_seen and c.stage==12
print('Complete keyboard rehearsal: Shoot, edited save, external change, conflict, cancellation and exclusive Save As are ordered by rendered witnesses')

# The no-conflict mode still runs the full native editor process and checks
# undo, a second edit, Save As, exit and return from the alternate screen. It omits only
# the host UDP rendezvous used to mutate the open file in the separate conflict
# workflow.
with tempfile.TemporaryDirectory(prefix='mangrove-reed-save-only-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create');c.reed_no_conflict=True;c.reed_from_shoot=True;c.reed_path=f'/temp/reed-launch-{run}.shoot';c.filename_probe_stage=6;c.actions.append({'action':'list-space-filename'})
    c.shell_stage=20;c.shoot_stage=3
    normal='NORMAL  /vol/QUAL_MGFS/work/reed.txt'
    insert='INSERT  /vol/QUAL_MGFS/work/reed.txt [+]'
    c.observe(qmp,normal+'\nbase',part,0)
    c.observe(qmp,normal+'  1:1\nbase',part,0)
    for i in range(7):
        c.observe(qmp,'edited'[:i]+'base\n'+insert,part,0)
    c.observe(qmp,normal+' [+]\neditedbase',part,0)
    call_count=len(qmp.calls);key_count=len(qmp.keys)
    c.observe(qmp,normal+'\nbase',part,0)
    assert qmp.keys[-1]=='sendkey shift-u 30' and len(qmp.keys)==key_count+1
    assert qmp.calls[call_count:] == [
        ('human-monitor-command', {'command-line':'sendkey shift-u 30'})]
    c.observe(qmp,normal+' [+]\neditedbase',part,0)
    assert qmp.keys[-1]=='sendkey u 30'
    c.observe(qmp,normal+'\nbase',part,0)
    for i in range(7):c.observe(qmp,'edited'[:i]+'base\n'+insert,part,0)
    c.observe(qmp,normal+' [+]\neditedbase',part,0)
    def command(text):
        for i in range(len(text)+1):c.observe(qmp,('/'+text[:i]).rstrip(),part,0)
    command('s')
    c.observe(qmp,normal+' [+]\neditedbase\nSaved opened file.',part,0)
    command('saveas copied.txt')
    c.observe(qmp,'NORMAL  copied.txt\nSaved new file.',part,0)
    command('q')
    c.observe(qmp,'developer ~:',part,0)
    c.observe(qmp,'developer ~:',part,0)
    readback='type /vol/QUAL_MGFS/work/reed.txt'
    for i in range(len(readback)+1):
        c.observe(qmp,'developer ~: '+readback[:i],part,0)
    c.observe(qmp,'editedbase\ndeveloper ~:',part,0)
    c.shell_stage=22
    c.shell_typed=0
    for i in range(len('exit')+1):
        c.observe(qmp,'developer ~: '+'exit'[:i],part,0)
    assert c.shell_stage==23 and c.shoot_stage==4
    c.finish(False,False)
    actions=[row['action'] for row in c.actions]
    assert 'edit-before-undo' in actions
    assert 'undo-restores-base' in actions
    assert 'redo-restores-edit' in actions
    assert 'undo-after-redo' in actions
    assert c.redo_status=='observed'
    assert any(action.startswith('reinsert:') for action in actions)
    assert 'terminal-restored' in actions and c.stage==9
    assert any(action.startswith('shell-read-saved-reed-file:') for action in actions)
    assert 'shell-read-saved-reed-file-submit' in actions
    assert any(action.startswith('shoot-exit-prefix:') for action in actions)
print('Reed keyboard rehearsal: Shoot launches Reed, observes redo, saves, reads the file back and returns from the editor')

# The guest-side synchronization path must act on a rendered stale-generation
# conflict, then retain the edited buffer through cancellation and Save As.
with tempfile.TemporaryDirectory(prefix='mangrove-reed-conflict-sync-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create');c.reed_guest_conflict_sync=True;c.shoot_stage=3
    c.stage=3
    c.observe(qmp,'Changed: [c] Cancel [a] Save As [o] Overwrite:',part,0)
    assert c.external_seen and c.stage==5 and qmp.keys[-1]=='sendkey c 30'
    c.observe(qmp,'Save cancelled; external change unresolved; edits kept.',part,0)
    assert c.stage==6
    def conflict_command(text):
        for i in range(len(text)+1):c.observe(qmp,('/'+text[:i]).rstrip(),part,0)
    conflict_command('saveas occupied.txt')
    c.observe(qmp,'Save refused: destination exists; edits kept.',part,0)
    conflict_command('saveas copied.txt')
    c.observe(qmp,'NORMAL  copied.txt  Saved new file.',part,0)
    conflict_command('q');c.observe(qmp,'developer ~:',part,0)
    c.finish(False,False)
    actions={row['action'] for row in c.actions}
    assert {'stale-conflict','cancel-preserved','saved-as','terminal-restored'} <= actions
print('Guest file rendezvous controller: stale conflict, cancellation, retained edit, Save As and terminal return are required')

# If Shift+U is accepted by QMP but the guest does not render the restored
# edit, the runner records that as incomplete and continues only through the
# already-qualified second-edit path.
with tempfile.TemporaryDirectory(prefix='mangrove-reed-redo-unobserved-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create')
    c.reed_from_shoot=True;c.shoot_stage=3;c.shell_stage=20;c.filename_probe_stage=6
    c.stage=2.15;c.redo_status='attempted';c.redo_requested_at=0.0
    unchanged='NORMAL  reed.txt  1:1\nbase'
    c.observe(qmp,unchanged,part,0)
    assert c.redo_status=='not-observed' and c.stage==2.16
    c.observe(qmp,unchanged,part,0)
    assert qmp.keys[-1]=='sendkey i 30' and c.stage==2.2
print('Native redo fallback control: unchanged buffer is recorded incomplete before re-edit continues')

# Test-build Reed input evidence separates a missing USB HID event from a
# delivered Shift+U whose buffer state still needs qualification.
with tempfile.TemporaryDirectory(prefix='mangrove-reed-redo-input-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create');c.shoot_stage=3;c.stage=2.15
    c.redo_requested_at=0.0
    undo_diagnostic='NORMAL  reed.txt  1:1 test input U modifiers=0 action=undo bytes=4\nbase'
    c.observe(qmp,undo_diagnostic,part,0)
    assert c.redo_input_diagnostic is None
    diagnostic='NORMAL  reed.txt [+]  1:7 test input U modifiers=2 action=redo bytes=10\neditedbase'
    c.observe(qmp,diagnostic,part,0)
    assert c.redo_input_diagnostic=='test input U modifiers=2 action=redo bytes=10'
    assert c.actions[-1]['action']=='redo-input-received'
    c.observe(qmp,'NORMAL  reed.txt  1:1 [+]\neditedbase',part,0)
    assert c.redo_status=='observed' and c.actions[-2]['action']=='redo-restores-edit'
print('Reed redo input evidence: received key/action diagnostics are recorded separately from the rendered buffer transition')

# In the native conflict mode, save the buffer that Shift+U actually restored
# before requesting cancellation; do not undo and reconstruct that text first.
with tempfile.TemporaryDirectory(prefix='mangrove-reed-redo-conflict-') as temp:
    part=Path(temp);(part/'screen.ppm').write_bytes(b'controller-fixture')
    qmp=QMP();c=safe.ReedControl(run,'create');c.shoot_stage=3
    c.reed_guest_conflict_sync=True
    normal='NORMAL  /vol/QUAL_MGFS/work/reed.txt'
    insert='INSERT  /vol/QUAL_MGFS/work/reed.txt [+]'
    c.observe(qmp,normal+'\nbase',part,0)
    c.observe(qmp,normal+'  1:1\nbase',part,0)
    for i in range(7):c.observe(qmp,'edited'[:i]+'base\n'+insert,part,0)
    c.observe(qmp,normal+' [+]\neditedbase',part,0)
    c.observe(qmp,normal+'\nbase',part,0)
    assert qmp.calls[-2][0]=='human-monitor-command' and c.stage==2.15
    diagnostic='editedbase\n'+normal+' [+]  1:7  test input U modifiers=2 action=redo bytes=10'
    c.observe(qmp,diagnostic,part,0)
    c.observe(qmp,diagnostic,part,0)
    c.observe(qmp,'editedbase\n'+normal+' [+]  1:7',part,0)
    assert c.stage==2.31 and qmp.keys[-1]=='sendkey slash 30'
    def conflict_command(text):
        for i in range(len(text)+1):c.observe(qmp,('/'+text[:i]).rstrip(),part,0)
    conflict_command('s')
    assert c.stage==3 and qmp.keys[-1]=='sendkey ret 30'
    c.observe(qmp,'Changed: [c] Cancel [a] Save As [o] Overwrite:',part,0)
    actions=[row['action'] for row in c.actions]
    assert 'redo-restores-edit' in actions and 'undo-after-redo' not in actions
    assert 'redo-input-received' in actions and 'stale-conflict' in actions
print('Native conflict controller: Shift+U restored the edited buffer, which is saved directly into stale-conflict handling')

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

# Only create launches the integrated shell. Verify/readonly have separate
# editor workflows and must still reject missing editor-completion witnesses.
for phase, expected in (("create", 9), ("verify", 3), ("readonly", 9)):
    c=safe.ReedControl(run,phase);c.reed_from_shoot=True;c.stage=expected
    c.shell_stage=23 if phase=="create" else 0
    c.filename_probe_stage=6
    if phase=="create":c.actions.append({'action':'list-space-filename'})
    c.finish(False,False)
    c.stage=expected-1
    try:c.finish(False,False)
    except ValueError:pass
    else:raise AssertionError("incomplete per-phase editor workflow accepted")
c=safe.ReedControl(run,"create");c.reed_from_shoot=True;c.stage=9
c.filename_probe_stage=6;c.actions.append({'action':'list-space-filename'})
try:c.finish(False,False)
except ValueError:pass
else:raise AssertionError("create without integrated shell completion accepted")
print("Per-phase keyboard completion: shell required only during create; editor required in every phase")
