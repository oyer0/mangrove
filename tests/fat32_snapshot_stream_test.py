#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production FAT32 forward streaming, historical amplification and independent media."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests/mfqs'))
from images import external_tools
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
BASELINE='b3bccfc9715bd80f47f4b8b976ea4a385d52973a'
with tempfile.TemporaryDirectory(prefix='mangrove-fat32-snapshot-') as temp:
    folder=Path(temp)
    def build(historical=False):
        fixture=ROOT/'tests/fat32_mount_identity_test.c'
        if historical:
            old=folder/'old-fat32.c';old.write_bytes(subprocess.check_output(['git','show',BASELINE+':kernel/src/storage/fat32.c'],cwd=ROOT))
            copied=folder/'old-fixture.c';copied.write_text(fixture.read_text().replace('"../kernel/src/storage/fat32.c"','"'+str(old)+'"'));fixture=copied
        source=folder/('old-test.c' if historical else 'test.c')
        source.write_text((ROOT/'tests/fat32_snapshot_stream_test.c').read_text().replace('"fat32_mount_identity_test.c"','"'+str(fixture)+'"'))
        binary=source.with_suffix('')
        subprocess.run([CC,'-O1','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-type-limits',
            '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',*dead_strip_flags(),
            '-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel/src/storage'),'-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),str(source),'-o',str(binary)],check=True)
        return binary
    binary=build();subprocess.run([str(binary)],check=True)
    control=subprocess.run([str(build(True))],capture_output=True,text=True)
    assert control.returncode==3 and 'SNAPSHOT_LINEAR_BOUND:' in control.stderr,control.stdout+control.stderr
    print('Historical control rejected specifically for repeated FAT traversal:',control.stderr.strip())
    image=folder/'media.img'
    with image.open('wb') as f:f.truncate(64*1024*1024)
    tools=external_tools('fat32')
    subprocess.run([tools['formatter'],'-F32','-s','1',str(image)],check=True)
    payload=folder/'TARGET.BIN';payload.write_bytes(bytes((i*13+7)&255 for i in range(2*1024*1024)))
    subprocess.run(['mcopy','-i',str(image),str(payload),'::TARGET.BIN'],check=True)
    before=hashlib.sha256(image.read_bytes()).digest()
    subprocess.run([str(binary),'--image',str(image)],check=True)
    subprocess.run([tools['checker'],'-n',str(image)],check=True)
    assert hashlib.sha256(image.read_bytes()).digest()==before
