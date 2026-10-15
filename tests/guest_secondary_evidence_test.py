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
