#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regression for bounded native Reed guest-controller progress."""

from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests' / 'guest'))
import guest_safe_test as safe


RUN = '0123456789abcdef'

with tempfile.TemporaryDirectory(prefix='mangrove-reed-watchdog-') as directory:
    control = safe.ReedControl(RUN, 'create')
    control.reed_from_shoot = True
    control.shell_active = True
    control.shell_stage = 23
    control.stage = 9
    control.filename_probe_stage = 6
    control.shell_progress = time.monotonic() - 26
    try:
        control.observe(None, 'Username:', Path(directory), 0)
    except TimeoutError as error:
        assert 'shell stage 23' in str(error)
        assert 'editor stage 9' in str(error)
        assert 'filename probe stage 6' in str(error)
    else:
        raise AssertionError('stalled post-Reed workflow was accepted')

print('Reed guest controller: post-editor no-progress fails with stage diagnostics')
