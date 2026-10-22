#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable C stress core and real POSIX adapter, independent Python oracle."""
from pathlib import Path
import os,subprocess,sys,tempfile,json
sys.dont_write_bytecode=True
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from fsstress_oracle import decode,model,validate_log,encode,payload
ROOT=Path(__file__).resolve().parents[3]
cc=sys.argv[1] if len(sys.argv)>1 else 'gcc'
base=[cc,'-std=c17','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all']
source=ROOT/'userspace/fsstress'
folder=Path(tempfile.mkdtemp(prefix='mangrove-fsstress-'))
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}
def run(args,expected=0):
    r=subprocess.run([str(folder/'fsstress'),*map(str,args)],capture_output=True,text=True,env=env,timeout=30)
    (folder/('last-'+str(len(list(folder.glob('*.log'))))+'.log')).write_text(r.stdout+r.stderr)
    assert r.returncode==expected,(r.returncode,r.stdout,r.stderr)
    assert not r.stderr,r.stderr
    return r.stdout
try:
    subprocess.run(base+[str(source/'model.c'),str(ROOT/'tests/filesystems/stress/fsstress_unit.c'),'-o',str(folder/'unit')],check=True)
    subprocess.run([str(folder/'unit')],check=True,env=env)
    subprocess.run(base+[str(source/f) for f in ('model.c','engine.c','main.c','posix.c')]+['-o',str(folder/'fsstress')],check=True)
    for profile in ('mgfs','fat32','exfat','posix'):
        file=folder/(profile+'.trace');run(['plan',profile,'0','96',file]);trace=decode(file.read_bytes());assert trace['profile']==('mgfs','fat32','exfat','posix').index(profile)
    for seed in (0,1,0x5eed7,0xffffffffffffffff):
        file=folder/(str(seed)+'.trace');target=folder/('run-'+str(seed))
        text=run(['run','posix',f'{seed:x}','128',target,file]);trace=decode(file.read_bytes());validate_log(text,trace,root=str(target))
        files,dirs=model(trace)
        assert {str(p.relative_to(target)) for p in target.rglob('*') if p.is_dir()}==dirs-{''}
        assert {str(p.relative_to(target)):p.read_bytes() for p in target.rglob('*') if p.is_file()}==files
        validate_log(run(['verify',target,file]),trace,'verify')
        validate_log(run(['replay',folder/('again-'+str(seed)),file]),trace)
        victim=next((target/p for p,data in files.items() if data),None);assert victim
        original=victim.read_bytes()
        for offset in {0,len(original)//2,len(original)-1}:
            data=bytearray(original);data[offset]^=1;victim.write_bytes(data)
            bad=run(['verify',target,file],43);assert 'kind=content' in bad and f'offset={offset} ' in bad
            victim.write_bytes(original)
        for broken in (text.replace('observed=0','observed=-12',1),text.replace('FSOP 2 ','FSOP 3 ',1),text.replace('FSPASS ','NORESULT ',1),text[:text.index('FSOP 20 ')],
                       text.replace('count=513 observed_count=513','count=512 observed_count=512',1),
                       text.replace('path='+str(target),'path=/tmp/wrong-root',1),text+'unqualified extra result\n',
                       text.replace('witnesses=','witnesses=999',1)):
            try:validate_log(broken,trace,root=str(target))
            except ValueError:pass
            else:raise AssertionError('invalid completed evidence qualified')
    for control in ('model','status'):
        path=folder/(control+'.trace');text=run(['run','posix','0','96',folder/control,path,control],43);trace=decode(path.read_bytes());validate_log(text,trace,failure=control)
        prefix=folder/(control+'-prefix.trace');prefix.write_bytes(encode(trace,trace['ops'][:6]))
        validate_log(run(['replay',folder/(control+'-replay'),prefix],43),decode(prefix.read_bytes()),failure=control)
    trace=decode((folder/'posix.trace').read_bytes());short=folder/'short.trace';short.write_bytes(encode(trace,trace['ops'][:1]))
    assert 'FSINCOMPLETE kind=coverage' in run(['replay',folder/'short',short],2)
    short.write_bytes(short.read_bytes()[:-1]);assert 'kind=trace-load' in run(['replay',folder/'truncated',short],2)
    assert 'kind=platform-capabilities' in run(['replay',folder/'wrong-capabilities',folder/'mgfs.trace'],2)
    subprocess.run(base+['-Dio_write=fs_transport_write','-Dio_read=fs_transport_read','-c',str(source/'posix.c'),'-o',str(folder/'transport.o')],check=True)
    subprocess.run(base+[str(source/f) for f in ('model.c','engine.c','main.c')]+[str(ROOT/'tests/filesystems/stress/fsstress_fault.c'),str(folder/'transport.o'),'-o',str(folder/'fault')],check=True)
    for case,code in [('memory',2),('space',2),('io',43),('short',43),('uncertain',44),('read-error',43)]:
        target=folder/('fault-'+case);tracepath=folder/('fault-'+case+'.trace')
        r=subprocess.run([str(folder/'fault'),'run','posix','0','96',str(target),str(tracepath)],capture_output=True,text=True,env={**env,'FSSTRESS_FAULT':case},timeout=30)
        (folder/('fault-'+case+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==code and not r.stderr,(case,r.returncode,r.stdout,r.stderr)
        assert 'FSPASS' not in r.stdout
        actual=(target/'directory-0/ordinary-entry-0.bin').read_bytes()
        trace=decode(tracepath.read_bytes());wanted=payload(trace['ops'][5][-1],513)
        assert actual==(wanted[:256] if case in ('short','uncertain') else wanted if case=='read-error' else b''),case
        if code==2:assert 'FSINCOMPLETE kind=resource' in r.stdout
        elif code==44:assert 'FSUNCERTAIN retained-by-filesystem=required' in r.stdout
        else:assert 'kind=result' in r.stdout and 'first=6' in r.stdout
    print('fsstress controlled transport: allocation/space, pre-write I/O, confirmed short write, uncertain partial write and online read failure distinguished')
    print('fsstress POSIX: seeds 0,1,0x5eed7,UINT64_MAX; independent bytes/namespace, replay, model/status/content/incomplete evidence controls PASS')
except BaseException:
    print('Failure artifacts:',folder,file=sys.stderr);raise
else:print('Artifacts:',folder)
