#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Disposable secondary-volume native guest qualification; shares the MGFS runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import secrets
import shutil
import struct
import sys
import time
sys.dont_write_bytecode = True
import guest_mgfs_test as guest

ROOT = guest.ROOT
PART_OFFSET = 2048 * 512
PART_SIZE = 64 * 1024 * 1024
SECONDARY_SIZE = PART_OFFSET + PART_SIZE + 2048 * 512
LABELS = {'mgfs': 'QUAL_MGFS', 'fat32': 'QUAL_FAT', 'exfat': 'QUAL_EXFAT'}
SEED = b'independent-secondary\n'
u16 = lambda b, at: struct.unpack_from('<H', b, at)[0]
u32 = lambda b, at: struct.unpack_from('<I', b, at)[0]
u64 = lambda b, at: struct.unpack_from('<Q', b, at)[0]


def seal(image, tables=()):
    view = guest.layout(image)
    for table in tables:
        guest.image_tool.refresh_table_checksum(image, view, table)
    guest.image_tool.refresh_record_bitmap_checksum(image, view)
    guest.image_tool.refresh_metadata_checksums(image, view)
    header = bytearray(image[:4096]); guest.checksum(header, 192, 200)
    image[:4096] = header


def install(image, parent, name, data=b'', directory=False, owner=0, permissions=7):
    guest.install(image, parent, name, data)
    identifier = guest.image_tool.directory_find(guest.payload(image, guest.record(image, parent)), name)
    view = guest.layout(image)
    at = guest.image_tool.record_offset(image, view['record_table_start'], identifier, view['record_count'])
    if directory:
        if data: raise ValueError('only empty offline directory provisioning is supported')
        guest.image_tool.write_record(image, at, identifier, 2, 1, b'', [], 0, owner, permissions)
    else:
        guest.image_tool.rewrite_record_security(image, view, identifier, owner, permissions)
    seal(image, [(at // 4096) - view['record_table_start']])
    return identifier


def replace(image, path, data):
    identifier, _ = guest.find(image, path)
    table = guest.image_tool.replace_file_payload(image, guest.layout(image), identifier, data, set())
    seal(image, [table])


def write_partition(disk, image, offset=PART_OFFSET):
    with disk.open('r+b') as output:
        output.seek(offset); output.write(image)


def partition(disk):
    # Independently verify the actual GPT selection, not only a hardcoded offset.
    with disk.open('rb') as source:
        source.seek(512); header = source.read(512)
        if header[:8] != b'EFI PART' or u32(header,84) != 128: raise ValueError('GPT header')
        source.seek(u64(header,72)*512); entry = source.read(128)
        if u64(entry,32)*512 != PART_OFFSET or (u64(entry,40)+1)*512-PART_OFFSET != PART_SIZE:
            raise ValueError('GPT partition geometry')
        source.seek(PART_OFFSET); data = source.read(PART_SIZE)
    if len(data) != PART_SIZE: raise ValueError('short partition')
    return data


class ExfatReader:
    """Independent, read-only decoder. No Mangrove driver code or runtime writes.

    Declared layout: 512-byte sectors, one FAT, standard ordinary entry sets,
    <=64 MiB media, <=4096 entries per directory, <=16 directory levels.
    Both contiguous and FAT-linked streams are decoded with bounds/cycle checks.
    fsck.exfat separately validates allocation ownership and the format.
    """
    def __init__(self, data):
        if len(data)<512: raise ValueError('short independent exFAT boot sector')
        self.data = data
        if data[3:11] != b'EXFAT   ' or data[108] != 9 or data[110] != 1:
            raise ValueError('unsupported independent exFAT layout')
        self.cb = 512 << data[109]
        self.count, self.root = u32(data,92), u32(data,96)
        if len(data)>PART_SIZE: raise ValueError('independent media bound')
        self.fat, self.heap = u32(data,80)*512, u32(data,88)*512
        if (not 512 <= self.cb <= 32768 or not self.count or
            self.fat+self.count*4+8 > self.heap or self.heap+self.count*self.cb > len(data)):
            raise ValueError('exFAT geometry')

    def address(self, cluster):
        if not 2 <= cluster < self.count+2: raise ValueError('cluster outside media')
        return self.heap+(cluster-2)*self.cb

    def stream(self, first, length=None, contiguous=False):
        if length == 0:
            if first: raise ValueError('empty stream with allocation')
            return b''
        needed = None if length is None else (length+self.cb-1)//self.cb
        if needed is not None and needed > self.count: raise ValueError('stream length')
        pieces, seen = [], set()
        cluster = first
        while True:
            if cluster in seen or len(seen) >= self.count: raise ValueError('FAT cycle')
            seen.add(cluster); at = self.address(cluster)
            pieces.append(self.data[at:at+self.cb])
            if contiguous:
                if len(pieces) == needed: break
                cluster += 1
            else:
                following = u32(self.data,self.fat+cluster*4)
                if following >= 0xfffffff8:
                    if needed is not None and len(pieces) != needed: raise ValueError('short FAT chain')
                    break
                if needed is not None and len(pieces) >= needed: raise ValueError('long FAT chain')
                cluster = following
        result=b''.join(pieces)
        return result if length is None else result[:length]

    def entries(self, data):
        entries = {}
        at=0
        while at < len(data):
            if at//32 >= 4096: raise ValueError('independent directory bound')
            kind=data[at]
            if not kind: break
            if not kind&0x80: at+=32; continue
            if kind in (0x81,0x82,0x83): at+=32; continue
            if kind != 0x85: raise ValueError('unsupported active entry')
            count=data[at+1]; size=(count+1)*32; entry=data[at:at+size]
            if len(entry)!=size or not 2 <= count <= 18 or entry[32]!=0xc0:
                raise ValueError('entry set')
            checksum=0
            for i,b in enumerate(entry):
                if i not in (2,3): checksum=(((checksum>>1)|((checksum&1)<<15))+b)&65535
            if checksum!=u16(entry,2): raise ValueError('entry checksum')
            units=entry[35]; encoded=b''
            if not units or count != 1+(units+14)//15: raise ValueError('name secondary count')
            for slot in range(64,size,32):
                if entry[slot]!=0xc1: raise ValueError('filename secondary')
                encoded+=entry[slot+2:slot+32]
            name=encoded[:units*2].decode('utf-16-le',errors='strict')
            if name in entries: raise ValueError('duplicate independent namespace')
            length=u64(entry,56); valid=u64(entry,40); flags=entry[33]
            if flags not in (1,3) or valid>length: raise ValueError('stream metadata')
            body=self.stream(u32(entry,52),length,bool(flags&2))
            # ValidDataLength is part of the oracle, never expose undefined slack.
            body=body[:valid]+bytes(length-valid)
            entries[name]={'directory':bool(u16(entry,4)&16),'data':body,
                           'valid':valid,'length':length,'entry':entry}
            at+=size
        return entries

    def find(self, path):
        entries=self.entries(self.stream(self.root))
        for index,name in enumerate(path.strip('/').split('/')):
            if index>=16 or name not in entries: raise ValueError('missing independent exFAT path '+path)
            found=entries[name]
            if index+1 < len(path.strip('/').split('/')):
                if not found['directory']: raise ValueError('not directory')
                entries=self.entries(found['data'])
        return found


def create_secondary(folder, build, backend):
    image=folder/'initial-secondary.img'; label=LABELS[backend]; log=folder/'secondary-image.log'
    if backend=='mgfs':
        guest.command([build/'mkmgfs','--blocks','16384','--uuid',
            '00000000-0000-0000-0000-000000000002','--format-time-ns','0',image,'--label',label],log)
        data=bytearray(image.read_bytes()); work=install(data,1,'work',directory=True,owner=1000)
        install(data,work,'seed.txt',SEED); image.write_bytes(data)
        guest.check_image(build,image,log)
    elif backend=='fat32':
        with image.open('wb') as out: out.truncate(PART_SIZE)
        guest.command(['/usr/sbin/mkfs.fat','-F32','--invariant','-n',label,image],log)
        seed=folder/'seed.txt'; seed.write_bytes(SEED)
        guest.command(['mmd','-i',image,'::/work'],log)
        guest.command(['mcopy','-i',image,seed,'::/work/seed.txt'],log)
    else:
        with image.open('wb') as out: out.truncate(PART_SIZE)
        guest.command(['/usr/sbin/mkfs.exfat','-L',label,image],log)
        # Only root is initially needed. The guest creates /work through the
        # normal native mkdir path; no private exFAT image mutation encoder.
    if backend!='mgfs':
        guest.command(['/usr/sbin/fsck.'+('fat' if backend=='fat32' else 'exfat'),'-n',image],log)
    disk=folder/'secondary.img'
    with disk.open('wb') as out: out.truncate(SECONDARY_SIZE)
    guest.command(['/usr/sbin/sgdisk','--new=1:2048:133119','--typecode=1:0700',
                   '--change-name=1:QUAL_DATA',disk],log)
    write_partition(disk,image.read_bytes())
    return disk


def prepare_boot(folder, build, run, backend):
    disk=guest.create_disk(folder,build,run)
    with disk.open('rb') as source: source.seek(guest.ROOT_OFFSET); image=bytearray(source.read(guest.PART_BYTES))
    label=LABELS[backend]; target=f'/vol/{label}/work'
    for phase in ('create','verify','negative','readonly','denied','wrong-label','missing-service','missing-device'):
        mode={'negative':'verify','wrong-label':'verify','missing-service':'verify','missing-device':'verify'}.get(phase,phase)
        use_label='MISSING_VOLUME' if phase=='wrong-label' else label
        use_target='/vol/MISSING_VOLUME/work' if phase=='wrong-label' else target
        script=(f'/bin/fstest volume-{mode} {run} {use_label} {backend} {use_target} > /temp/{phase}.log\n'
                f'/bin/say child-exit-0-{phase}-{run} > /temp/{phase}.exit\n/bin/shutdown\n').encode()
        if phase in ('create','verify','negative'): replace(image,f'/temp/{phase}.shoot',script)
        else: install(image,6,phase+'.shoot',script)
    from populate_mgfs import DEFAULT_SECURITY_CONFIG
    if guest.payload(image,guest.find(image,'/conf/security/config')[1])!=DEFAULT_SECURITY_CONFIG:
        raise ValueError('qualification requires the unchanged confirmation policy')
    write_partition(disk,image,guest.ROOT_OFFSET)
    guest.check_image(build, save(folder/'prepared-root.img',image),folder/'prepared-root-checker.log')
    return disk


def save(path, data): path.write_bytes(data); return path


def expected_vops(phase, backend):
    rows=[('volume-snapshot',0),('automounted',1),('removable-leaf',1),('mountpoint',0),('target',0),
          ('readonly-state',int(phase in ('readonly','denied'))),('device-snapshot',0),('gpt-leaf',1),
          ('usb-parent',1),('physical-readonly',int(phase in ('readonly','denied'))),('utc-clock',0),('whole-second-clock',0),('direct-mount-denied',-25),('direct-unmount-denied',-25),('direct-suppression-denied',-25)]
    if phase=='create':
        rows += [('workspace-create' if backend=='exfat' else 'workspace-existing',0)]
        if backend=='exfat':
            rows += [('seed-create',0),('seed-write-open',1),('seed-write',len(SEED)),('seed-write-close',0)]
    rows += [('seed-open',1),('seed-read',len(SEED)),('seed-bytes',0),('seed-close',0)]
    if phase=='create':
        rows += [('mkdir',0),('directory-stat',0),('directory-type',2),('rmdir',0),('removed-directory',-2),
                 ('confirmation-denied',-26),('cancelled-mount-present',0),('busy-file-open',1),('busy-file',-14),
                 ('busy-file-preserved',0),('busy-file-close',0),('busy-cursor-open',1),('busy-cursor',-14),
                 ('busy-cursor-preserved',0),('busy-cursor-close',0),('getcwd',1),('cwd-size',1),('chdir-volume',0),('busy-cwd',-14),
                 ('restore-cwd',0),('child-spawn',1),('child-wait',0),('child-status',0),('child-close',0),
                 ('unmount',0),('unmount-identity',1),('detached',-2),('suppressed-snapshot',0),('not-automounted',0),
                 ('explicit-remount',0),('remount-identity',1)]
    if phase=='readonly': rows += [('readonly-create',-8),('readonly-mkdir',-8),('readonly-absent',-2),('readonly-open',-12)]
    if phase=='denied': rows += [('unauthorized-unmount',-25),('denied-mount-still-present',0)]
    else: rows += [('final-unmount',0),('final-detached',-2),('final-snapshot',0),('final-suppressed',0)]
    return rows


def validate(text, phase, run, backend):
    label=LABELS[backend]; target=f'/vol/{label}/work'
    header=f'VOLUME1 phase=volume-{phase} run={run} label={label} filesystem={backend} target={target}\n'
    if not text.startswith(header): raise ValueError('missing volume execution header')
    identity=re.findall(r'^VOLUME1 identity instance=(\d+) parent=(\d+) name=(disk\d+p\d+) mount=(/vol/\w+)$',text,re.M)
    if len(identity)!=1 or int(identity[0][0])<=0 or int(identity[0][1])<=0 or identity[0][0]==identity[0][1] or identity[0][3]!=f'/vol/{label}':
        raise ValueError('missing validated secondary identity')
    rows=re.findall(r'^VOP (\d+) ([\w-]+) expected=(-?\d+) observed=(-?\d+)$',text,re.M)
    wanted=expected_vops(phase,backend)
    if rows != [(str(i),name,str(value),str(value)) for i,(name,value) in enumerate(wanted,1)]:
        raise ValueError('incorrect or incomplete native lifecycle outcomes')
    complete=f'VOLUME1 complete phase=volume-{phase} run={run} ops={len(wanted)}\n'
    if not text.endswith(complete): raise ValueError('missing complete lifecycle')
    blocks=re.findall(r'GUESTFS1 phase=(create|verify) run=[^\n]+\n.*?GUESTFS1 complete[^\n]+\n',text,re.S)
    bodies=re.findall(r'GUESTFS1 phase=(?:create|verify) run=[^\n]+\n.*?GUESTFS1 complete[^\n]+\n',text,re.S)
    if blocks != (['create','verify'] if phase=='create' else ['verify']): raise ValueError('missing content phases')
    for kind,body in zip(blocks,bodies):
        match=re.search(r'identifier=(\d+)\n$',body)
        if not match or int(match[1])<=0: raise ValueError('file identity')
        guest.validate_transcript(body,kind,run,int(match[1]))
    def position(step):
        return text.index(next(line for line in text.splitlines(True) if re.match(r'VOP \d+ '+step+r' ',line)))
    if position('seed-close') > text.index(bodies[0]): raise ValueError('content before initialization')
    if phase=='create':
        if text.index(bodies[0])+len(bodies[0]) > position('mkdir') or position('remount-identity') > text.index(bodies[1]):
            raise ValueError('content/lifecycle ordering')
        marker=text.index('VOLUME1 child-open-handles-exit\n') if 'VOLUME1 child-open-handles-exit\n' in text else -1
        if not position('restore-cwd') < marker < position('child-wait'): raise ValueError('child lifetime ordering')
    if phase!='denied' and text.index(bodies[-1])+len(bodies[-1]) > position('final-unmount'):
        raise ValueError('content after detach')
    remaining=text[len(header):]
    for body in bodies: remaining=remaining.replace(body,'',1)
    remaining=re.sub(r'^VOP [^\n]+\n|^VOLUME1 identity [^\n]+\n','',remaining,flags=re.M)
    if phase=='create':
        if remaining.count('VOLUME1 child-open-handles-exit\n')!=1: raise ValueError('no child cleanup witness')
        remaining=remaining.replace('VOLUME1 child-open-handles-exit\n','',1)
    if remaining!=complete: raise ValueError('unexpected guest output')
    return {'volume_instance':int(identity[0][0]),'parent_instance':int(identity[0][1]),'operations':len(rows)}


def root_evidence(folder, disk, phase, build, run, backend, success=True):
    with disk.open('rb') as source: source.seek(guest.ROOT_OFFSET); image=source.read(guest.PART_BYTES)
    guest.check_image(build,save(folder/f'{phase}-root.img',image),folder/f'{phase}-root-checker.log')
    _, item=guest.find(image,f'/temp/{phase}.log'); text=guest.payload(image,item).decode('ascii')
    (folder/f'{phase}-guest.log').write_text(text)
    if success:
        result=validate(text,phase,run,backend)
        _, item=guest.find(image,f'/temp/{phase}.exit')
        if guest.payload(image,item)!=f'child-exit-0-{phase}-{run}\n'.encode(): raise ValueError('missing waited child')
        return result
    if 'VOLUME1 complete' in text: raise ValueError('failed phase reported completion')
    try: guest.find(image,f'/temp/{phase}.exit')
    except ValueError: return text
    raise ValueError('failed child continued success script')


def filesystem_identity(data, backend):
    if backend=='mgfs':
        if data[:8]!=b'MGFSv1\0\0' or u64(data,32)!=4096: raise ValueError('MGFS geometry')
        return data[72:88].hex()
    if backend=='fat32':
        if data[82:90]!=b'FAT32   ' or u16(data,11)!=512: raise ValueError('FAT32 geometry')
        return f'{u32(data,67):08x}'
    ExfatReader(data)
    return f'{u32(data,100):08x}'


def inspect_secondary(folder,disk,backend,build,run,phase):
    data=partition(disk); image=save(folder/f'{phase}-secondary.img',data); log=folder/f'{phase}-media.log'
    identity=filesystem_identity(data,backend)
    initial=filesystem_identity((folder/'initial-secondary.img').read_bytes(),backend)
    if identity!=initial: raise ValueError('filesystem identity changed')
    path=f'/work/mgfs-{run}.bin'
    if backend=='mgfs':
        guest.check_image(build,image,log); _, item=guest.find(data,path)
        actual=guest.payload(data,item)
        entries=[row[4].decode() for row in guest.image_tool.directory_entries(
            guest.payload(data,guest.find(data,'/work')[1])) if row[3]&1]
        if guest.image_tool.u64(item,0)!=1: raise ValueError('MGFS file type')
        if u64(data,88)!=1: raise ValueError('MGFS root identity')
        seed=guest.payload(data,guest.find(data,'/work/seed.txt')[1])
        label=data[216:216+u64(data,208)].decode()
    elif backend=='fat32':
        guest.command(['/usr/sbin/fsck.fat','-n',image],log)
        extracted=folder/f'{phase}-payload.bin'
        guest.command(['mcopy','-i',image,'::'+path,extracted],log); actual=extracted.read_bytes()
        guest.command(['mdir','-b','-i',image,'::/work'],log)
        listing=log.read_text(); entries=re.findall(r'::/work/([^\n]+)',listing)
        label=data[71:82].decode().rstrip()
        seed_file=folder/f'{phase}-seed.txt'
        guest.command(['mcopy','-i',image,'::/work/seed.txt',seed_file],log)
        seed=seed_file.read_bytes()
    else:
        guest.command(['/usr/sbin/fsck.exfat','-n',image],log)
        reader=ExfatReader(data); item=reader.find(path); actual=item['data']
        if item['directory'] or item['valid']!=guest.SIZE or item['length']!=guest.SIZE: raise ValueError('exFAT metadata')
        entries=list(reader.entries(reader.find('/work')['data']))
        root=reader.stream(reader.root); labels=[]
        for at in range(0,len(root),32):
            if not root[at]: break
            if root[at]==0x83: labels.append(root[at+2:at+2+2*root[at+1]].decode('utf-16-le'))
        if len(labels)!=1: raise ValueError('exFAT volume label')
        label=labels[0]
        seed=reader.find('/work/seed.txt')['data']
    if seed!=SEED: raise ValueError('independent seed content mismatch')
    if label!=LABELS[backend]: raise ValueError('host volume label mismatch: '+repr(label))
    if actual!=guest.expected_payload(run): raise ValueError('independent secondary content mismatch')
    if sorted(entries)!=sorted(['seed.txt',f'mgfs-{run}.bin']): raise ValueError('independent namespace mismatch '+repr(entries))
    return {'payload_sha256':hashlib.sha256(actual).hexdigest(),'partition_sha256':hashlib.sha256(data).hexdigest(),
            'filesystem_identity':identity,'label':label,'bytes':len(actual),'namespace':sorted(entries)}


def private_boot_copy(folder, disk, build, kind):
    copy=folder/(kind+'-boot.img'); shutil.copyfile(disk,copy)
    with copy.open('rb') as source:
        source.seek(guest.ROOT_OFFSET); image=bytearray(source.read(guest.PART_BYTES))
    if kind=='denied':
        from populate_mgfs import ACCOUNT_DATABASE_RECORD_ID
        accounts=guest.payload(image,guest.record(image,ACCOUNT_DATABASE_RECORD_ID)).decode('ascii')
        original=next(row for row in accounts.splitlines() if row.startswith('account '))
        visitor=original.replace('uid=1000','uid=1001').replace('username=developer','username=visitor').replace(
            'role=admin','role=regular').replace('home=/home/developer','home=/home/visitor').replace('flags=initial','flags=none')
        replace(image,'/sys/accounts/users',(accounts.replace('next_uid=1001','next_uid=1002')+'\n'+visitor+'\n').encode())
        install(image,7,'visitor',directory=True,owner=1001,permissions=3)
        replace(image,'/conf/session/config',b'autologin=true\nuser=visitor\n')
    elif kind=='missing-service':
        _, item=guest.find(image,'/core')
        body=guest.image_tool.directory_rename(guest.payload(image,item),'volumed','missing')
        table=guest.image_tool.replace_directory_payload(image,guest.layout(image),4,body,set())
        seal(image,[table])
    write_partition(copy,image,guest.ROOT_OFFSET)
    guest.check_image(build,save(folder/(kind+'-initial-root.img'),image),folder/(kind+'-initial-checker.log'))
    return copy


def corrupt_payload(disk, backend, folder, build, run):
    data=bytearray(partition(disk)); path=f'/work/mgfs-{run}.bin'
    if backend=='mgfs':
        _, item=guest.find(data,path); extents,_=guest.image_tool.record_extents(data,item)
        if not extents or extents[0][0]!=0: raise ValueError('negative mapping')
        data[extents[0][1]*4096]^=1
    elif backend=='exfat':
        reader=ExfatReader(data); item=reader.find(path)
        data[reader.address(u32(item['entry'],52))]^=1
    else:
        image=save(folder/'negative-before.img',data); blob=folder/'corrupted-payload.bin'
        blob.write_bytes(bytes([guest.expected_payload(run)[0]^1])+guest.expected_payload(run)[1:])
        guest.command(['mcopy','-o','-i',image,blob,'::'+path],folder/'negative-mtools.log')
        data=bytearray(image.read_bytes())
    write_partition(disk,data)


def controls(folder, boot, secondary, backend, build, run, args):
    result={}
    args.secondary_readonly=True; args.confirmations=['y']
    before=guest.digest(secondary)
    result['readonly']=guest.boot(folder,boot,'readonly',args)
    result['readonly'].update(root_evidence(folder,boot,'readonly',build,run,backend))
    if guest.digest(secondary)!=before: raise ValueError('read-only media changed')
    result['readonly']['unchanged_sha256']=before
    denied_boot=private_boot_copy(folder,boot,build,'denied')
    args.confirmations=[]; args.login_prompt='visitor ~:'
    result['unauthorized']=guest.boot(folder,denied_boot,'denied',args)
    result['unauthorized'].update(root_evidence(folder,denied_boot,'denied',build,run,backend))
    if guest.digest(secondary)!=before: raise ValueError('unauthorized read-only campaign changed media')
    args.login_prompt='developer ~:'
    args.expected_child_failure=True
    for phase in ('wrong-label','missing-service','missing-device'):
        disk=private_boot_copy(folder,boot,build,phase) if phase=='missing-service' else boot
        args.secondary_disk=None if phase=='missing-device' else secondary
        result[phase]=guest.boot(folder,disk,phase,args)
        text=root_evidence(folder,disk,phase,build,run,backend,False)
        if not re.search(r'^VOP 1 volume-snapshot expected=0 observed=-(2|27)$',text,re.M):
            raise ValueError('negative control failed for unexpected reason: '+phase)
        result[phase]['status']='EXPECTED_FAILURE'
    args.secondary_disk=secondary; args.secondary_readonly=False
    wrong=folder/'wrong-secondary.img'; shutil.copyfile(secondary,wrong)
    corrupt_payload(wrong,backend,folder,build,run); args.secondary_disk=wrong
    result['wrong-content']=guest.boot(folder,boot,'negative',args)
    text=root_evidence(folder,boot,'negative',build,run,backend,False)
    expected=guest.expected_payload(run)
    if f'FAIL bytes offset=0 expected={expected[0]} observed={expected[0]^1}\n' not in text:
        raise ValueError('wrong-content control did not identify byte zero')
    # Independently establish that the verification process did not repair data.
    try: inspect_secondary(folder,wrong,backend,build,run,'negative')
    except ValueError as error:
        if str(error)!='independent secondary content mismatch': raise
    else: raise ValueError('negative image unexpectedly correct')
    result['wrong-content']['status']='EXPECTED_FAILURE'
    args.expected_child_failure=False
    args.secondary_disk=secondary
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True); parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--backend',choices=('mgfs','fat32','exfat','all'),default='all')
    parser.add_argument('--accel',choices=('tcg','kvm'),default='tcg'); parser.add_argument('--qemu',default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code',type=Path,default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars',type=Path,default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--run-id',help='16 lowercase hexadecimal digits; one filesystem and repetition only')
    parser.add_argument('--timeout',type=int,default=180); parser.add_argument('--repetitions',type=int,default=1)
    args=parser.parse_args(); args.output=args.output.resolve(); args.build_dir=args.build_dir.resolve()
    if args.output.exists() or not 1<=args.repetitions<=3 or not 30<=args.timeout<=300: parser.error('new output directory and bounded repetitions/timeout required')
    for path in (args.output,args.ovmf_code,args.ovmf_vars):
        if ',' in str(path) or '\n' in str(path): parser.error('unsafe QEMU path')
    if args.run_id and (not re.fullmatch('[0-9a-f]{16}',args.run_id) or args.backend=='all' or args.repetitions!=1):
        parser.error('run-id requires one backend, one repetition and 16 hexadecimal digits')
    args.output.mkdir(parents=True)
    started=time.monotonic()
    report={'status':'FAIL','accel':args.accel,'profiles':[]}
    try:
        profiles=(3 if args.backend=='all' else 1)*args.repetitions
        if shutil.disk_usage(args.output).free < profiles*2*1024**3:
            raise ValueError('qualification requires 2 GiB free artifact capacity per profile; use disk-backed output')
        report['binaries']={str(name):guest.digest(args.build_dir/name) for name in
            (Path('Mangrove/pith.elf'),Path('fstest/fstest.elf'),Path('volumed/volumed.elf'))}
        for backend in (LABELS if args.backend=='all' else [args.backend]):
            for repeat in range(args.repetitions):
                folder=args.output/f'{backend}-{repeat}'; folder.mkdir(); run=args.run_id or secrets.token_hex(8)
                result={'backend':backend,'run':run}; report['profiles'].append(result)
                secondary=create_secondary(folder,args.build_dir,backend)
                boot=prepare_boot(folder,args.build_dir,run,backend)
                result['initial_boot_sha256']=guest.digest(boot)
                result['initial_secondary_sha256']=guest.digest(secondary)
                args.secondary_disk=secondary; args.secondary_readonly=False
                for phase in ('create','verify'):
                    args.confirmations=['n','y','y','y','y','y','y'] if phase=='create' else ['y']
                    result[phase]=guest.boot(folder,boot,phase,args)
                    result[phase].update(root_evidence(folder,boot,phase,args.build_dir,run,backend))
                    result[phase]['media']=inspect_secondary(folder,secondary,backend,args.build_dir,run,phase)
                result['controls']=controls(folder,boot,secondary,backend,args.build_dir,run,args)
                result['status']='PASS'
        report['status']='PASS'
    except Exception as error:
        report['error']=str(error)
        raise
    finally:
        usage=resource.getrusage(resource.RUSAGE_SELF)
        report['wall_seconds']=time.monotonic()-started
        report['runner_cpu_seconds']=usage.ru_utime+usage.ru_stime
        report['runner_peak_rss_bytes']=usage.ru_maxrss*(1 if sys.platform=='darwin' else 1024)
        report['artifact_bytes']=sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2))

if __name__=='__main__': main()
