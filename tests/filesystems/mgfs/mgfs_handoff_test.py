#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production VFS/MGFS resolved-handoff admission with real host waiters."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests' / 'shared'))
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tests/filesystems/mfqs'))
import artifact_lifecycle as artifacts
from runner import build as build_fixture, encode, response, FORMAT_ARGS
from images import create
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
workspace = artifacts.ArtifactWorkspace(name='mgfs-handoff')
folder = workspace.path
env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'PYTHONDONTWRITEBYTECODE': '1'}
flags = [compiler, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread', '-DMFQS_ADMISSION_TEST',
         '-Ikernel/include', '-Iinclude', '-idirafter', 'libc/include', '-Itests/filesystems']
try:
    workspace.require_free_space(64 * 1024 * 1024,
                                 'MGFS handoff regression artifacts')
    renames = ['mutex_init', 'mutex_lock', 'mutex_try_lock', 'mutex_unlock',
               'spinlock_init', 'spin_lock_irqsave', 'spin_unlock_irqrestore', 'thread_current', 'kmalloc', 'kfree', 'mfqs_heap_contains']
    subprocess.run(flags + ['-D' + n + '=mfqs_' + n for n in renames] +
                   ['-c', 'tests/filesystems/mfqs/host.c', '-o', str(folder / 'host.o')], cwd=ROOT, check=True)
    subprocess.run(flags + ['tests/filesystems/mgfs/mgfs_handoff_test.c', 'kernel/src/vfs.c', str(folder / 'host.o'),
                   'libc/src/utf8.c',
                   '-o', str(folder / 'test')], cwd=ROOT, check=True)
    # Build the ordinary production VFS adapter and independent formatter/checker.
    adapter, tools = build_fixture(folder, compiler)
    source = folder / 'initial.img'
    create(source, 'mgfs', 'empty', tools, FORMAT_ARGS)
    trace = [{'op':'HELLO'}, {'op':'MKDIR','path':'/core'},
             {'op':'CREATE_ORDINARY','path':'/core/deviced'},
             {'op':'WRITE','data':(b'\x7f' + bytes(range(256))*16).hex()}, {'op':'CLOSE'},
             {'op':'CREATE_ORDINARY','path':'/history'}, {'op':'WRITE','data':b'old line\n'.hex()}, {'op':'CLOSE'}]
    result = subprocess.run([str(adapter), str(source)], input=''.join(encode(c,i+1) for i,c in enumerate(trace)),
                            text=True, capture_output=True, env=env, check=True)
    lines=result.stdout.splitlines()
    assert len(lines)==len(trace),result.stdout
    for i,(step,line) in enumerate(zip(trace,lines),1):
        observed=response(step,line,i,'mgfs')
        assert observed['status']==0 and observed['pins']==0,observed
    subprocess.run([str(tools['mgfsck']), str(source)], check=True)
    for operation in ['create','history','write','truncate','replace','rename']:
        for case in (['success', 'failed-handoff', 'removal', 'torn', 'flush', 'exhausted'] + (['legacy-control'] if operation=='create' else [])):
            label=operation+'-'+case
            image = folder / (label + '.img'); shutil.copyfile(source, image)
            p = subprocess.run([str(folder / 'test'), str(image), case, operation], env=env, text=True,
                               capture_output=True, timeout=30)
            (folder / (label + '.log')).write_text(p.stdout + p.stderr)
            assert p.returncode == 0, p.stdout + p.stderr
            print(p.stdout, end='')
            if case == 'success': subprocess.run([str(tools['mgfsck']), str(image)], check=True)
            image.unlink() # Expected outcomes checked; preserve current media on unexpected failure.
except BaseException as error:
    try:
        report = workspace.finish({'status': 'FAIL',
                                   'error': f'{type(error).__name__}: {error}'},
                                  verified=False)
        print('Failure evidence:', report.get('failure_evidence', folder),
              file=sys.stderr)
    except BaseException as cleanup_error:
        print(f'Owned workspace retained for review: {folder} ({cleanup_error})',
              file=sys.stderr)
    raise
else:
    workspace.finish({'status': 'PASS'}, verified=True)
    print('MGFS handoff temporary artifacts removed after verification')
