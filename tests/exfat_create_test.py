#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production ordinary exFAT creation and disposable exfatprogs checks."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags
from exfat_names_image import seed_image

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
with tempfile.TemporaryDirectory(prefix='mangrove-exfat-create-') as directory:
    binary = Path(directory) / 'create'
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections', '-fdata-sections',
        '-fsanitize=address,undefined', *dead_strip_flags(),
        '-I'+str(root/'kernel/include'), '-I'+str(root/'include'),
        '-idirafter', str(root/'libc/include'), str(root/'tests/exfat_create_test.c'),
        str(root/'libc/src/utf8.c'),
        '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    search = os.environ.get('PATH', '') + os.pathsep + '/usr/sbin'
    mkfs, fsck = (shutil.which(name, path=search) for name in ('mkfs.exfat', 'fsck.exfat'))
    if not mkfs or not fsck:
        print('SKIP independent exFAT creation: install exfatprogs (mkfs.exfat and fsck.exfat).')
    else:
        for imported in (False, True):
            image = Path(directory) / ('imported.img' if imported else 'fresh.img')
            with image.open('wb') as stream: stream.truncate(12000 * 512)
            subprocess.run([mkfs, '-c', '4096', str(image)], check=True)
            if imported: seed_image(image)
            subprocess.run([fsck, '-n', str(image)], check=True)
            subprocess.run([str(binary), *(['--imported-image'] if imported else []), str(image)], check=True)
            subprocess.run([fsck, '-n', str(image)], check=True)
        print('Independent exfatprogs checks passed for fresh and imported Unicode images.')
