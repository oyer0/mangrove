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
import guest_mgfs_test as guest


RUN = '0123456789abcdef'

assert guest.native_test_failure('FSTEST1 failed pid=27 status=43\ndeveloper ~:') == \
    'FSTEST1 failed pid=27 status=43'
for text in ('developer ~:', 'FSTEST1 failed pid=27 status=0',
             'quoted FSTEST1 failed pid=27 status=43',
             'FSTEST1 failed pid=0 status=43'):
    assert guest.native_test_failure(text) is None

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
