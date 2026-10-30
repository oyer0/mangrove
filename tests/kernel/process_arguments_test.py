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
workspace_parent = Path(os.environ.get('TMPDIR') or
                        ROOT / 'build' / 'test-workspaces')
workspace_parent.mkdir(parents=True, exist_ok=True)

def extract_function(prefix):
    start = source.index(prefix)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

installer = (extract_function('static int setup_user_stack_args(') +
             extract_function('bool process_setup_argv('))
legacy = extract_function('static bool parse_spawn_cmdline(')
spawn = (extract_function('static int process_arguments_executable_path(') +
         extract_function('bool process_publish_launch(') +
         extract_function('static int process_spawn_args_with_context_internal(') +
         extract_function('static int process_spawn_kernel_argv_with_context(') +
         extract_function('int process_spawn_session_shell('))
with tempfile.TemporaryDirectory(prefix='mangrove-argv-',
                                 dir=workspace_parent) as folder:
    folder = Path(folder)
    fixture = (ROOT / 'tests/kernel/process_arguments_test.c').read_text()
    (folder / 'fixture.c').write_text(fixture.replace('/* INSTALLER */', installer).replace('/* SPAWN */', spawn).replace('/* LEGACY */', legacy))
    executable = folder / 'test'
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'gcc', '-std=gnu17',
        '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-I'+str(ROOT/'tests/kernel'), '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'include'),
        '-idirafter', str(ROOT/'libc/include'), str(folder/'fixture.c'),
        str(ROOT/'kernel/src/process_arguments.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
