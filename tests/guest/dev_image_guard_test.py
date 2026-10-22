#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise development-image guards using only disposable media fixtures."""
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def fixture(minor):
    block = bytearray(4096)
    block[:8] = b'MGFSv1\0\0'
    struct.pack_into('<QQ', block, 8, 1, minor)
    return bytes(block)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            result.update(chunk)
    return result.digest()


with tempfile.TemporaryDirectory(prefix='mangrove-image-guard-') as directory:
    work = Path(directory)
    tools = work / 'tools'; tools.mkdir()
    for name in ('mkdir', 'dirname', 'head', 'grep', 'wc'):
        target = Path('/usr/bin') / name
        if not target.exists(): target = Path('/bin') / name
        (tools / name).symlink_to(target)
    (tools / 'python3').symlink_to(sys.executable)
    env = {**os.environ, 'PATH': str(tools)}
    disk = work / 'disk.img'
    root = work / 'root.img'
    disk.write_bytes(b'preserve disk')
    root.write_bytes(b'preserve root')

    def update():
        return subprocess.run(['/bin/sh', str(ROOT / 'scripts/update_dev_image.sh'),
                               '--disk', str(disk), '--root', str(root)],
                              cwd=ROOT, env=env, capture_output=True, text=True)

    def assert_preserved(result, disk_before=None, root_before=None):
        assert result.returncode != 0
        if disk_before is not None: assert disk.read_bytes() == disk_before
        if root_before is not None: assert root.read_bytes() == root_before
        return result.stdout + result.stderr

    assert 'lsof is unavailable' in assert_preserved(update(), b'preserve disk', b'preserve root')
    lsof = tools / 'lsof'; lsof.write_text('#!/bin/sh\nexit 0\n'); lsof.chmod(0o755)
    assert 'disk is in use' in assert_preserved(update(), b'preserve disk', b'preserve root')
    lsof.write_text('#!/bin/sh\nexit 1\n')
    root.unlink()
    assert 'Invalid persistent development disk size' in assert_preserved(
        update(), b'preserve disk')

    disk.unlink()
    old = fixture(1)
    root.write_bytes(old)
    assert 'image preserved' in assert_preserved(update(), root_before=old)
    make = subprocess.run(['/bin/sh', str(ROOT / 'scripts/make_image.sh'),
                           '--root', str(root)], cwd=ROOT, env=env,
                          capture_output=True, text=True)
    assert 'image preserved' in assert_preserved(make, root_before=old)

    root.unlink()
    disk.write_bytes(old)
    assert 'image preserved' in assert_preserved(update(), disk_before=old)
    assert not root.exists()

    # A valid GPT root partition can be located through production GPT parsing.
    # Its old-minor contents must be rejected before extract can replace root.
    disk_size = 264225 * 512
    with disk.open('wb') as output:
        output.truncate(disk_size)
    header = bytearray(512)
    header[:8] = b'EFI PART'
    struct.pack_into('<I', header, 12, 92)
    struct.pack_into('<Q', header, 32, 264224)
    struct.pack_into('<QQ', header, 40, 34, 264191)
    struct.pack_into('<QII', header, 72, 2, 128, 128)
    entry = bytearray(128)
    entry[:16] = b'\x01' * 16
    struct.pack_into('<QQ', entry, 32, 133120, 264191)
    entry[56:56 + len('MANGROVE_ROOT'.encode('utf-16le'))] = 'MANGROVE_ROOT'.encode('utf-16le')
    with disk.open('r+b') as output:
        output.seek(512); output.write(header)
        output.seek(2 * 512); output.write(entry)
        output.seek(133120 * 512); output.write(old)
    before_digest = digest(disk)
    result = update()
    assert 'image preserved' in assert_preserved(result)
    assert digest(disk) == before_digest
    with disk.open('rb') as input_file:
        input_file.seek(133120 * 512)
        assert input_file.read(4096) == old
    assert not root.exists()

print('development image guard: early legacy rejection preserved disposable raw and GPT images')
