#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production exFAT Unicode parsing and independently constructed image checks."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags
from exfat_names_image import seed_image

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
with tempfile.TemporaryDirectory(prefix='mangrove-exfat-names-') as directory:
    binary = Path(directory) / 'names'
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections', '-fdata-sections',
                    '-fsanitize=address,undefined', *dead_strip_flags(),
                    '-I'+str(root/'kernel/include'), '-I'+str(root/'include'),
                    '-idirafter', str(root/'libc/include'), str(root/'tests/exfat_names_test.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # Host-only tools run exclusively against the fresh temporary image.
    search = os.environ.get('PATH', '') + os.pathsep + '/usr/sbin'
    mkfs = shutil.which('mkfs.exfat', path=search)
    fsck = shutil.which('fsck.exfat', path=search)
    if not mkfs or not fsck:
        print('SKIP independent exFAT image checks: install exfatprogs (mkfs.exfat and fsck.exfat).')
    else:
        image = Path(directory) / 'names.img'
        with image.open('wb') as stream: stream.truncate(12000 * 512)
        subprocess.run([mkfs, '-c', '4096', str(image)], check=True)
        seed_image(image)
        subprocess.run([fsck, '-n', str(image)], check=True)
        before = hashlib.sha256(image.read_bytes()).digest()
        subprocess.run([str(binary), str(image)], check=True)
        subprocess.run([fsck, '-n', str(image)], check=True)
        assert hashlib.sha256(image.read_bytes()).digest() == before
        print('Independent exFAT checker validation and production readback passed; image bytes unchanged.')
