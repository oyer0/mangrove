#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Complete production discovery/mutex, controlled IRQ and scheduler parking.
No guest, SMP hardware or filesystem mutation qualification is implied.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
compiler=sys.argv[1] if len(sys.argv)>1 else 'gcc'
with tempfile.TemporaryDirectory(prefix='mangrove-device-snapshot-') as temp:
    folder=Path(temp)
    mutex=(ROOT/'kernel/src/mutex.c').read_text()
    mutex=re.sub(r'static u64 mutex_irq_save\(void\).*?\n}', 'static u64 mutex_irq_save(void) { return 0; }',mutex,count=1,flags=re.S)
    mutex=re.sub(r'static void mutex_irq_restore\(u64 flags\).*?\n}', 'static void mutex_irq_restore(u64 flags) { (void)flags; }',mutex,count=1,flags=re.S)
    (folder/'mutex.c').write_text(mutex)
    scheduler=(ROOT/'kernel/src/scheduler.c').read_text()
    wake=re.search(r'^bool scheduler_mutex_wake\(.*?\n}',scheduler,re.M|re.S).group(0)
    # Include exactly the production wake decision in the fixture translation unit,
    # where controlled scheduler_lock/unblock dependencies are available.
    source=(ROOT/'tests/storage/device_snapshot_test.c').read_text()
    source=source.replace('/* scheduler_mutex_wake() is compiled from the production scheduler by the runner. */',wake)
    (folder/'test.c').write_text(source.replace('"../../kernel/src/device.c"','"'+str(ROOT/'kernel/src/device.c')+'"'))
    command=[compiler,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel/include/pci'),
        '-I'+str(ROOT/'kernel/include/usb'),'-I'+str(ROOT/'include'),'-idirafter',str(ROOT/'libc/include'),
        str(folder/'test.c'),str(folder/'mutex.c'),'-o',str(folder/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(folder/'test')],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},check=True,timeout=30)

    historical=subprocess.check_output(['git','show','32c4d70d05eee9fdeb74a3f3916e15c38c396be8:kernel/src/device.c'],cwd=ROOT).decode()
    (folder/'historical-device.c').write_text(historical)
    control=(folder/'test.c').read_text().replace('"'+str(ROOT/'kernel/src/device.c')+'"','"'+str(folder/'historical-device.c')+'"')
    (folder/'control.c').write_text(control)
    old_command=command.copy()
    old_command[old_command.index(str(folder/'test.c'))]=str(folder/'control.c')
    old_command[old_command.index('-o')+1]=str(folder/'control')
    old_command.insert(1,'-DSNAPSHOT_LEGACY_CONTROL')
    subprocess.run(old_command,check=True)
    failed=subprocess.run([str(folder/'control')],cwd=folder,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},capture_output=True,text=True,timeout=10)
    assert failed.returncode!=0 and '==MG_ERR_BUSY' in failed.stderr,failed.stdout+failed.stderr
    print('historical negative control: unsafe nested refresh is admitted; production regression rejects it')
