#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checked namespace failures and a historical busy-CWD bypass, full production units."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tests/filesystems/mfqs')]
from images import create, external_tools, check, Unavailable
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags


def main():
    work = Path(tempfile.mkdtemp(prefix='mangrove-lookup-'))
    compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
    try:
        baseline = work / 'historical-vfs.c'
        baseline.write_bytes(subprocess.check_output(['git', 'show',
            '707a060a2332b7c343639ba721da39fd647fc27e:kernel/src/vfs.c'], cwd=ROOT))
        for backend in ('mgfs', 'fat32', 'exfat'):
            tools = {'mkmgfs': ROOT / 'build/mkmgfs', 'mgfsck': ROOT / 'build/mgfsck'}
            if backend != 'mgfs': tools.update(external_tools(backend))
            for historical in (False, True):
                name = backend + ('-historical' if historical else '')
                image, binary = work / (name + '.img'), work / name
                create(image, backend, 'empty', tools, ['--blocks', '3200', '--uuid',
                       '11111111-2222-3333-4444-555555555555', '--format-time-ns', '1'])
                check(image, backend, tools)
                command = [compiler, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections', '-fdata-sections',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DMFQS_ADMISSION_TEST',
                    '-DTEST_' + backend.upper(), *(['-DLOOKUP_HISTORICAL'] if historical else []),
                    '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel/src'),
                    '-I' + str(ROOT / 'include'), '-idirafter', str(ROOT / 'libc/include'),
                    '-I' + str(ROOT / 'tests/filesystems'), str(ROOT / 'tests/filesystems/vfs/vfs_lookup_test.c'),
                    str(ROOT / 'tests/filesystems/mfqs/host.c'), str(baseline if historical else ROOT / 'kernel/src/vfs.c'),
                    str(ROOT / 'libc/src/utf8.c'),
                    *([str(ROOT / 'libc/src/time_convert.c')] if backend == 'exfat' else []),
                    *dead_strip_flags(), '-o', str(binary)]
                subprocess.run(command, check=True, timeout=120)
                result = subprocess.run([str(binary), str(image)], text=True, capture_output=True,
                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}, timeout=60)
                (work / (name + '.log')).write_text(result.stdout + result.stderr)
                if result.returncode or result.stderr:
                    raise AssertionError((name, result.returncode, result.stdout, result.stderr))
                assert ('historical: false NOT_FOUND and deleted live CWD' if historical else
                        'checked lookup: IO/NOT_FOUND/NO_MEM/DEVICE_GONE') in result.stdout
                print(result.stdout.strip(), flush=True)
                check(image, backend, tools)
    except Unavailable as error:
        print('INCOMPLETE ' + str(error)); return 2
    except BaseException:
        print('Lookup failure evidence retained:', work, file=sys.stderr); raise
    else:
        print('Independent images, ASan/UBSan and historical controls PASS; controlled block/heap/scheduler.')
        shutil.rmtree(work)
    return 0


if __name__ == '__main__':
    sys.exit(main())
