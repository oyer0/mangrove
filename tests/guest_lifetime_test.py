#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded native filesystem failure/removal campaigns through the guest runner.
Not persistent recovery, controller stress, SMP or physical-hardware testing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import shutil
import sys
import time
sys.dont_write_bytecode = True
import guest_secondary_test as secondary
import guest_mgfs_test as guest


class RecoveryControl:
    def __init__(self, image, run, kind, inject=True):
        self.image, self.run, self.kind, self.inject = image, run, kind, inject
        self.actions = []

    def configure(self, invocation, part):
        drives = [i for i, value in enumerate(invocation) if value.startswith('id=secondary,file=')]
        if len(drives) != 1 or invocation[drives[0]-1] != '-drive':
            raise ValueError('expected exactly one isolated secondary backend')
        del invocation[drives[0]-1:drives[0]+1]
        debug = {'driver':'blkdebug', 'node-name':'secondary-debug', 'image':'secondary-file'}
        if self.inject:
            debug['inject-error'] = [{'event':'none','iotype':self.kind,'errno':5,'once':True}]
        for node in ({'driver':'file','node-name':'secondary-file','filename':str(self.image)}, debug,
                     {'driver':'raw','node-name':'secondary','file':'secondary-debug'}):
            invocation += ['-blockdev', json.dumps(node)]
        (part/'fault-definition.json').write_text(json.dumps(debug,indent=2))

    def observe(self, qmp, text, part, started):
        stage = len(self.actions)
        if stage >= 3:
            return
        name = ('ready','uncertain','retained')[stage]
        marker = f'RECOVERY1 {name} run={self.run}'
        if marker not in text:
            return
        (part/f'recovery-{name}.txt').write_text(text)
        stats = qmp.call('query-blockstats')
        selected=[entry for entry in stats if entry.get('node-name')=='secondary']
        if (stage==0 or (stage==1 and not getattr(self,'physically_deleted',False))) and len(selected)!=1:
            raise ValueError('missing unique secondary accounting')
        if stage==0 and any(selected[0]['stats'][key] for key in
                ('wr_operations','wr_bytes','flush_operations','failed_wr_operations','failed_flush_operations')):
            raise ValueError('secondary mutated before intended native operation')
        if stage==1:
            if self.inject and not getattr(self,'physically_deleted',False):
                key='failed_wr_operations' if self.kind=='write' else 'failed_flush_operations'
                if selected[0]['stats'][key]!=1:
                    raise ValueError('missing exact targeted QEMU error accounting')
            if not getattr(self,'physically_deleted',False):
                qmp.call('device_del', {'id':'secondary-storage'})
                qmp.wait_deleted('secondary-storage')
        status = qmp.call('query-status')
        if status['status'] != 'running':
            raise ValueError('QMP/guest not running at recovery handshake')
        self.actions.append(dict(stage=stage, marker=marker, seconds=time.monotonic()-started,
                                 blockstats=stats, qmp_status=status))
        qmp.call('human-monitor-command', {'command-line':'sendkey ret 30'})

    def finish(self, expected_failure, failed):
        if expected_failure:
            if self.inject or not failed or len(self.actions)!=1:
                raise ValueError('incorrect missing-fault negative-control execution')
        elif len(self.actions)!=3:
            raise ValueError('incomplete recovery/removal handshakes')



class ReplacementSectorControl(RecoveryControl):
    """Inline replacement allocates nothing: table write is publication and
    the first old-allocation bitmap write can only be post-publication cleanup.
    Sector addresses are decoded from the independently provisioned image.
    """
    def __init__(self,image,run,sector,inject=True):
        super().__init__(image,run,'write',inject);self.sector=sector
    def configure(self,invocation,part):
        super().configure(invocation,part)
        for i,value in enumerate(invocation):
            if value!='-blockdev':continue
            node=json.loads(invocation[i+1])
            if node.get('node-name')=='secondary-debug' and self.inject:
                node['inject-error'][0]['sector']=self.sector
                invocation[i+1]=json.dumps(node)
                (part/'fault-definition.json').write_text(json.dumps(node,indent=2))

class ReadControl(RecoveryControl):
    """Permanent read error at an independently provisioned data extent only."""
    def __init__(self,image,run,sector,inject=True):
        super().__init__(image,run,'read',inject)
        self.sector=sector
    def configure(self,invocation,part):
        super().configure(invocation,part)
        for i,value in enumerate(invocation):
            if value!='-blockdev':continue
            node=json.loads(invocation[i+1])
            if node.get('node-name')=='secondary-debug' and self.inject:
                node['inject-error'][0].update(sector=self.sector,once=False)
                invocation[i+1]=json.dumps(node)
                (part/'fault-definition.json').write_text(json.dumps(node,indent=2))
    def observe(self,qmp,text,part,started):
        if self.actions or f'RECOVERY1 ready run={self.run}' not in text:return
        stats=qmp.call('query-blockstats')
        target=[entry for entry in stats if entry.get('node-name')=='secondary']
        if len(target)!=1 or target[0]['stats']['failed_rd_operations'] or target[0]['stats']['wr_operations']:
            raise ValueError('read fault fired before intended native operation')
        self.actions.append(dict(stage=0,marker=f'RECOVERY1 ready run={self.run}',
                                 seconds=time.monotonic()-started,blockstats=stats,
                                 qmp_status=qmp.call('query-status')))
        qmp.call('human-monitor-command',{'command-line':'sendkey ret 30'})
    def finish(self,expected_failure,failed):
        if len(self.actions)!=1 or (expected_failure and (self.inject or not failed)):
            raise ValueError('missing online read-fault handshake')

READ_ROWS=[('volume',0),('open',1),('ready',1),('zero',0),('online-error',-12),
           ('retry-error',-12),('seek-eof',0),('eof',0),('close',0),('reopen',1),
           ('fresh-error',-12),('load-error',-12),('reclose',0),('no-owner',0),('online-stat',0),
           ('length',8193),('unmount',0),('detached',-2)]
def validate_read(text,run):
    wanted=f'READFAIL1 start run={run}\n'
    wanted+=''.join(f'VOP {i} readfault-{name} expected={n} observed={n}\n'
                    for i,(name,n) in enumerate(READ_ROWS,1))
    wanted+=f'READFAIL1 complete run={run} ops={len(READ_ROWS)}\n'
    if text!=wanted:raise ValueError('incorrect native online-read/error/EOF evidence')
    return dict(operations=len(READ_ROWS),online_io_error=True,genuine_eof=True,no_recovery_owner=True)


class ActiveControl(RecoveryControl):
    """Trace-triggered deletion during a finite, guest-origin delayed write.

    Finite backend latency avoids QEMU's synchronous unplug drain deadlocking
    an indefinitely suspended blkdebug request. The delay is fault latency;
    synchronization uses actual target-command and port-detach trace events.
    QEMU cancels/drains SCSI before USB object destruction; cancellation is
    evidence of removal, not a successful USB command completion.
    """
    def __init__(self,image,run,offset):
        super().__init__(image,run,'write',True)
        self.offset=offset
        self.active={}

    def configure(self,invocation,part):
        super().configure(invocation,part)
        for i,value in enumerate(invocation):
            if value!='-blockdev':continue
            node=json.loads(invocation[i+1])
            if node.get('node-name')=='secondary-debug':
                node['inject-error'][0]['delay-ns']=2_000_000_000
                invocation[i+1]=json.dumps(node)
                (part/'fault-definition.json').write_text(json.dumps(node,indent=2))
        events=part/'trace-events'
        events.write_text('bdrv_co_pwritev_part\nscsi_disk_new_request\nscsi_disk_dma_command_WRITE\nusb_msd_cmd_complete\nusb_msd_cmd_cancel\nusb_port_detach\n')
        invocation += ['-trace',f'events={events},file={part / "io-trace.log"}']

    def observe(self,qmp,text,part,started):
        if len(self.actions)==1 and not self.active:
            trace=(part/'io-trace.log').read_text(errors='replace')
            lines=trace.splitlines()
            starts=[i for i,line in enumerate(lines) if 'scsi_disk_dma_command_WRITE' in line and
                    f'(sector {self.offset//512}, count 8)' in line]
            if starts:
                if len(starts)!=1:raise ValueError('ambiguous target write trace')
                start=starts[0]
                requests=[line for line in lines[:start] if 'scsi_disk_new_request' in line]
                tag=re.search(r'tag=(0x[0-9a-f]+)',requests[-1])[1]
                if any(('usb_msd_cmd_complete' in line or 'usb_msd_cmd_cancel' in line) and
                       f'tag {tag}' in line for line in lines[start+1:]):
                    raise ValueError('missed active guest-origin request window')
                self.active=dict(target_offset=self.offset,tag=tag,request_trace=lines[start],
                                 pending_seconds=time.monotonic()-started,
                                 responsive_before_delete=qmp.call('query-status'))
                self.active['before_delete']=qmp.call('query-blockstats')
                (part/'active-io.json').write_text(json.dumps(self.active,indent=2))
                deletion=time.monotonic()
                qmp.call('device_del',{'id':'secondary-storage'})
                self.active['delete_response_seconds']=time.monotonic()-deletion
                self.active['responsive_after_delete']=qmp.call('query-status')
                qmp.wait_deleted('secondary-storage')
                self.physically_deleted=True
                trace=(part/'io-trace.log').read_text(errors='replace');lines=trace.splitlines()
                detaches=[i for i,line in enumerate(lines) if i>start and 'usb_port_detach' in line and 'port 3' in line]
                completions=[i for i,line in enumerate(lines) if i>start and
                    'usb_msd_cmd_complete' in line and f'tag {tag}' in line]
                if len(detaches)!=1 or (completions and min(completions)<detaches[0]):
                    raise ValueError('USB detached only after target request completion')
                self.active.update(port_detach_trace=lines[detaches[0]],
                                   usb_completion_before_detach=False,
                                   backend_drained_by_qemu=True,
                                   cancellation_traces=[line for line in lines[start+1:] if 'usb_msd_cmd_cancel' in line and f'tag {tag}' in line],
                                   detached_before_completion=True,completed_seconds=time.monotonic()-started)
                (part/'active-io.json').write_text(json.dumps(self.active,indent=2))
        super().observe(qmp,text,part,started)

    def finish(self,expected_failure,failed):
        if expected_failure or failed or not self.active.get('detached_before_completion') or len(self.actions)!=3:
            raise ValueError('incomplete active-I/O feasibility witnesses')


CHILD = [
    ('volume',0),('open',1),('owner-empty',0),('ready',1),('write',-33),('close',0),
    ('owner-online',1),('unresolved',1),('unmount-busy',-14),('uncertain',1),('clock',0),
    ('snapshot',0),('owner-offline',1),('owner-preserved',1),('detached',-2),
    ('devices',0),('absent',1),('retained',1)]
PARENT = [('volume',0),('spawn',1),('wait',0),('status',0),('close',0),('owner',1),('retained',1)]


def validate(text, run, events, actions, active=False, replacement=False):
    child_rows=list(CHILD)
    if replacement:
        at=child_rows.index(("ready",1))
        child_rows[at:at]=[("bind",0),("length",8193)]
        child_rows[child_rows.index(("write",-33))]=("replace",-33)
    rows = re.findall(r'^VOP (\d+) (recovery-(?:child|parent)-[\w-]+) expected=(-?\d+) observed=(-?\d+)$', text, re.M)
    wanted_count=len(child_rows)+len(PARENT)-(1 if active else 0)
    if len(rows)!=wanted_count or len(re.findall(r'^VOP ',text,re.M))!=wanted_count:
        raise ValueError('unknown or malformed recovery operation')
    for role, expected in (('child',[row for row in child_rows if not active or row[0]!='unmount-busy']),('parent',PARENT)):
        actual = [(int(i),n,int(w),int(g)) for i,n,w,g in rows if n.startswith('recovery-'+role+'-')]
        wanted = [(i,'recovery-'+role+'-'+n,w,w) for i,(n,w) in enumerate(expected,1)]
        if actual != wanted:
            raise ValueError('incorrect recovery '+role+' outcomes: '+repr(actual))
    owners = re.findall(r'^ROWNER stage=(online|returned|offline|exited) run=([0-9a-f]{16}) owner=(\d+) instance=(\d+) physical=(\d+) state=(\d+) flags=(\d+)$', text, re.M)
    if len(re.findall(r'^ROWNER ',text,re.M))!=3:
        raise ValueError('unknown or malformed recovery owner')
    if len(owners)!=3 or [o[0] for o in owners]!=['returned' if active else 'online','offline','exited']:
        raise ValueError('missing native recovery-owner witnesses')
    identities = {tuple(o[1:6]) for o in owners}
    if len(identities)!=1 or owners[0][1]!=run or int(owners[0][2])==0 or owners[0][5]!='2':
        raise ValueError('recovery identity/state changed')
    if [int(o[6]) for o in owners[1:]]!=[8,8] or int(owners[0][6]) not in ((3,8,9,10) if active else (3,)) or owners[0][3]==owners[0][4]:
        raise ValueError('incorrect physical-parent/liveness evidence')
    if len([e for e in events if e.get('event')=='DEVICE_DELETED' and
            e.get('data',{}).get('device')=='secondary-storage'])!=1:
        raise ValueError('missing matched physical deletion')
    if len(actions)!=3 or any(action.get('stage')!=i or
            action.get('marker')!=f'RECOVERY1 {name} run={run}' or
            action.get('qmp_status',{}).get('status')!='running'
            for i,(action,name) in enumerate(zip(actions,('ready','uncertain','retained')))):
        raise ValueError('missing QMP/guest handshake evidence')
    if len(re.findall(r'^RECOVERY1 ',text,re.M))!=4:
        raise ValueError('unknown recovery process marker')
    for role in ('parent','child'):
        for state in ('start','complete'):
            if text.count(f'RECOVERY1 {role}-{state} run={run}\n')!=1:
                raise ValueError('missing actual child/process completion')
    body = re.findall(r'GUESTFS1 phase=create run=[^\n]+\n.*?GUESTFS1 complete[^\n]+\n',text,re.S)
    if len(body)!=1:
        raise ValueError('missing independent system-root progress')
    identifier=re.search(r'identifier=(\d+)\n$',body[0])
    guest.validate_transcript(body[0],'create',run,int(identifier[1]))
    # A spawned child may run before the parent prints the returned handle.
    # Only the join/completion edges, not incidental scheduling, are ordered.
    ordered=[f'RECOVERY1 parent-start run={run}\n',f'RECOVERY1 child-start run={run}\n',
             f'RECOVERY1 child-complete run={run}\n','VOP 3 recovery-parent-wait ',
             body[0],f'RECOVERY1 parent-complete run={run}\n']
    positions=[text.index(item) for item in ordered]
    if positions!=sorted(positions):
        raise ValueError('incorrect child/wait/root completion ordering')
    remaining=text.replace(body[0],'',1)
    remaining=re.sub(r'^VOP [^\n]+\n|^ROWNER [^\n]+\n|^RECOVERY1 [^\n]+\n','',remaining,flags=re.M)
    if remaining:
        raise ValueError('unexpected recovery transcript content')
    return dict(owner_id=int(owners[0][2]), instance_id=int(owners[0][3]), physical_id=int(owners[0][4]),
                child_operations=len(child_rows)-(1 if active else 0), parent_operations=len(PARENT), states=['UNRESOLVED']*3)


def independent_payload(data,backend,path,image,output,log):
    if backend=='mgfs':return guest.payload(data,guest.find(data,path)[1])
    if backend=='exfat':return secondary.ExfatReader(data).find(path)['data']
    image.write_bytes(data)
    guest.command(['mcopy','-i',image,'::'+path,output],log)
    return output.read_bytes()

def campaign(args):
    folder=args.output
    folder.mkdir(parents=True)
    run=args.run_id or secrets.token_hex(8)
    report={'status':'FAIL','run':run,'kind':args.kind,'fault_injected':not args.negative_control,
            'accel':args.accel,'mutation':args.mutation,'replacement_phase':args.replacement_phase,'backend':args.backend}
    started=time.monotonic()
    try:
        backend=args.backend
        source_hash=None
        if args.source_media is not None:
            source_hash=guest.digest(args.source_media)
            image=folder/'secondary.img';shutil.copyfile(args.source_media,image)
            data=bytearray(secondary.partition(image))
            fixture_path=f'/work/safe-{run}/s8193.bin'
            from guest_safe_test import pattern
            initial_payload=pattern(run,8193)
        else:
            image=secondary.create_secondary(folder,args.build_dir,backend)
            data=bytearray(secondary.partition(image))
            work,_=guest.find(data,'/work')
            secondary.install(data,work,'fault.bin',guest.expected_payload(run),owner=1000,permissions=3)
            guest.check_image(args.build_dir,secondary.save(folder/'fault-initial.img',data),folder/'fault-initial-checker.log')
            secondary.write_partition(image,data)
            fixture_path='/work/fault.bin';initial_payload=guest.expected_payload(run)
        if independent_payload(data,backend,fixture_path,folder/'initial.img',folder/'initial-data.bin',folder/'initial-data.log')!=initial_payload:
            raise ValueError('independent initial fault-target content mismatch')
        report['initial_secondary_sha256']=guest.digest(image)
        report['source_media_sha256']=source_hash
        disk=secondary.prepare_boot(folder,args.build_dir,run,backend)
        with disk.open('rb') as stream:
            stream.seek(guest.ROOT_OFFSET);root=bytearray(stream.read(guest.PART_BYTES))
        mode='volume-read-failure' if args.kind=='read' else 'volume-active' if args.kind=='active' else 'volume-recovery'
        if args.mutation=='replace':mode='volume-replace-recovery'
        target=f'/vol/{secondary.LABELS[backend]}/work'
        suffix=(' '+fixture_path.removeprefix('/work/')) if args.source_media is not None else ''
        script=(f'/bin/fstest {mode} {run} {secondary.LABELS[backend]} {backend} {target}{suffix} > /temp/recovery.log\n'
                f'/bin/say child-exit-0-recovery-{run} > /temp/recovery.exit\n/bin/shutdown\n').encode()
        secondary.install(root,6,'recovery.shoot',script)
        secondary.write_partition(disk,root,guest.ROOT_OFFSET)
        guest.check_image(args.build_dir,secondary.save(folder/'recovery-initial-root.img',root),folder/'root-initial-checker.log')
        args.secondary_disk=image;args.secondary_readonly=False;args.qmp_pipe=True
        args.hotplug_run=None;args.confirmations=[] if args.negative_control or args.kind=='active' else ['y']
        args.expected_child_failure=args.negative_control
        # MGFS first publishes its dirty superblock before file data.
        data_offset=secondary.PART_OFFSET
        if args.kind=='read':
            extents,_=guest.image_tool.record_extents(data,guest.find(data,fixture_path)[1])
            if not extents or extents[0][0]!=0 or extents[0][2]<1 or extents[0][3] not in (1,2) or not (0<extents[0][1]<len(data)//4096):
                raise ValueError('unsupported independent read-fault extent')
            read_sector=(secondary.PART_OFFSET+extents[0][1]*4096)//512
            control=ReadControl(image,run,read_sector,not args.negative_control)
        elif args.kind=='active':control=ActiveControl(image,run,data_offset)
        else:control=RecoveryControl(image,run,args.kind,not args.negative_control)
        if args.replacement_phase!='stage':
            view=guest.layout(data)
            record_id,_=guest.find(data,fixture_path)
            record_offset=guest.image_tool.record_offset(data,view['record_table_start'],record_id,view['record_count'])
            byte_offset=record_offset//4096*4096 if args.replacement_phase=='publication' else view['allocation_bitmap_start']*4096
            sector=(secondary.PART_OFFSET+byte_offset)//512
            control=ReplacementSectorControl(image,run,sector,not args.negative_control)
            report['targeted_write_sector']=sector
        args.lifetime_control=control
        report['guest']=guest.boot(folder,disk,'recovery',args)
        report['actions']=control.actions
        if args.kind=='active':report['active_io']=control.active
        with disk.open('rb') as stream:
            stream.seek(guest.ROOT_OFFSET);root=stream.read(guest.PART_BYTES)
        # QEMU has stopped; no mutable image is inspected during execution.
        guest.check_image(args.build_dir,secondary.save(folder/'recovery-root.img',root),folder/'root-checker.log')
        text=guest.payload(root,guest.find(root,'/temp/recovery.log')[1]).decode('ascii')
        (folder/'native-transcript.log').write_text(text)
        final=secondary.partition(image)
        retained_image=folder/('confirmed-control-secondary.img' if args.negative_control or args.kind=='read' else 'uncertain-secondary.img')
        secondary.save(retained_image,final)
        report['final_secondary_sha256']=guest.digest(image)
        if args.mutation=='replace':
            actual=independent_payload(final,backend,fixture_path,folder/'failed-data.img',folder/'failed-data.bin',folder/'failed-data.log')
            expected=b'controlled-fault' if args.replacement_phase=='cleanup' or args.negative_control else initial_payload
            if actual!=expected:raise ValueError('independent replacement failure-state content mismatch')
            report['independent_failed_file_bytes']=len(actual)
            report['independent_failed_file_sha256']=hashlib.sha256(actual).hexdigest()
        if args.negative_control:
            if ('readfault-online-error expected=-12 observed=1\n' if args.kind=='read' else ('recovery-child-replace expected=-33 observed=0\n' if args.mutation=='replace' else 'recovery-child-write expected=-33 observed=16\n')) not in text:
                raise ValueError('negative control failed for a reason other than confirmed write success')
            try: guest.find(root,'/temp/recovery.exit')
            except ValueError as error:
                if str(error)!='namespace mismatch: /temp/recovery.exit':raise
            else: raise ValueError('failed child continued script')
            report['status']='EXPECTED_FAILURE'
        else:
            if guest.payload(root,guest.find(root,'/temp/recovery.exit')[1])!=f'child-exit-0-recovery-{run}\n'.encode():
                raise ValueError('missing successful waited parent completion')
            report['witnesses']=validate_read(text,run) if args.kind=='read' else validate(text,run,json.loads((folder/'recovery/events.json').read_text()),control.actions,args.kind=='active',args.mutation=='replace')
            item=guest.find(root,f'/temp/mgfs-{run}.bin')[1] if args.kind!='read' else None
            if item is not None and guest.payload(root,item)!=guest.expected_payload(run):
                raise ValueError('root progress content mismatch')
            report['status']='PASS'
        if args.kind=='read' and report['final_secondary_sha256']!=report['initial_secondary_sha256']:
            raise ValueError('read-fault campaign mutated secondary media')
        if args.kind=='read':
            targets=[entry for entry in report['guest']['qemu_blockstats'] if entry.get('node-name')=='secondary']
            if len(targets)!=1 or targets[0]['stats']['wr_operations'] or (
                    targets[0]['stats']['failed_rd_operations']!=0 if args.negative_control else
                    targets[0]['stats']['failed_rd_operations']<4):
                raise ValueError('missing actual native online read failures or unexpected write')
            report['read_fault_sector']=read_sector
            report['failed_read_requests']=targets[0]['stats']['failed_rd_operations']
        # Checker success is recorded, not mistaken for resolution of runtime
        # uncertainty. Failed media is preserved without repair or another boot.
        import subprocess
        checker=[str(args.build_dir/'mgfsck'),str(retained_image)] if backend=='mgfs' else ['/usr/sbin/fsck.'+('fat' if backend=='fat32' else 'exfat'),'-n',str(retained_image)]
        checked=subprocess.run(checker,
                               text=True,capture_output=True,timeout=30)
        (folder/'uncertain-checker.log').write_text(checked.stdout+checked.stderr)
        report['checker_exit']=checked.returncode
        report['uncertain_media_retained']=not args.negative_control and args.kind!='read'
        report['retained_partition']=retained_image.name
        if source_hash is not None and source_hash!=guest.digest(args.source_media):raise ValueError('read-only fault fixture source changed')
    finally:
        report['seconds']=time.monotonic()-started
        report['artifact_bytes']=sum(p.stat().st_size for p in folder.rglob('*') if p.is_file() and not p.is_fifo())
        (folder/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--backend',choices=('mgfs','fat32','exfat'),default='mgfs')
    parser.add_argument('--source-media',type=Path,help='copy a previously qualified disposable safe-file image; source is never attached/written')
    parser.add_argument('--kind',choices=('read','write','flush','active'),default='write')
    parser.add_argument('--negative-control',action='store_true')
    parser.add_argument('--mutation',choices=('write','replace'),default='write')
    parser.add_argument('--replacement-phase',choices=('stage','publication','cleanup'),default='stage')
    parser.add_argument('--run-id')
    parser.add_argument('--accel',choices=('tcg','kvm'),default='tcg')
    parser.add_argument('--qemu',default='qemu-system-x86_64')
    parser.add_argument('--timeout',type=int,default=180)
    parser.add_argument('--ovmf-code',type=Path,default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars',type=Path,default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    args=parser.parse_args();args.output=args.output.resolve();args.build_dir=args.build_dir.resolve()
    if args.source_media is not None and (args.mutation!='replace' or not args.run_id or args.kind not in ('write','flush')):parser.error('qualified source requires replacement write/flush and exact run-id')
    if args.backend!='mgfs' and (args.source_media is None or args.replacement_phase!='stage'):parser.error('FAT profiles require copied qualified media and staging faults')
    if args.replacement_phase!='stage' and (args.mutation!='replace' or args.kind!='write'):parser.error('sector phases require replacement write faults')
    if args.mutation=='replace' and args.kind in ('read','active'):parser.error('replacement faults use write/flush injection')
    if args.negative_control and args.kind=='active':parser.error('active feasibility has explicit suspension witnesses')
    if args.output.exists() or not 30<=args.timeout<=300:
        parser.error('new disposable output directory and bounded timeout required')
    if args.run_id and not re.fullmatch('[0-9a-f]{16}',args.run_id):
        parser.error('run-id must contain 16 lowercase hexadecimal digits')
    campaign(args)

if __name__=='__main__': main()
