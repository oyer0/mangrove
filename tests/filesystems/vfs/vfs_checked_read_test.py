#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production checked reads with independently formatted disposable media."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/filesystems/mfqs'))
from images import create, external_tools, check
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags

with tempfile.TemporaryDirectory(prefix='mangrove-checked-read-') as work:
    work = Path(work)
    for backend in ('mgfs', 'fat32', 'exfat'):
        image, binary = work / (backend + '.img'), work / backend
        tools = {'mkmgfs': ROOT / 'build/mkmgfs', 'mgfsck': ROOT / 'build/mgfsck'}
        if backend != 'mgfs':
            tools.update(external_tools(backend))
        create(image, backend, 'empty', tools, ['--blocks', '3200', '--uuid',
               '11111111-2222-3333-4444-555555555555', '--format-time-ns', '1'])
        compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-DMFQS_ADMISSION_TEST', '-DTEST_' + backend.upper(),
            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel/src'),
            '-I' + str(ROOT / 'include'), '-idirafter', str(ROOT / 'libc/include'),
            '-I' + str(ROOT / 'tests/filesystems'), str(ROOT / 'tests/filesystems/vfs/vfs_checked_read_test.c'),
            str(ROOT / 'tests/filesystems/mfqs/host.c'), str(ROOT / 'kernel/src/vfs.c'),
            str(ROOT / 'libc/src/utf8.c'),
            *([str(ROOT / 'libc/src/time_convert.c')] if backend == 'exfat' else []),
            *dead_strip_flags(), '-o', str(binary)], check=True)
        uncertain=work/(backend+'-uncertain.img')
        shutil.copyfile(image,uncertain)
        subprocess.run([str(binary), str(image)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        check(image, backend, tools)
        subprocess.run([str(binary), str(uncertain), 'uncertain'], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
