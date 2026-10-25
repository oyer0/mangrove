#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production argument capture and initial native argv mapping, with failures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/src/process.c').read_text()
start = source.index('static int setup_user_stack_args(')
end = source.index('\nbool process_setup_cmdline', start)
installer = source[start:end]
start_legacy = source.index('static bool parse_spawn_cmdline(')
end_legacy = source.index('static int setup_user_stack_args(', start_legacy)
legacy = source[start_legacy:end_legacy]
start_spawn = source.index("static int process_spawn_args_with_context_internal(")
end_spawn = source.index("static bool process_spawn_with_context_internal", start_spawn)
spawn = source[start_spawn:end_spawn]
with tempfile.TemporaryDirectory(prefix='mangrove-argv-') as folder:
    folder = Path(folder)
    fixture = (ROOT / 'tests/kernel/process_arguments_test.c').read_text()
    (folder / 'fixture.c').write_text(fixture.replace('/* INSTALLER */', installer).replace('/* SPAWN */', spawn).replace('/* LEGACY */', legacy))
    executable = folder / 'test'
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'gcc', '-std=gnu17',
        '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'include'),
        '-idirafter', str(ROOT/'libc/include'), str(folder/'fixture.c'),
        str(ROOT/'kernel/src/process_arguments.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
