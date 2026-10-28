#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Complete FAT32/exFAT VFS resolved-handoff admission with deterministic waiters."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tests'/'shared'))
sys.path.insert(0,str(ROOT/'tests/filesystems/mfqs'))
import artifact_lifecycle as artifacts
from runner import build,encode,response,FORMAT_ARGS
from images import create,external_tools,check
compiler=sys.argv[1] if len(sys.argv)>1 else 'gcc'
workspace=artifacts.ArtifactWorkspace(name='fat-handoff')
folder=workspace.path
flags=[compiler,'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-type-limits',
       '-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread','-DMFQS_ADMISSION_TEST',
       '-Ikernel/include','-Iinclude','-idirafter','libc/include','-Itests/filesystems']
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','PYTHONDONTWRITEBYTECODE':'1'}
try:
    workspace.require_free_space(128*1024*1024,
                                 'FAT32/exFAT handoff regression artifacts')
    renames=['mutex_init','mutex_lock','mutex_try_lock','mutex_unlock','spinlock_init','spin_lock_irqsave','spin_unlock_irqrestore','thread_current','kmalloc','kfree','mfqs_heap_contains']
    subprocess.run(flags+['-D'+n+'=mfqs_'+n for n in renames]+['-c','tests/filesystems/mfqs/host.c','-o',str(folder/'host.o')],cwd=ROOT,check=True)
    for backend in ('fat32','exfat'):
        binary=folder/backend
        subprocess.run(flags+['-DTEST_'+backend.upper(),'tests/filesystems/mgfs/mgfs_handoff_test.c','kernel/src/vfs.c',str(folder/'host.o'),'libc/src/utf8.c']+(['libc/src/time_convert.c'] if backend=='exfat' else [])+['-o',str(binary)],cwd=ROOT,check=True)
        adapter_dir=folder/(backend+'-adapter');adapter_dir.mkdir()
        adapter,tools=build(adapter_dir,compiler,backend=backend)
        tools.update(external_tools(backend));source=folder/(backend+'-initial.img')
        create(source,backend,'empty',tools,FORMAT_ARGS)
        trace=[{'op':'HELLO'},{'op':'MKDIR','path':'/core'},{'op':'CREATE_ORDINARY','path':'/core/deviced'},
               {'op':'WRITE','data':(b'\x7f'+bytes(range(256))*16).hex()},{'op':'CLOSE'},
               {'op':'CREATE_ORDINARY','path':'/history'},{'op':'WRITE','data':b'old line\n'.hex()},{'op':'CLOSE'}]
        prepared=subprocess.run([str(adapter),str(source)],input=''.join(encode(c,i+1) for i,c in enumerate(trace)),text=True,capture_output=True,check=True,env=env)
        for i,(step,line) in enumerate(zip(trace,prepared.stdout.splitlines()),1):
            result=response(step,line,i,backend);assert result['status']==0 and result['pins']==0,result
        assert len(prepared.stdout.splitlines())==len(trace)
        for operation in (('replace',) if '--control-only' in sys.argv else ('replace','write','truncate','create','rename')):
            for case in (('legacy-fat-control',) if '--control-only' in sys.argv else (*(['legacy-fat-control'] if operation=='replace' else []),'success','failed-handoff','removal','torn','flush','exhausted')):
                image=folder/(backend+'-'+operation+'-'+case+'.img');shutil.copyfile(source,image)
                run=subprocess.run([str(binary),str(image),case,operation],text=True,capture_output=True,env=env,timeout=30)
                image.with_suffix('.log').write_text(run.stdout+run.stderr)
                assert run.returncode==0,run.stdout+run.stderr
                print(run.stdout,end='')
                if case=='success':check(image,backend,tools)
                image.unlink()
except BaseException as error:
    try:
        report=workspace.finish({'status':'FAIL',
                                 'error':f'{type(error).__name__}: {error}'},
                                verified=False)
        print('Failure evidence:',report.get('failure_evidence',folder),file=sys.stderr)
    except BaseException as cleanup_error:
        print(f'Owned workspace retained for review: {folder} ({cleanup_error})',file=sys.stderr)
    raise
else:
    workspace.finish({'status':'PASS'},verified=True)
    print('FAT32/exFAT handoff temporary artifacts removed after verification')
