#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capacity validation and sparse-file creation without physical devices."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/create_usb_image_file.py'
spec = importlib.util.spec_from_file_location('usb_image_file', TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory(prefix='mangrove-usb-capacity-') as tmp:
    output = Path(tmp) / 'disk.img'
    for invalid in ('0', '-1', '264224', '1.5', 'abc', '0x12345',
                    str(module.MAX_SECTORS + 1)):
        output.write_bytes(b'preserve')
        result = subprocess.run([sys.executable, str(TOOL), '--sectors', invalid,
                                 str(output)], capture_output=True, text=True)
        assert result.returncode != 0 and output.read_bytes() == b'preserve', invalid
    for sectors in (module.MIN_SECTORS, 30629376):
        with output.open('wb') as image:
            image.seek(sectors * 512 - 512)
            image.write(b'stale GPT' + bytes(503))
        module.create_image(output, sectors)
        assert output.stat().st_size == sectors * 512
        with output.open('rb') as image:
            assert image.read(512) == bytes(512)
            image.seek(-512, 2)
            assert image.read() == bytes(512)
        # A capacity-sized file must not allocate its all-zero tail on disk.
        assert output.stat().st_blocks * 512 < output.stat().st_size
    output.unlink()
    output.symlink_to(Path(tmp) / 'missing-target')
    result = subprocess.run([sys.executable, str(TOOL), '--sectors', '264225',
                             str(output)], capture_output=True, text=True)
    assert result.returncode != 0 and not output.resolve().exists()
print('PASS: compact/physical capacities, sparse tail, invalid sizes preserve output, nonregular output rejected')
