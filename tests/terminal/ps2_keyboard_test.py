#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production i8042/IRQ/keyboard code against a bounded port-I/O model."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
scratch = ROOT / 'build' / 'host-tests'
scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ps2-', dir=os.environ.get('TMPDIR', scratch)) as work:
    temp = Path(work)
    (temp / 'spinlock.h').write_text('''#pragma once
#include <types.h>
typedef struct { volatile u32 value; } spinlock_t;
void spinlock_init(spinlock_t *);
u64 spin_lock_irqsave(spinlock_t *);
void spin_unlock_irqrestore(spinlock_t *,u64);
''')
    binary = temp / 'ps2-test'
    idt = (ROOT / 'kernel/src/idt.c').read_text()
    (temp / 'irq_path.h').write_text(idt[idt.index('void irq_handler('):
                                         idt.index('static void idt_set_gate(')])
    command = [sys.argv[1] if len(sys.argv)>1 else 'cc', '-std=gnu17',
               '-Wall', '-Wextra', '-Werror', '-pthread',
               '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
               '-I', str(temp), '-I', str(ROOT / 'kernel/include'),
               '-I', str(ROOT / 'include'), '-idirafter', str(ROOT / 'libc/include'),
               str(ROOT / 'tests/terminal/ps2_keyboard_test.c'), '-Wl,--gc-sections',
               '-o', str(binary)]
    for diagnostics in (0, 1):
        subprocess.run(command + [f'-DPS2_INPUT_DIAGNOSTICS={diagnostics}',
                                  str(ROOT / 'kernel/src/input_diagnostics.c')], check=True)
        subprocess.run([str(binary)], check=True, timeout=30,
                       env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
