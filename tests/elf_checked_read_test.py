#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production ELF/VFS/MGFS read status, controlled paging and block faults."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mangrove-elf-read-') as temporary:
    folder=Path(temporary)
    source=(ROOT/'kernel/src/syscall.c').read_text()
    function=re.search(r'static i64 syscall_spawn_error\(int result\)\n\{.*?\n\}',source,re.S)
    assert function
    (folder/'elf_native_error.inc').write_text(function[0])
    binary=folder/'test';image=folder/'media.img'
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DMFQS_ADMISSION_TEST',
        '-I'+str(folder),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel/src'),'-I'+str(ROOT/'include'),
        '-idirafter',str(ROOT/'libc/include'),'-I'+str(ROOT/'tests'),str(ROOT/'tests/elf_checked_read_test.c'),
        str(ROOT/'kernel/src/vfs.c'),str(ROOT/'tests/mfqs/host.c'),'-o',str(binary)],check=True)
    subprocess.run([str(ROOT/'build/mkmgfs'),'--blocks','3200','--uuid','11111111-2222-3333-4444-555555555555',
        '--format-time-ns','1',str(image)],check=True)
    subprocess.run([str(binary),str(image)],check=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    subprocess.run([str(ROOT/'build/mgfsck'),str(image)],check=True)
