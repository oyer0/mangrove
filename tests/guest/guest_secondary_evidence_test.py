#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Evidence/parser negative controls; synthetic transcripts are not guest tests."""
import re
import sys
sys.dont_write_bytecode=True
import guest_secondary_test as secondary
import guest_mgfs_test as guest

run='0123456789abcdef'

def content(phase):
    rows=guest.expected_operations(phase,run,221)
    return (f'GUESTFS1 phase={phase} run={run} bytes=8193\n'+
        ''.join(f'OP {i} {name} expected={n} observed={n}\n' for i,(name,n) in enumerate(rows,1))+
        f'GUESTFS1 complete phase={phase} run={run} ops={len(rows)} identifier=221\n')

for backend,label in secondary.LABELS.items():
    for phase in ('create','verify','readonly','denied'):
        text=f'VOLUME1 phase=volume-{phase} run={run} label={label} filesystem={backend} target=/vol/{label}/work\n'
        for i,(name,value) in enumerate(secondary.expected_vops(phase,backend),1):
            text+=f'VOP {i} {name} expected={value} observed={value}\n'
            if name=='readonly-state':
                text+=f'VOLUME1 identity instance=4 parent=5 name=disk2p1 mount=/vol/{label}\n'
            if name=='seed-close' and phase!='denied': text+=content('create' if phase=='create' else 'verify')
            if name=='denied-mount-still-present': text+=content('verify')
            if name=='child-spawn': text+='VOLUME1 child-open-handles-exit\n'
            if name=='remount-identity': text+=content('verify')
        text+=f'VOLUME1 complete phase=volume-{phase} run={run} ops={len(secondary.expected_vops(phase,backend))}\n'
        secondary.validate(text,phase,run,backend)
        negatives=['',text.replace('observed=8193','observed=8192',1),
                   text.replace('direct-mount-denied expected=-25 observed=-25','direct-mount-denied expected=0 observed=0'),
                   text.replace('automounted expected=1 observed=1','automounted expected=1 observed=0'),
                   text.replace('parent=5','parent=4'),text.replace('name=disk2p1','name=disk1'),
                   text.replace(label,'UNEXPECTED'),text.split('VOLUME1 complete')[0],
                   text+'PASS\n',text.replace(' bytes expected=8193',' wrong-bytes expected=8193',1)]
        if phase=='create':
            negatives += [text.replace('VOLUME1 child-open-handles-exit\n',''),
                text.replace('busy-file expected=-14 observed=-14','busy-file expected=0 observed=0'),
                text.replace('not-automounted expected=0 observed=0','not-automounted expected=0 observed=1')]
        for bad in negatives:
            assert bad != text, (backend, phase, 'ineffective negative control')
            try: secondary.validate(bad,phase,run,backend)
            except ValueError: pass
            else: raise AssertionError('invalid lifecycle evidence qualified')

# The independent decoder must reject unsupported/corrupt geometry, not infer PASS.
for image in (b'',bytes(512),b'\0'*3+b'EXFAT   '+bytes(501)):
    try: secondary.ExfatReader(image)
    except ValueError: pass
    else: raise AssertionError('unsupported decoder layout accepted')
print('Secondary evidence: 12 valid schemas; missing/corrupt outcomes, identities, content, child cleanup and lifecycle witnesses rejected')

# Idle hotplug evidence is exact and ordered; these are parser controls only.
text=f'HOTPLUG1 phase=idle run={run} label=QUAL_MGFS target=/vol/QUAL_MGFS/work\n'
for i,(name,n) in enumerate(secondary.hotplug_rows(),1):
    text+=f'VOP {i} {name} expected={n} observed={n}\n'
    if name=='initial-gpt-usb': text+='HOTPLUG1 original instance=4 parent=5\n'
    if name=='seed-close':text+=content('create')
    if name=='offline-open': text+=content('create')
    if name=='replacement-gpt-usb': text+='HOTPLUG1 replacement instance=8 parent=9\n'
    if name=='stale-lifecycle-preserved':text+=content('verify')
text+=f'HOTPLUG1 complete run={run} ops={len(secondary.hotplug_rows())}\n'
events=[{'event':'DEVICE_DELETED','data':{'device':'secondary-storage'}}]
actions=[{'stage':i,'seconds':i+1,'marker':f'HOTPLUG1 {stage} run={run}'} for i,stage in enumerate(('ready','removed'))]
secondary.validate_hotplug(text,run,events,actions)
cases=[('',events,actions),(text,[],actions),
       (text,[{'event':'DEVICE_DELETED','data':{'device':'other'}}],actions),
       (text,events,actions[:1]),(text,events,actions[::-1]),
       (text.replace('instance=8','instance=4'),events,actions),
       (text.replace('offline-open expected=-2 observed=-2','offline-open expected=-2 observed=1'),events,actions),
       (text.replace('observed=8193','observed=8192',1),events,actions),
       (text.split('HOTPLUG1 complete')[0],events,actions),
       (text.replace('old-identities-absent expected=1 observed=1','old-identities-absent expected=1 observed=0'),events,actions)]
for bad,bad_events,bad_actions in cases:
    try: secondary.validate_hotplug(bad,run,bad_events,bad_actions)
    except ValueError: pass
    else: raise AssertionError('invalid hotplug evidence qualified')
print('Hotplug evidence: missing/mismatched deletion, absent observation, stale identities, unexpected success, wrong content and incomplete execution rejected')

# Retained references must produce DEVICE_GONE before and after fresh registration.
text=f'HOTPLUG1 phase=idle run={run} label=QUAL_MGFS target=/vol/QUAL_MGFS/work\n'
for i,(name,n) in enumerate(secondary.hotplug_rows(True),1):
    text+=f'VOP {i} {name} expected={n} observed={n}\n'
    if name=='initial-gpt-usb':text+='HOTPLUG1 original instance=4 parent=5\n'
    if name=='seed-close':text+=content('create')
    if name=='offline-stale-cursor':text+=content('create')
    if name=='replacement-gpt-usb':text+='HOTPLUG1 replacement instance=8 parent=9\n'
    if name=='stale-lifecycle-preserved':text+=content('verify')
text+=f'HOTPLUG1 complete run={run} ops={len(secondary.hotplug_rows(True))}\n'
secondary.validate_hotplug(text,run,events,actions,True)
for step in ('retained-busy','offline-stale-read','offline-stale-write','offline-stale-cursor',
             'replacement-stale-read','replacement-stale-write','replacement-stale-cursor'):
    bad=re.sub(r'('+step+r' expected=-?\d+ observed=)-?\d+',r'\g<1>0',text)
    try:secondary.validate_hotplug(bad,run,events,actions,True)
    except ValueError:pass
    else:raise AssertionError('unsafe retained reference qualified')
# A child deletion event or another frontend must never acknowledge our deletion.
qmp=object.__new__(guest.QMP)
qmp.events=[{'event':'DEVICE_DELETED','data':{'path':'/child'}},
            {'event':'DEVICE_DELETED','data':{'device':'other'}}]
qmp.timeout=1.0
def deadline(_deadline,_operation):raise TimeoutError('controlled missing frontend event')
qmp.receive=deadline
try:qmp.wait_deleted('secondary-storage')
except TimeoutError:pass
else:raise AssertionError('unmatched deletion accepted')
qmp.events+=events;qmp.wait_deleted('secondary-storage')
print('Retained-reference and QMP controls: BUSY/DEVICE_GONE required, wrong or missing frontend deletion refused')
for answer in ('y','n'):
    assert guest.confirmation_echo('request\n[y/N] '+answer+'\n',answer)
    for text in ('','[y/N]','[y/N] y\nnew request\n[y/N]',
                 '[y/N] '+answer+answer,'[y/N] '+('n' if answer=='y' else 'y')):
        assert not guest.confirmation_echo(text,answer)
print('Authorization injection: requires current native echo; old, absent, wrong and duplicated input refused')
