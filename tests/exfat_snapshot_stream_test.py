#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production exFAT streaming/read I/O bounds with an independent historical control."""
from pathlib import Path
import subprocess
import sys
import tempfile
from host_build import dead_strip_flags
ROOT=Path(__file__).resolve().parents[1]
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
BASELINE='b3bccfc9715bd80f47f4b8b976ea4a385d52973a'
with tempfile.TemporaryDirectory(prefix='mangrove-exfat-stream-') as temp:
    folder=Path(temp)
    def build(old=False):
        fixture=ROOT/'tests/exfat_mount_identity_test.c'
        if old:
            source=folder/'old-exfat.c';source.write_bytes(subprocess.check_output(['git','show',BASELINE+':kernel/src/storage/exfat.c'],cwd=ROOT))
            target=folder/'old-fixture.c';target.write_text(fixture.read_text().replace('"../kernel/src/storage/exfat.c"','"'+str(source)+'"').replace('"../libc/src/time_convert.c"','"'+str(ROOT/'libc/src/time_convert.c')+'"'));fixture=target
        source=folder/('old-test.c' if old else 'test.c');source.write_text((ROOT/'tests/exfat_snapshot_stream_test.c').read_text().replace('"exfat_mount_identity_test.c"','"'+str(fixture)+'"'))
        binary=source.with_suffix('')
        subprocess.run([CC,'-O1','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-type-limits',
            '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',*dead_strip_flags(),
            '-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel/src/storage'),'-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),str(source),'-o',str(binary)],check=True)
        return binary
    subprocess.run([str(build())],check=True)
    result=subprocess.run([str(build(True))],capture_output=True,text=True)
    assert result.returncode==3 and 'EXFAT_LINEAR_BOUND:' in result.stderr,result.stdout+result.stderr
    print('Historical amplification control rejected specifically:',result.stderr.strip())
