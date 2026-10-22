#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Full production mount proof, VFS recovery, cursor and management admission.
Independent corruption encoder only prepares offline disposable images.
No mutation/namespace logic is replaced. Single-threaded lock dependencies.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import resource

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/filesystems/mfqs'))
from runner import build

CC = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
U64 = lambda b, at: struct.unpack_from('<Q', b, at)[0]


def crc(data, offset):
    value = 0
    for i, byte in enumerate(data):
        value ^= (0 if offset <= i < offset + 8 else byte) << 56
        for _ in range(8):
            value = ((value << 1) ^ (0x42f0e1eba9ea3693 if value >> 63 else 0)) & ((1 << 64) - 1)
    return value


def seal(data, at, length, offset):
    struct.pack_into('<Q', data, at + offset, crc(data[at:at + length], offset))


with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-admission-') as temp:
    directory = Path(temp)
    adapter, tools = build(directory, CC)
    image = directory / 'base.img'
    subprocess.run([str(tools['mkmgfs']), '--help'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Exact formatter invocation is shared with the existing independent fixture.
    from images import create
    from runner import FORMAT_ARGS
    create(image, 'mgfs', 'empty', tools, FORMAT_ARGS)
    commands = [('CREATE_ORDINARY','2f61','-'), ('WRITE','-', '41'*8192), ('CLOSE','-','-'),
                ('CREATE_ORDINARY','2f62','-'), ('WRITE','-', '42'*8192), ('CLOSE','-','-'),
                ('CREATE_ORDINARY','2f68','-'), ('WRITE','-', '6f6c640a'), ('CLOSE','-','-'),
                ('MKDIR','2f656d707479','-')]
    wire = ''.join(f'MFQS1\t{i}\t{op}\t{a}\t{b}\n' for i,(op,a,b) in enumerate(commands,1))
    result = subprocess.run([str(adapter),str(image)], input=wire, text=True, capture_output=True, env=ENV, check=True)
    assert all(row.split('\t')[2]=='0' for row in result.stdout.splitlines()), result.stdout
    original = image.read_bytes()
    binary = directory / 'proof'
    subprocess.run([CC,'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-DMFQS_ADMISSION_TEST',
        '-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),
        '-I'+str(ROOT/'kernel/src/storage'),str(ROOT/'tests/filesystems/mgfs/mgfs_mount_ownership_test.c'),
        str(ROOT/'tests/filesystems/mfqs/host.c'),str(ROOT/'kernel/src/vfs.c'),
        str(ROOT/'libc/src/utf8.c'),'-o',str(binary)],check=True)
    cases = 0
    cpu = 0.0
    wall = 0.0

    def run(operation, fault='-', ordinal=0, remove=False, data=original):
        global cases, cpu, wall
        image.write_bytes(data)
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        start_cpu = usage.ru_utime + usage.ru_stime
        start_wall = time.monotonic()
        p = subprocess.run([str(binary),str(image),operation,fault,str(ordinal),str(int(remove))],
                           text=True,capture_output=True,env=ENV,timeout=60)
        if p.returncode:
            # Preserve the first failing fixture, transport and sanitizer output.
            failed = Path(tempfile.mkdtemp(prefix='mangrove-mgfs-admission-failure-'))
            shutil.copy(image,failed/'media.img')
            (failed/'result.log').write_text(p.stdout+p.stderr)
            (failed/'replay.txt').write_text(f'{operation} {fault} {ordinal} {int(remove)}\n')
            raise AssertionError(f'{operation}/{fault}/{ordinal}: {p.stderr}\nArtifacts {failed}')
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu += usage.ru_utime + usage.ru_stime - start_cpu
        wall += time.monotonic() - start_wall
        cases += 1
        return p.stdout.strip()

    print(run('cursor'))
    print(run('budget'))
    print(run('exhaust'))
    print(run('identity'))
    table = U64(original,136)*4096
    records = {}
    for block in range(U64(original,144)):
        for slot in range(21):
            at=table+block*4096+24+slot*192
            if U64(original,at) in (1,2) and U64(original,at+16): records[U64(original,at+16)]=at
    root=records[1]; a=records[2]; b=records[3]
    root_at=U64(original,root+72)*4096
    entries=[]; offset=0
    while offset<U64(original,root+32):
        at=root_at+offset; n=U64(original,at+8); length=(32+n+7)&~7
        entries.append((at,length)); offset+=length
    assert [original[at+32:at+32+U64(original,at+8)] for at,_ in entries]==[b'a',b'b',b'h',b'empty']

    def record_seal(data, at):
        seal(data,at,192,184); seal(data,at//4096*4096,4096,16)

    shared=bytearray(original)
    shared[b+40:b+128]=shared[a+40:a+128]
    record_seal(shared,b)
    print(run('reject',data=shared))
    diagnosed = subprocess.run([str(tools['mgfsck']), str(image)], capture_output=True, text=True)
    assert diagnosed.returncode == 1 and 'multiply owned' in diagnosed.stderr
    print(run('recovery',data=shared))
    # Same Record referenced twice, no allocation overlap: independent root cause.
    for target in (2,1,999999):
        data=bytearray(original); at,length=entries[1]
        struct.pack_into('<Q',data,at,target); seal(data,at,length,24)
        run('reject',data=data)
    data=bytearray(original); at,length=entries[1];data[at+32]=ord('a');seal(data,at,length,24)
    run('reject',data=data)
    # Unreachable allocated Record still participates in ownership proof.
    data=bytearray(shared);at,length=entries[1];struct.pack_into('<Q',data,at+16,2);seal(data,at,length,24)
    run('reject',data=data)
    # Allocation beyond EOF is owned even when the logical contents are empty.
    data=bytearray(shared);struct.pack_into('<Q',data,b+32,0);record_seal(data,b)
    run('reject',data=data)
    # A referenced allocation bit cannot be free, and directory/file sharing is unsafe.
    data=bytearray(original); first=U64(data,a+72);bit=first-U64(data,160);bitmap=U64(data,104)*4096
    data[bitmap+24+bit//8]&=~(1<<(bit%8));seal(data,bitmap,4096,16);run('reject',data=data)
    data=bytearray(original);struct.pack_into('<Q',data,b+72,U64(data,root+72));record_seal(data,b)
    run('reject',data=data)

    data=bytearray(original)
    for at,length in entries[:2]:
        struct.pack_into('<Q',data,at+16,2);seal(data,at,length,24)
    for record,target,name in ((a,3,b'b'),(b,2,b'a')):
        struct.pack_into('<Q',data,record,2)
        struct.pack_into('<Q',data,record+32,40)
        struct.pack_into('<Q',data,record+88,2) # directory metadata extent flag
        child=U64(data,record+72)*4096
        data[child:child+40]=bytes(40)
        struct.pack_into('<QQQ',data,child,target,1,1)
        data[child+32:child+33]=name
        seal(data,child,40,24);record_seal(data,record)
    run('reject',data=data)

    # Valid unreachable allocated objects remain owned and are not repaired.
    data=bytearray(original);at,length=entries[1]
    struct.pack_into('<Q',data,at+16,2);seal(data,at,length,24)
    print(run('admit',data=data))
    # Independent formatter plus offline format encoding: 31.25 MiB of data
    # in 8,000 ascending fragmented extents, with 64 list blocks.
    larger=directory/'larger.img'
    args=FORMAT_ARGS.copy();args[args.index('3200')]='16384'
    create(larger,'mgfs','empty',tools,args)
    formatted=larger.read_bytes()
    data=bytearray(formatted);ds=U64(data,160);rt=U64(data,136)*4096
    ab=U64(data,104)*4096;rb=U64(data,120)*4096
    def allocated(block):
        bit=block-ds; data[ab+24+bit//8]|=1<<(bit%8)
    def put_record(identifier,kind,size,extents,list_head=0):
        slot=identifier-1;at=rt+(slot//21)*4096+24+(slot%21)*192
        record=bytearray(192)
        struct.pack_into('<QQQQQQQQ',record,0,kind,U64(data,rt+24+8),identifier,1,
                         size,len(extents),min(2,len(extents)),list_head)
        for i,(logical,physical,blocks) in enumerate(extents[:2]):
            struct.pack_into('<QQQQ',record,64+i*32,logical,physical,blocks,1 if kind==1 else 2)
        data[at:at+192]=record;seal(data,at,192,184)
        data[rb+24+slot//8]|=1<<(slot%8)
        return at
    def entry(at,target,name):
        size=(32+len(name)+7)&~7
        data[at:at+size]=bytes(size)
        struct.pack_into('<QQQ',data,at,target,len(name),1)
        data[at+32:at+32+len(name)]=name;seal(data,at,size,24)
        return size
    allocated(ds)
    extents=[(i,ds+2+i*2,1) for i in range(8000)]
    lists=ds+16002
    for logical,physical,blocks in extents:allocated(physical)
    remaining=extents[2:];pages=(len(remaining)+125)//126
    for i in range(pages):
        block=lists+i;allocated(block);at=block*4096
        subset=remaining[i*126:(i+1)*126]
        struct.pack_into('<QQQQQ',data,at,0x315458455346474D,2,block+1 if i+1<pages else 0,len(subset),0)
        for n,(logical,physical,blocks) in enumerate(subset):
            struct.pack_into('<QQQQ',data,at+64+n*32,logical,physical,blocks,1)
        seal(data,at,4096,32)
    put_record(2,1,8000*4096,extents,lists)
    size=entry(ds*4096,2,b'large');put_record(1,2,size,[(0,ds,1)])
    struct.pack_into('<Q',data,96,3);seal(data,0,200,192)
    for block in range(U64(data,144)):seal(data,rt+block*4096,4096,16)
    seal(data,ab,4096,16);seal(data,rb,4096,16)
    print(run('large',data=data))
    subprocess.run([str(tools['mgfsck']),str(image)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    # Deep namespace traversal is iterative, includes every allocated Record,
    # and does not consume one kernel frame per directory.
    data=bytearray(formatted);depth=256
    for identifier in range(1,depth+1):
        block=ds+identifier-1;allocated(block)
        size=entry(block*4096,identifier+1,b'n') if identifier<depth else 0
        put_record(identifier,2,size,[(0,block,1)])
    struct.pack_into('<Q',data,96,depth+1);seal(data,0,200,192)
    for block in range(U64(data,144)):seal(data,rt+block*4096,4096,16)
    seal(data,ab,4096,16);seal(data,rb,4096,16)
    print(run('admit',data=data))
    print(run('deep-move',data=data))
    subprocess.run([str(tools['mgfsck']),str(image)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)

    operations=('write','grow','truncate','history','unlink','rmdir','create','mkdir',
                'owned','owned-dir','rename','move','replace','exclusive','cancel')
    for operation in operations:
        success=run(operation)
        print(success)
        # Independent checker verifies every confirmed complete production outcome.
        subprocess.run([str(tools['mgfsck']),str(image)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        values=dict(item.split('=',1) for item in success.split() if '=' in item)
        for fault,metric in (('w','writes'),('f','flushes'),('a','requests'),('r','reads')):
            for ordinal in range(1,int(values[metric])+1):
                run(operation,fault,ordinal)
        run(operation,'w',1,True)
    # Every mount read/heap preparation boundary: no media writes or leaked state.
    # Bound from the complete production state, not duplicated parser behavior.
    for fault in ('a','r'):
        for ordinal in range(1,100):
            if run('mount-fault',fault,ordinal).startswith('NOFAULT'): break
        else: raise AssertionError('mount fixture exceeded bounded fault coverage')
    run('mount-fault','r',3,True)
    print(f'MGFS full-production admission/recovery: {cases} cases passed; CPU={cpu:.3f}s wall={wall:.3f}s; ASan/UBSan; disposable mgfsck')
