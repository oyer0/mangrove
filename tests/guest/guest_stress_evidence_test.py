#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Supervisor/evidence rejection tests. These fixtures are not guest execution."""
import sys
import tempfile
import subprocess
from pathlib import Path
sys.dont_write_bytecode=True
import guest_stress_test as stress
run='0000000000000000'
for phase in ('create','verify'):
    for backend in ('mgfs','fat32','exfat'):
        text=f'VOLUME1 phase=volume-stress-{phase} run={run} label={stress.secondary.LABELS[backend]} filesystem={backend} target=/vol/{stress.secondary.LABELS[backend]}/work\n'
        text+=f'VOLUME1 identity instance=5 parent=6 name=usb mount=/vol/{stress.secondary.LABELS[backend]}\n'
        start=stress.secondary.expected_vops('create' if phase=='create' else 'verify',backend)
        start=start[:start.index(('seed-close',0))+1]
        vrows=start+[('final-unmount',0),('final-detached',-2),('final-snapshot',0),('final-suppressed',0)]
        text+=''.join(f'VOP {i} {n} expected={v} observed={v}\n' for i,(n,v) in enumerate(vrows,1))
        text+=f'STRESSCHILD phase={phase} run={run} instance=5 parent=6\n'
        rows=[('owner-snapshot',0),('owners',0)];text+=''.join(f'TSTEP {i} {n} expected={v} observed={v}\n' for i,(n,v) in enumerate(rows,1))
        for _ in range(3 if phase=='verify' else 1):
            at=len(rows)+1;rows += [('spawn',1),('wait',0),('process-close',0),('status',0)]
            text+=f'TSTEP {at} spawn expected=1 observed=1\nFSBEGIN fixture-only-not-qualified\nFSVERIFIED ops=1 checks=1\n'
            text+=''.join(f'TSTEP {i} {n} expected={v} observed={v}\n' for i,(n,v) in enumerate(rows[at:],at+1))
        at=len(rows)+1;rows += [('owner-snapshot',0),('owners',0)]
        text+=''.join(f'TSTEP {i} {n} expected={v} observed={v}\n' for i,(n,v) in enumerate(rows[at-1:],at))
        text+=f'STRESSCHILD complete phase={phase} run={run} steps={len(rows)}\nVOLUME1 complete phase=volume-stress-{phase} run={run} ops={len(vrows)}\n'
        stress.environment(text,phase,backend,run)
        missing=[line+'\n' for line in text.splitlines() if any(w in line for w in (' wait ',' status ','process-close',' complete ','identity instance'))]
        bad=[text.replace(line,'',1) for line in missing]
        bad += [text+'extra result\n',text.replace('observed=0','observed=-12',1),text.replace('expected=0 observed=0','expected=43 observed=43',1),
                text.replace('owners expected=0 observed=0','owners expected=0 observed=1',1)]
        done='FSVERIFIED ops=1 checks=1\n';wait='TSTEP 4 wait expected=0 observed=0\n'
        bad.append(text.replace(done+wait,wait+done,1))
        for broken in bad:
            try:stress.environment(broken,phase,backend,run)
            except (ValueError,IndexError):pass
            else:raise AssertionError('incomplete or incorrect supervisor qualified')
        # Parent's spawn-result print can legitimately appear inside child output.
        spawn='TSTEP 3 spawn expected=1 observed=1\n';begin='FSBEGIN fixture-only-not-qualified\n'
        stress.environment(text.replace(spawn+begin,begin+spawn,1),phase,backend,run)
with tempfile.TemporaryDirectory(prefix='mangrove-stress-prefix-') as temp:
    folder=Path(temp)
    trace=dict(seed=0,profile=3,caps=10,count=2,control=0,
               ops=[(0,0,4,0,0,0,0),(1,0,4,0,0,0,0)])
    stress.failing_prefix(folder,'FSFAILED first=1 checks=1 prefix_ops=1\n',trace)
    encoded=(folder/'failing-prefix.trace').read_bytes()
    assert stress.decode(encoded)['ops']==trace['ops'][:1]
    stress.failing_prefix(folder,'FSFAILED first=3 checks=1 prefix_ops=3\n',trace)
    assert (folder/'failing-prefix.trace').read_bytes()==encoded
    stress.failing_prefix(folder,'FSFAIL op=2 kind=result\nFSUNCERTAIN retained-by-filesystem=required\n',trace)
    assert stress.decode((folder/'failing-prefix.trace').read_bytes())['ops']==trace['ops']
    for output,extra,message in [('bad,option',[],'QEMU image paths'),('bad-profile',['--backend','fat32','--extra-controls'],'extra controls require MGFS')]:
        path=folder/output
        r=subprocess.run([sys.executable,str(Path(stress.__file__)), '--build-dir',str(folder),'--host',str(folder/'absent-host'),'--output',str(path),*extra],text=True,capture_output=True,timeout=10)
        assert r.returncode==2 and message in r.stderr and not path.exists(),(r.returncode,r.stderr)
print('Stress evidence: actual wait/status/close, device identity, owner checks, partial ordering, complete lifecycle and unknown-output requirements PASS')
