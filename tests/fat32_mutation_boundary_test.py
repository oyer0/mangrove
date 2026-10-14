#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check production symbol reachability and ownership-validated mutation hooks."""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
removed = ('fat32_alloc_cluster', 'fat32_extend_chain', 'fat32_free_chain',
           'fat32_set_next_cluster', 'fat32_zero_cluster',
           'fat32_invalidate_fsinfo', 'fat32_release_unpublished')
for path in (root/'kernel/include/storage/fat32.h', root/'kernel/src/storage/fat32.c',
             root/'kernel/src/init.c'):
    text = path.read_text()
    assert not any(re.search(r'\b'+name+r'\b', text) for name in removed), path
includes = ['-I'+str(root/'kernel/include'), '-I'+str(root/'include')]
with tempfile.TemporaryDirectory(prefix='mangrove-fat32-boundary-') as temporary:
    directory = Path(temporary)
    driver = directory/'fat32.o'
    subprocess.run([compiler, '-std=gnu11', '-ffreestanding', *includes,
                    '-I'+str(root/'libc/include'), '-c',
                    str(root/'kernel/src/storage/fat32.c'), '-o', str(driver)], check=True)
    symbols = subprocess.check_output(['nm', '-g', str(driver)], text=True)
    exported = {fields[-1].removeprefix('_') for line in symbols.splitlines()
                if len(fields := line.split()) == 3 and fields[-2] != 'U'}
    expected = {'fat32_init', 'fat32_read_label', 'fat32_label_valid',
                'fat32_set_label', 'fat32_set_label_for_id', 'fat32_get_cluster_link',
                'fat32_ownership_preflight_locked'}
    assert exported == expected, exported
    # Declarations must be absent too; tests cannot accidentally keep a bypass
    # alive merely because normal target link dead-strips it.
    for name in removed[:3]:
        probe = directory/(name+'.c')
        probe.write_text('#include <storage/fat32.h>\nvoid *probe = (void *)&'+name+';\n')
        result = subprocess.run([compiler, *includes, '-I'+str(root/'libc/include'),
                                 '-c', str(probe), '-o', str(directory/'probe.o')],
                                capture_output=True, text=True)
        assert result.returncode and name in result.stderr, result.stderr
    binary = directory/'boundary'
    subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections',
                    '-fdata-sections', '-fsanitize=address,undefined', *dead_strip_flags(),
                    *includes, '-idirafter', str(root/'libc/include'),
                    str(root/'tests/fat32_mutation_boundary_test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    tools = {name: shutil.which(name, path=os.environ.get('PATH', '')+os.pathsep+'/usr/sbin')
             for name in ('mkfs.fat', 'fsck.fat', 'mcopy', 'mdir')}
    if not all(tools.values()):
        print('Independent FAT32 retirement not run; missing: '+
              ', '.join(name for name, tool in tools.items() if not tool))
    else:
        retirement = directory/'retirement'
        subprocess.run([compiler, '-O1', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-type-limits', '-ffunction-sections',
                        '-fdata-sections', '-fsanitize=address,undefined', *dead_strip_flags(),
                        *includes, '-idirafter', str(root/'libc/include'),
                        str(root/'tests/fat32_retirement_test.c'), '-o', str(retirement)], check=True)
        original = directory/'original.img'
        with original.open('wb') as stream:
            stream.truncate(64*1024*1024)
        subprocess.run([tools['mkfs.fat'], '-F', '32', '-s', '1', str(original)], check=True)
        payload = directory/'payload'
        payload.write_bytes(b'x'*(2*1024*1024+1))
        long_name = 'Long file retirement '+'x'*200+'.txt'
        for name in ('LARGE.BIN', long_name):
            subprocess.run([tools['mcopy'], '-i', str(original), str(payload), '::'+name], check=True)
        neighbor = directory/'neighbor'
        neighbor.write_bytes(b'Preserve unrelated contents')
        subprocess.run([tools['mcopy'], '-i', str(original), str(neighbor), '::KEEP.TXT'], check=True)
        subprocess.run([tools['fsck.fat'], '-n', str(original)], check=True)
        digest = hashlib.sha256(original.read_bytes()).digest()
        updated = directory/'updated.img'
        shutil.copyfile(original, updated)
        subprocess.run([str(retirement), '--image', str(updated), 'LARGE.BIN', long_name], check=True)
        subprocess.run([tools['fsck.fat'], '-n', str(updated)], check=True)
        listing = subprocess.check_output([tools['mdir'], '-i', str(updated), '::'], text=True)
        assert 'KEEP' in listing and 'LARGE' not in listing and long_name not in listing, listing
        readback = directory/'readback'
        subprocess.run([tools['mcopy'], '-i', str(updated), '::KEEP.TXT', str(readback)], check=True)
        assert readback.read_bytes() == neighbor.read_bytes()
        assert hashlib.sha256(original.read_bytes()).digest() == digest
        print('Independent FAT32: large short/LFN retirement, neighbor readback, fsck and unchanged source passed.')
print('FAT32 production exports, removed declarations and optional boot call sites verified.')
