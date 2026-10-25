#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production editor/buffer large-document and measured-work fixture."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/shared'))
from host_build import dead_strip_flags
with tempfile.TemporaryDirectory(prefix='mangrove-reed-large-') as temp:
    binary=Path(temp)/'test'
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-O2','-std=gnu17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fno-builtin','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
        '-I'+str(ROOT/'libc/include'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'userspace/common'),
        str(ROOT/'tests/userspace/reed_large_document_test.c'),str(ROOT/'userspace/reed/buffer.c'),str(ROOT/'userspace/reed/config.c'),str(ROOT/'libc/src/clipboard.c'),str(ROOT/'userspace/clipd/clipboard.c'),str(ROOT/'libc/src/utf8.c'),*dead_strip_flags(),'-Wl,--wrap=malloc','-o',str(binary)],check=True)
    subprocess.run([str(binary),'baseline'],check=True)
    subprocess.run([str(binary)],check=True)
