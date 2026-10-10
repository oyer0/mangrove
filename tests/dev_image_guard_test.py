#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the actual development-image script against disposable files/tool stubs."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mangrove-image-guard-') as directory:
    work = Path(directory)
    tools = work / 'tools'; tools.mkdir()
    for name in ['mkdir', 'dirname']:
        target = Path('/usr/bin') / name
        if not target.exists(): target = Path('/bin') / name
        (tools / name).symlink_to(target)
    disk = work / 'disk.img'; disk.write_bytes(b'preserve disk')
    root = work / 'root.img'; root.write_bytes(b'preserve root')
    def run():
        result = subprocess.run(['/bin/sh', str(ROOT / 'scripts/update_dev_image.sh'),
                                 '--disk', str(disk), '--root', str(root)],
                                cwd=work, env={**os.environ, 'PATH': str(tools)},
                                capture_output=True, text=True)
        assert result.returncode != 0
        assert disk.read_bytes() == b'preserve disk' and root.read_bytes() == b'preserve root'
        return result.stdout + result.stderr
    assert 'lsof is unavailable' in run()
    lsof = tools / 'lsof'; lsof.write_text('#!/bin/sh\nexit 0\n'); lsof.chmod(0o755)
    assert 'disk is in use' in run()
    lsof.write_text('#!/bin/sh\nexit 1\n')
    wc = tools / 'wc'; wc.write_text('#!/bin/sh\necho 1\n'); wc.chmod(0o755)
    assert 'Invalid persistent development disk size' in run()
print('development image guard: missing lsof, open image and idle image paths passed; fixtures preserved')
