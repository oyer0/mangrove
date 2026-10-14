#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Complete production MGFS writes via actual VFS dispatch; controlled hardware."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
compiler = next((a for a in sys.argv[1:] if not a.startswith('--')), 'gcc')
baseline = '--baseline' in sys.argv
ref = '487b9bdd6a536820c2f2ffcddcca691568c7de23'
vfs = (root/'kernel/src/vfs.c').read_text()

def function(name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b'+name+r'\s*\(', vfs, re.M):
        if vfs[match.start():match.end()].lstrip().startswith(('if ', 'if(', 'return ')): continue
        brace = vfs.find('{',match.end())
        if ';' in vfs[match.end():brace]: continue
        depth, end = 1, brace+1
        while depth:
            depth += (vfs[end]=='{')-(vfs[end]=='}'); end += 1
        return vfs[match.start():end]+'\n'
    raise AssertionError(name)

names = ('vfs_handle_valid','vfs_operation_finish_pin','vfs_write_dispatch',
    'vfs_file_write_status','vfs_file_read','vfs_seek','vfs_truncate_dispatch',
    'vfs_resize_handle','vfs_truncate_handle','vfs_file_append_lines')
with tempfile.TemporaryDirectory(prefix='mangrove-mgfs-positional-') as temp:
    directory = Path(temp)
    driver = root/'kernel/src/storage/mgfs.c'
    if baseline:
        driver = directory/'mgfs.c'
        driver.write_bytes(subprocess.check_output(['git','show',ref+':kernel/src/storage/mgfs.c'],cwd=root))
    fixture = (root/'tests/mgfs_stage_cancel_test.c').read_text()
    fixture = fixture.split('\nint main(',1)[0]
    # This older fixture reads one 4 KiB block. Production file readback may
    # issue an aligned extent group; adapt only the disposable media callback.
    start = fixture.index('bool block_read_for_id(')
    end = fixture.index('bool block_write_for_id(',start)
    media_read = fixture[start:end]
    for before, after in (
        ('sectors == 8 && lba % 8 == 0', 'sectors && sectors % 8 == 0 && lba % 8 == 0'),
        ('assert(lba / 8 < FIXTURE_BLOCKS);', 'assert(lba / 8 + sectors / 8 <= FIXTURE_BLOCKS);'),
        ('memcpy(buffer, media[lba / 8], MGFS_BLOCK_BYTES);', 'memcpy(buffer, media[lba / 8], (usize)sectors * 512U);'),
    ):
        assert media_read.count(before) == 1, before
        media_read = media_read.replace(before,after)
    fixture = fixture[:start]+media_read+fixture[end:]
    fixture = fixture.replace('"../kernel/src/storage/mgfs.c"','"'+str(driver)+'"')
    (directory/'mgfs_stage_cancel_test.c').write_text(fixture)
    (directory/'positional_vfs.inc').write_text('#define VFS_FILE_HANDLE_VALID 0x56465348U\n'+''.join(function(n) for n in names))
    source = directory/'test.c'; source.write_bytes((root/'tests/mgfs_positional_write_test.c').read_bytes())
    binary = directory/'positional'
    command = [compiler,'-O1','-std=gnu11','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined',*dead_strip_flags(),
        '-I'+str(root/'kernel/include'),'-I'+str(root/'include'),
        '-idirafter',str(root/'libc/include'),str(source),'-o',str(binary)]
    subprocess.run(command,check=True)
    if baseline:
        result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=90)
        assert result.returncode != 0 and 'facts.size == expected_length' in result.stderr, result.stderr
        print('Desired positional-write invariant FAILS against committed production VFS/MGFS as expected.')
        subprocess.run(command+['-DMGFS_POSITIONAL_BASELINE'],check=True)
    subprocess.run([str(binary)],check=True,timeout=90)
