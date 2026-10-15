#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production label management, immutable registry, cache, VFS owners and GPT.
Only host transport, IRQs/parking and syscall authorization are controlled.
"""
from pathlib import Path
import hashlib
import os
import re
import resource
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib
from host_build import dead_strip_flags

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}


def function(source, name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(', source, re.M):
        brace = source.index('{', match.end())
        if ';' in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[match.start():end] + '\n'
    raise AssertionError(name)


with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-label-') as temp:
    directory = Path(temp)
    syscall = (ROOT / 'kernel/src/syscall.c').read_text()
    start = syscall.index('        case SYSCALL_STORAGE_LABEL: {')
    end = syscall.index('        case SYSCALL_STORAGE_GPT_INFO:', start)
    native = r'''
typedef struct { bool storage_management_session; u32 storage_authorized_operation; } label_process_t;
static label_process_t requester;
#define process_t label_process_t
#define process_current label_process_current
static label_process_t *label_process_current(void) { return &requester; }
static bool syscall_is_diskutil(label_process_t *p) { return p == &requester; }
static bool syscall_user_buffer_valid(const void *p, usize n) { return p && n; }
static bool syscall_storage_system_managed(block_device_t *d) {
    if (replace_dispatch) {
        replace_dispatch=false;
        assert(block_unregister_by_id(d->id));
        block_device_t replacement=source;replacement.registered=false;
        assert(block_register(&replacement));source=replacement;
    }
    return false;
}
#define SYSCALL_STORAGE_LABEL 74U
typedef struct { u64 rax,rdi; } label_frame_t;
static void syscall_fail(label_frame_t *f,i64 result) { f->rax=(u64)result; }
'''
    native += function(syscall, 'syscall_fixed_text_valid') + function(syscall, 'syscall_vfs_error')
    native += function(syscall, 'syscall_storage_device_matches')
    native += 'static void label_dispatch(label_frame_t *frame) { switch(frame->rax) {\n' + syscall[start:end] + 'default: assert(0); } }\n#undef process_t\n#undef process_current\n'
    native += r'''
static int native_label(const char *text) {
    mg_storage_label_request_t request={.instance_id=BLOCK_DEVICE_ID_BASE | (device->id+1U)};
    if (device->type == BLOCK_DEVICE_PARTITION) {
        gpt_partition_info_t info; assert(gpt_get_partition_info(device,&info));
        request.parent_instance_id=BLOCK_DEVICE_ID_BASE | (info.parent_id+1U);
    }
    strcpy(request.filesystem,"mgfs"); strcpy(request.label,text);
    requester.storage_management_session=true;requester.storage_authorized_operation=MG_STORAGE_OP_LABEL;
    label_frame_t frame={.rax=SYSCALL_STORAGE_LABEL,.rdi=(u64)(usize)&request};
    label_dispatch(&frame);assert(!requester.storage_authorized_operation);return (int)(i64)frame.rax;
}
'''
    (directory / 'mgfs_label_native.inc').write_text(native)
    binary = directory / 'label'
    subprocess.run([CC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-pthread', *dead_strip_flags(), '-I' + str(directory), '-I' + str(ROOT / 'kernel/include'),
        '-I' + str(ROOT / 'include'), '-idirafter', str(ROOT / 'libc/include'),
        str(ROOT / 'tests/mgfs_label_test.c'), str(ROOT / 'kernel/src/storage/mgfs.c'),
        str(ROOT / 'kernel/src/storage/gpt.c'), str(ROOT / 'kernel/src/block.c'), '-o', str(binary)], check=True)
    for name in ('mkmgfs', 'mgfsck'):
        subprocess.run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
            '-I' + str(ROOT / 'tools'), str(ROOT / ('tools/' + name + '.c')),
            '-o', str(directory / name)], check=True)
    image = directory / 'source.img'
    subprocess.run([str(directory / 'mkmgfs'), '--blocks', '3200', '--uuid',
        '11111111-2222-3333-4444-555555555555', '--format-time-ns', '1', str(image), '--label', 'ORIGINAL'], check=True)
    original = image.read_bytes()
    digest = hashlib.sha256(original).digest()
    output = directory / 'output.img'
    cases = 0
    cpu_seconds = wall_seconds = 0.0

    def run(mode, ordinal=0, amount=0):
        global cases, cpu_seconds, wall_seconds
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        started_cpu = usage.ru_utime + usage.ru_stime
        started_wall = time.monotonic()
        result = subprocess.run([str(binary), str(image), str(output), mode, str(ordinal), str(amount)],
            text=True, capture_output=True, env=ENV, timeout=60)
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu_seconds += usage.ru_utime + usage.ru_stime - started_cpu
        wall_seconds += time.monotonic() - started_wall
        if result.returncode:
            saved = Path(tempfile.mkdtemp(prefix='mangrove-mgfs-label-failure-'))
            shutil.copytree(directory, saved / 'fixture')
            (saved / 'result.log').write_text(result.stdout + result.stderr)
            raise AssertionError(f'{mode}/{ordinal}/{amount}: {result.stdout}{result.stderr}\nArtifacts {saved}')
        cases += 1
        return result.stdout.strip()

    for mode in ('ok', 'clear', 'noop', 'unicode', 'max', 'invalid', 'malformed', 'readonly', 'stale', 'repeat', 'lifetime', 'concurrent', 'exhaust', 'pending', 'reservation', 'finish-race', 'slots', 'ordinary', 'history', 'cancel', 'cancel-finish'):
        print(run(mode))
        if mode in ('ok', 'clear', 'noop', 'unicode', 'max'):
            data = output.read_bytes()
            assert data[:200] == original[:200] and data[288:] == original[288:]
            if mode == 'clear':
                assert data[200:288] == bytes(88)
            else:
                length = struct.unpack_from('<Q', data, 208)[0]
                expected = {'noop': b'ORIGINAL', 'ok': b'NEW', 'unicode': 'Název'.encode(),
                            'max': b'1234567890' * 6 + b'123'}[mode]
                assert data[216:216+length] == expected
            subprocess.run([str(directory / 'mgfsck'), str(output)], check=True)
    for amount in (0, 199, 200, 207, 208, 215, 216, 217, 255, 279, 280, 283, 287, 288, 511, 512, 2048, 4095, 4096):
        run('write', 1, amount)
        if amount in (0, 2048, 4096):
            data = output.read_bytes()
            assert data[:200] == original[:200] and data[288:] == original[288:]
            assert data[216:219] == (b'ORI' if not amount else b'NEW')
            # A failed callback may leave either completely old or completely
            # valid new metadata. Checker success must never resolve its pin.
            subprocess.run([str(directory / 'mgfsck'), str(output)], check=True,
                           stdout=subprocess.DEVNULL)
    for mode in ('flush', 'remove', 'drop', 'corrupt', 'finish', 'finish-remove'):
        print(run(mode, 1))
    for amount in (0, 216, 280, 512):
        run('clear-write', 1, amount)
    print(run('clear-flush', 1))
    print(run('clear-drop', 1))
    for mode in ('read', 'alloc'):
        for ordinal in range(1, 12):
            run(mode, ordinal)
    # Independently encode a conventional GPT with two disjoint MGFS volumes.
    # Runtime partition discovery/mapping and parent-alias I/O are production GPT.
    first = 2048
    sectors = len(original) // 512
    second = first + sectors + 2048
    total = second + sectors + 2048
    disk = bytearray(total * 512)
    entries = bytearray(128 * 128)
    for i, start in enumerate((first, second)):
        at = i * 128
        entries[at:at+16] = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le
        entries[at+16:at+32] = uuid.UUID(int=i+1).bytes_le
        struct.pack_into('<QQ',entries,at+32,start,start+sectors-1)
        name = f'Test{i}'.encode('utf-16le')
        entries[at+56:at+56+len(name)] = name
        disk[start*512:(start+sectors)*512] = original
    def header(primary):
        value = bytearray(512)
        value[:8] = b'EFI PART'
        struct.pack_into('<II',value,8,0x10000,92)
        struct.pack_into('<QQQQ',value,24,1 if primary else total-1,total-1 if primary else 1,34,total-34)
        value[56:72] = uuid.UUID(int=99).bytes_le
        struct.pack_into('<QIII',value,72,2 if primary else total-33,128,128,zlib.crc32(entries))
        struct.pack_into('<I',value,16,zlib.crc32(value[:92]))
        return value
    disk[510:512] = b'\x55\xaa'
    disk[446+4] = 0xee
    struct.pack_into('<II',disk,446+8,1,total-1)
    disk[512:1024] = header(True)
    disk[1024:1024+len(entries)] = entries
    disk[(total-33)*512:(total-1)*512] = entries
    disk[(total-1)*512:] = header(False)
    image.write_bytes(disk)
    print(run('partition'))
    output_volume = directory / 'partition.img'
    output_volume.write_bytes(output.read_bytes()[first*512:(first+sectors)*512])
    subprocess.run([str(directory / 'mgfsck'), str(output_volume)], check=True)
    print(run('partition-fail'))
    print(run('partition-finish'))
    print(run('parent-retire'))
    image.write_bytes(original)
    assert hashlib.sha256(image.read_bytes()).digest() == digest
    print(f'MGFS label: {cases} production management/registry fault cases passed; CPU={cpu_seconds:.3f}s wall={wall_seconds:.3f}s; ASan/UBSan; independent mgfsck; original unchanged')
