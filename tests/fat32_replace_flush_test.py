#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production FAT32 replacement barriers, sync and disposable interoperability."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
with tempfile.TemporaryDirectory(prefix='mangrove-fat32-flush-') as temporary:
    directory = Path(temporary); binary = directory/'flush'
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections', '-fdata-sections',
        '-fsanitize=address,undefined', *dead_strip_flags(), '-I'+str(root/'kernel/include'),
        '-I'+str(root/'include'), '-idirafter', str(root/'libc/include'),
        str(root/'tests/fat32_replace_flush_test.c'),
        str(root/'libc/src/utf8.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    search = os.environ.get('PATH', '')+os.pathsep+'/usr/sbin'
    tools = {name: shutil.which(name, path=search) for name in ('mkfs.fat', 'fsck.fat', 'mcopy', 'mdir')}
    if not all(tools.values()):
        print('SKIP independent FAT32 checks: missing '+', '.join(n for n,p in tools.items() if not p))
    else:
        original = directory/'original.img'
        with original.open('wb') as stream: stream.truncate(64*1024*1024)
        subprocess.run([tools['mkfs.fat'], '-F', '32', '-s', '1', str(original)], check=True)
        old = directory/'old.bin'; old.write_bytes((b'OLD'*234)[:700])
        neighbor = directory/'neighbor.bin'; neighbor.write_bytes(b'N'*1024)
        for path, name in ((old, 'TARGET.TXT'), (neighbor, 'NEIGHBOR.TXT')):
            subprocess.run([tools['mcopy'], '-i', str(original), str(path), '::'+name], check=True)
        subprocess.run([tools['fsck.fat'], '-n', str(original)], check=True)
        original_hash = hashlib.sha256(original.read_bytes()).digest()
        for boundary in range(5):
            image = directory/f'boundary-{boundary}.img'; shutil.copyfile(original, image)
            if not boundary:
                subprocess.run([str(binary), '--image', str(image)], check=True)
                names = [('TARGET.TXT', 512*1024+1), ('CREATED.BIN', 512*1024+7),
                         ('Résumé from Mangrove.txt', 3)]
            else:
                subprocess.run([str(binary), '--image-fail', str(boundary), str(image)], check=True)
                names = [('TARGET.TXT', 700 if boundary in (1,4) else 512*1024+1)]
            for name, length in names:
                output = directory/'readback.bin'
                subprocess.run([tools['mcopy'], '-o', '-i', str(image), '::'+name, str(output)], check=True)
                expected = old.read_bytes() if boundary in (1,4) else bytes(i & 255 for i in range(length))
                assert output.read_bytes() == expected, (boundary, name)
            output = directory/'neighbor-readback.bin'
            subprocess.run([tools['mcopy'], '-o', '-i', str(image), '::NEIGHBOR.TXT', str(output)], check=True)
            assert output.read_bytes() == neighbor.read_bytes()
            subprocess.run([tools['mdir'], '-i', str(image), '::'], check=True)
            result = subprocess.run([tools['fsck.fat'], '-n', str(image)], check=False)
            if boundary in (1,2):
                assert result.returncode in (0,1), result.returncode
                print(f'Failed flush {boundary}: issued-write image inspected; unreachable allocation may need repair, runtime remains uncertain.')
            else:
                assert result.returncode == 0, (boundary, result.returncode)
                if boundary: print(f'Failed flush {boundary}: checker-clean issued-write image still returned uncertainty and retained its pin.')
        assert original_hash == hashlib.sha256(original.read_bytes()).digest()
        print('Independent mkfs.fat/mtools/fsck.fat success and uncertain-image observations passed; original unchanged.')
