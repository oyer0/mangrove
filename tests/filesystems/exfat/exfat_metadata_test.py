#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production metadata transactions plus independent exfatprogs/field checks."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[3]
compiler = sys.argv[1] if len(sys.argv) > 1 else 'gcc'
get32 = lambda data, off: struct.unpack_from('<I', data, off)[0]
get64 = lambda data, off: struct.unpack_from('<Q', data, off)[0]
rotate = lambda n: (n >> 1) | ((n & 1) << 15)
def checksum(data):
    result = 0
    for i, byte in enumerate(data):
        if i not in (2, 3): result = (rotate(result) + byte) & 65535
    return result

def seed_split(path):
    data = bytearray(path.read_bytes())
    heap, root_cluster = get32(data,88), get32(data,96)
    cluster = 1 << (data[108]+data[109])
    address = lambda c: heap*512 + (c-2)*cluster
    at = address(root_cluster)
    bitmap = next(get32(data,pos+20) for pos in range(at,at+cluster,32) if data[pos]==0x81)
    system = [bytes(data[pos:pos+32]) for pos in range(at,at+cluster,32) if data[pos] in (0x81,0x82)]
    data[at:at+cluster] = bytes(cluster); data[at:at+64] = b''.join(system)
    first = next(c for c in range(2,get32(data,92)+2)
        if not data[address(bitmap)+(c-2)//8] & (1 << ((c-2)%8)))
    data[address(bitmap)+(first-2)//8] |= 1 << ((first-2)%8)
    data[address(first)] = ord('A')
    for pos in range(at+64,at+480,32): data[pos]=0x05
    entry = bytearray(96); entry[0]=0x85; entry[1]=2; entry[4]=0x20
    stamp = (20<<25)|(1<<21)|(1<<16)
    for off in (8,12,16): struct.pack_into('<I',entry,off,stamp)
    entry[22:25]=b'\x80'*3
    entry[20]=137; entry[22]=0x84  # Imported creation fraction and UTC+1h survive.
    name='split.txt'; entry[32]=0xc0; entry[33]=3; entry[35]=len(name)
    struct.pack_into('<Q',entry,40,1); struct.pack_into('<I',entry,52,first)
    struct.pack_into('<Q',entry,56,1); entry[64]=0xc1
    hashed=0
    for i, char in enumerate(name):
        unit=ord(char.upper()); hashed=(rotate(hashed)+unit)&65535; hashed=rotate(hashed)
        struct.pack_into('<H',entry,66+i*2,ord(char))
    struct.pack_into('<H',entry,36,hashed); struct.pack_into('<H',entry,2,checksum(entry))
    data[at+480:at+576]=entry; path.write_bytes(data)
    return at+480, address(bitmap), stamp

def verify(path, primary, bitmap, creation, length):
    data=path.read_bytes(); count=get32(data,92)
    used=sum(bool(data[bitmap+i//8] & (1 << (i%8))) for i in range(count))
    assert data[112] == used*100//count
    assert not struct.unpack_from('<H',data,106)[0] & 2
    entry=data[primary:primary+96]
    assert get32(entry,8)==creation and entry[20]==137 and entry[22]==0x84
    stamp=(46<<25)|(12<<21)|(31<<16)|(23<<11)|(59<<5)|29
    assert get32(entry,12)==stamp and get32(entry,16)==stamp
    assert entry[21]==100 and entry[23:25]==b'\x80'*2
    assert get64(entry,40)==length and get64(entry,56)==length
    assert struct.unpack_from('<H',entry,2)[0]==checksum(entry)
    print(f'Independent fields: {path.name} allocated={used}/{count} PercentInUse={data[112]} length={length} UTC=2026-12-31 23:59:59')

with tempfile.TemporaryDirectory(prefix='mangrove-exfat-metadata-') as temp:
    directory=Path(temp); binary=directory/'metadata'
    subprocess.run([compiler,'-O1','-std=gnu11','-Wall','-Wextra','-Werror',
        '-Wno-unused-function','-Wno-type-limits','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined',*dead_strip_flags(),'-I'+str(root/'kernel/include'),
        '-I'+str(root/'include'),'-idirafter',str(root/'libc/include'),
        str(root/'tests/filesystems/exfat/exfat_metadata_test.c'),str(root/'libc/src/utf8.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    search=os.environ.get('PATH','')+os.pathsep+'/usr/sbin'
    mkfs,fsck=(shutil.which(n,path=search) for n in ('mkfs.exfat','fsck.exfat'))
    if not mkfs or not fsck:
        print('SKIP independent metadata verification: exfatprogs required (mkfs.exfat, fsck.exfat).')
    else:
        image=directory/'imported.img'
        with image.open('wb') as stream: stream.truncate(12000*512)
        subprocess.run([mkfs,'-c','4096',str(image)],check=True)
        primary,bitmap,creation=seed_split(image)
        subprocess.run([fsck,'-n',str(image)],check=True)
        outputs=[directory/name for name in ('large.img','small.img','empty.img')]
        subprocess.run([str(binary),str(image),*[str(p) for p in outputs]],check=True)
        for path,length in zip(outputs,(2*1024*1024,513,0)):
            verify(path,primary,bitmap,creation,length)
            subprocess.run([fsck,'-n',str(path)],check=True)
        print('Independent split-sector metadata, production readback and exfatprogs validation passed.')
