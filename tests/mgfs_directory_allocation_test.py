#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Imported allocation beyond directory EOF, complete production VFS/MGFS.
Offline fixture encoder and independent mgfsck; no namespace/mutation mocks.
"""
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/mfqs'))
from runner import build, FORMAT_ARGS
from images import create
from mgfs_images import preallocate

CC = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
folder = Path(tempfile.mkdtemp(prefix='mangrove-mgfs-directory-allocation-'))
try:
    adapter, tools = build(folder, CC)
    binary = folder / 'test'
    subprocess.run([CC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DMFQS_ADMISSION_TEST',
        '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'include'), '-idirafter',str(ROOT/'libc/include'),
        '-I'+str(ROOT/'kernel/src/storage'),str(ROOT/'tests/mgfs_directory_allocation_test.c'),
        str(ROOT/'tests/mfqs/host.c'),str(ROOT/'kernel/src/vfs.c'),
        str(ROOT/'libc/src/utf8.c'),'-o',str(binary)],check=True)
    image = folder / 'media.img'
    create(image, 'mgfs', 'empty', tools, FORMAT_ARGS)

    def adapter_run(commands):
        wire = ''.join(f'MFQS1\t{i}\t{op}\t{a}\t{b}\n' for i,(op,a,b) in enumerate(commands,1))
        p = subprocess.run([str(adapter),str(image)],input=wire,text=True,capture_output=True,env=ENV,timeout=60)
        (folder/'adapter.log').write_text(wire+p.stdout+p.stderr)
        assert p.returncode == 0, p.stderr
        rows = [line.split('\t') for line in p.stdout.splitlines()]
        assert len(rows) == len(commands) and all(row[2]=='0' and row[6]=='0' for row in rows), p.stdout
        return rows

    def checker():
        p = subprocess.run([str(tools['mgfsck']),str(image)],capture_output=True,text=True,timeout=30)
        (folder/'checker.log').write_text(p.stdout+p.stderr)
        assert p.returncode == 0 and 'Warnings: 0' in p.stdout, p.stdout+p.stderr

    hx = lambda text: text.encode().hex()
    adapter_run([('MKDIR',hx('/parent'),'-'),('CREATE_ORDINARY',hx('/source'),'-'),
                 ('WRITE','-',hx('KEEP')),('CLOSE','-','-')])
    empty = image.read_bytes()
    # Exact empty-directory reproducer, poisoned contiguous and linked layouts.
    for runs in ((1,), (1,1,1,1), (1,)*130):
        data = bytearray(empty); preallocate(data,2,runs)
        for operation in ('CREATE_ORDINARY','CREATE_CONTENTS','MKDIR'):
            image.write_bytes(data); checker()
            commands = [(operation,hx('/parent/new'), hx('DATA') if operation=='CREATE_CONTENTS' else '-')]
            if operation != 'MKDIR': commands += [('CLOSE','-','-')]
            commands += [('STAT',hx('/parent/new'),'-'),('LIST',hx('/parent'),'-'),('REMOUNT','-','-'),
                         ('STAT',hx('/parent/new'),'-'),('LIST',hx('/parent'),'-')]
            rows = adapter_run(commands)
            assert rows[-1][5] == '1' and rows[-1][7] == hx('new')+'00'
            checker()
            rows = adapter_run([('RMDIR' if operation=='MKDIR' else 'UNLINK',hx('/parent/new'),'-'),
                ('RMDIR',hx('/parent'),'-'),('REMOUNT','-','-'),('LIST','-','-')])
            assert rows[-1][5] == '1' and rows[-1][7] == hx('source')+'00'
            checker()
    image.write_bytes(empty)
    adapter_run([('CREATE_ORDINARY',hx('/parent/keep'),'-'),('WRITE','-',hx('STAY')),('CLOSE','-','-')])
    original = image.read_bytes()
    cases = 9
    outcomes = {}
    max_recovery = max_heap = 0
    def run(data, op, fault='-', ordinal=0, torn=False, remove=False):
        global cases, max_recovery, max_heap
        image.write_bytes(data)
        command = [str(binary),str(image),op,fault,str(ordinal),str(int(torn)),str(int(remove))]
        p = subprocess.run(command,text=True,capture_output=True,env=ENV,timeout=60)
        with (folder/'results.log').open('a') as log: log.write(' '.join(command)+'\n'+p.stdout+p.stderr)
        assert p.returncode == 0, p.stdout+p.stderr
        cases += 1
        values = dict(field.split('=',1) for field in p.stdout.split() if '=' in field)
        outcomes[values['status']] = outcomes.get(values['status'], 0) + 1
        max_recovery = max(max_recovery, int(values['recovery']))
        max_heap = max(max_heap, int(values['peak']))
        if values['uncertain'] == '0': checker()
        return values

    operations = ('create','mkdir','owned','owned-dir','move','exclusive')
    scenarios = [((3,),40), ((1,1,1,1),4080), ((1,1,1),4096),
                 ((1,2,1),8176), ((1,),4080), ((1,)*130,4080), ((1,)*512,4080), ((1,)*128,128*4096-16), ((1,)*130,130*4096-16)]
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    start_cpu = usage.ru_utime + usage.ru_stime
    for runs, size, inline in [(runs,size,2) for runs,size in scenarios] + [((1,1,1,1),4080,0), ((1,1,1),40,1)]:
        data = bytearray(original); preallocate(data,2,runs,size,inline_count=inline)
        image.write_bytes(data); checker()
        for op in operations:
            result = run(data,op)
            assert result['status'] == '0'
            print(f'PASS runs={len(runs)} blocks={sum(runs)} eof={size}: {result}')
    # Every attempted write and flush boundary: definite false, torn write,
    # and removal. Read and allocation preparation failures are also exhaustive
    # for the two-block imported layout and the real-growth layout.
    for runs, size in (((1,1,1,1),4080), ((1,),4080), ((1,)*128,128*4096-16)):
        data = bytearray(original); preallocate(data,2,runs,size)
        for op in operations:
            good = run(data,op)
            fault_types = [('w','writes'),('f','flushes')]
            if len(runs) < 128: fault_types += [('r','reads'),('a','requests')]
            for fault, key in fault_types:
                for ordinal in range(1,int(good[key])+1):
                    for torn, remove in ((False,False), (True,False), (True,True)) if fault=='w' else ((False,False),):
                        run(data,op,fault,ordinal,torn,remove)
            if op=='exclusive':
                canceled = run(data,'cancel')
                assert canceled['status'] != '0'
    # Repeated retirement/re-append and an ordinary-write lifecycle after create.
    data = bytearray(original); preallocate(data,2,(1,1,1,1),4080)
    image.write_bytes(data)
    adapter_run([('CREATE_ORDINARY',hx('/parent/new'),'-'),('WRITE','-',hx('bytes')),('CLOSE','-','-'),
        ('UNLINK',hx('/parent/new'),'-'),('MKDIR',hx('/parent/new'),'-'),('RMDIR',hx('/parent/new'),'-'),
        ('CREATE_CONTENTS',hx('/parent/new'),hx('again')),('CLOSE','-','-'),
        ('RENAME',hx('/parent/new'),hx('/parent/moved')),('REMOUNT','-','-'),
        ('OPEN',hx('/parent/moved'),'-'),('READ','5','-'),('CLOSE','-','-')])
    checker()
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    print(f'Fault outcomes={outcomes}; peak_requested={max_heap}; max_recovery={max_recovery}; ordinary32={int(result['ordinary'])*32}; safe_fixed32={int(result['safe'])*32}')
    print(f'MGFS directory allocation: {cases} production-path cases; CPU={usage.ru_utime+usage.ru_stime-start_cpu:.3f}s; ASan/UBSan; independent mgfsck before/after; poisoned unused entries remain invisible')
except BaseException:
    print('Failure artifacts preserved:',folder, file=sys.stderr)
    raise
else:
    shutil.rmtree(folder)
