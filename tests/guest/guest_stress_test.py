#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standalone native C stress qualification using existing disposable guest boot paths."""
import argparse,hashlib,json,re,secrets,shutil,subprocess,sys,time,resource,copy
from pathlib import Path
sys.dont_write_bytecode=True
import guest_mgfs_test as guest
import guest_secondary_test as secondary
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1] / "shared"))
import artifact_lifecycle as artifacts
from fsstress_oracle import decode,encode,model,validate_log,ACTIONS

def failing_prefix(folder,text,trace):
    failed=re.search(r'^FSFAILED first=(\d+)',text,re.M)
    if failed is None:failed=re.search(r'^FSFAIL op=(\d+)',text,re.M)
    if failed and 0<int(failed[1])<=trace['count']:
        (folder/'failing-prefix.trace').write_bytes(encode(trace,trace['ops'][:int(failed[1])]))

def prepare(folder,build,seed,backend,count,host):
    disk=secondary.prepare_boot(folder,build,seed,backend)
    with disk.open('rb') as f:f.seek(guest.ROOT_OFFSET);image=bytearray(f.read(guest.PART_BYTES))
    guest.install(image,2,'fsstress',(build/'fsstress/fsstress.elf').read_bytes())
    for phase,mode in [('create','create'),('verify','verify'),('negative','check'),('model','model'),('status','status'),('prefix','prefix'),('incomplete','prefix'),('fault','fault')]:
        script=(f'/bin/fstest volume-stress-{mode} {seed} {secondary.LABELS[backend]} {backend} /vol/{secondary.LABELS[backend]}/work {count} > /temp/{phase}.log\n'
                f'/bin/say child-exit-0-{phase}-{seed} > /temp/{phase}.exit\n/bin/shutdown\n').encode()
        secondary.replace(image,f'/temp/{phase}.shoot',script) if phase in ('create','verify','negative') else guest.install(image,6,phase+'.shoot',script)
    planned=folder/'planned.trace'
    guest.command([host,'plan',backend,seed,str(count),planned],folder/'plan.log')
    trace=decode(planned.read_bytes());bad=dict(trace,control=1)
    guest.install(image,6,'prefix-'+seed+'.trace',encode(bad,trace['ops'][:6]))
    secondary.write_partition(disk,image,guest.ROOT_OFFSET)
    guest.check_image(build,secondary.save(folder/'stress-initial-root.img',image),folder/'stress-initial-checker.log')
    return disk,trace

def root_image(folder,disk,phase,build):
    with disk.open('rb') as f:f.seek(guest.ROOT_OFFSET);root=f.read(guest.PART_BYTES)
    guest.check_image(build,secondary.save(folder/(phase+'-root.img'),root),folder/(phase+'-root-checker.log'))
    text=guest.payload(root,guest.find(root,'/temp/'+phase+'.log')[1]).decode('ascii')
    (folder/(phase+'-native.log')).write_text(text)
    return root,text

def sections(text):
    # A child may print before the parent reports spawn's returned handle.
    # Supervisor records are separately checked, not a tool operation stream.
    text=re.sub(r'^TSTEP [^\n]*\n','',text,flags=re.M)
    return re.findall(r'^FSBEGIN [^\n]+\n.*?^(?:FSPASS|FSVERIFIED|FSFAILED|FSINCOMPLETE|FSUNCERTAIN)[^\n]*\n',text,re.M|re.S)

def environment(text,phase,backend,seed,negative=False,fault=False):
    mode='check' if phase=='negative' else 'prefix' if phase=='incomplete' else phase
    first=f'VOLUME1 phase=volume-stress-{mode} run={seed} label={secondary.LABELS[backend]} filesystem={backend} target=/vol/{secondary.LABELS[backend]}/work\n'
    if not text.startswith(first):raise ValueError('wrong native supervisor header')
    identity=re.findall(r'^VOLUME1 identity instance=([1-9]\d*) parent=([1-9]\d*) name=\S+ mount=/vol/'+secondary.LABELS[backend]+'$',text,re.M)
    if len(identity)!=1 or identity[0][0]==identity[0][1]:raise ValueError('missing device identity')
    child=f'STRESSCHILD phase={mode} run={seed} instance={identity[0][0]} parent={identity[0][1]}\n'
    if text.count(child)!=1:raise ValueError('missing native supervision')
    rows=re.findall(r'^TSTEP (\d+) ([\w-]+) expected=(-?\d+) observed=(-?\d+)$',text,re.M)
    if len(re.findall('^TSTEP ',text,re.M))!=len(rows):raise ValueError('malformed child result')
    launch=[('spawn',1),('wait',0),('process-close',0),('status',0)]
    expected=[('owner-snapshot',0),('owners',0)]
    expected+=launch*(3 if phase=='verify' else 1)
    if not negative and not fault:expected += [('owner-snapshot',0),('owners',0)]
    if fault:expected[-1]=('status',44);expected += [('owner-snapshot',0),('owner-state',2),('owner-parent',int(identity[0][1])),('owners',1),('uncertain-unmount',-14)]
    wanted=[(str(i),name,str(n),str(n)) for i,(name,n) in enumerate(expected,1)]
    if negative:wanted[-1]=(wanted[-1][0],'status','0',str(2 if phase=='incomplete' else 43))
    if rows!=wanted:raise ValueError('wrong actual process/owner results')
    completed=list(re.finditer(r'^FS(?:PASS|VERIFIED|FAILED|INCOMPLETE|UNCERTAIN)[^\n]*\n',text,re.M))
    launches=3 if phase=='verify' else 1
    if len(completed)!=launches:raise ValueError('missing child completion record')
    for i,done in enumerate(completed):
        if done.end()>text.index(f'TSTEP {4+4*i} wait '):raise ValueError('wait reported before child completion')
    startup=secondary.expected_vops('create' if phase in ('create','model','status') else 'verify',backend)
    end=startup.index(('seed-close',0))+1;vrows=startup[:end]
    if not negative and not fault:vrows += [('final-unmount',0),('final-detached',-2),('final-snapshot',0),('final-suppressed',0)]
    actual=re.findall(r'^VOP (\d+) ([\w-]+) expected=(-?\d+) observed=(-?\d+)$',text,re.M)
    if actual!=[(str(i),name,str(n),str(n)) for i,(name,n) in enumerate(vrows,1)]:raise ValueError('volume admission/unmount mismatch')
    if not negative:
        if f'STRESSCHILD complete phase={mode} run={seed} steps={len(expected)}\n' not in text or not text.endswith(f'VOLUME1 complete phase=volume-stress-{mode} run={seed} ops={len(vrows)}\n'):raise ValueError('missing completed supervision')
    elif 'STRESSCHILD complete' in text or 'VOLUME1 complete' in text:raise ValueError('negative operation falsely completed')
    remaining=re.sub(r'^TSTEP [^\n]*\n','',text,flags=re.M)
    for block in sections(text):remaining=remaining.replace(block,'',1)
    remaining=re.sub(r'^(?:VOLUME1 |VOP |STRESSCHILD |STRESSOWNER )[^\n]*\n','',remaining,flags=re.M)
    if remaining:raise ValueError('unknown or incomplete native transcript')
    return dict(instance=int(identity[0][0]),parent=int(identity[0][1]),native_steps=len(expected),volume_steps=len(vrows))

def inspect(folder,disk,backend,build,trace,phase,replayed=False,wrong=None):
    data=secondary.partition(disk);part=secondary.save(folder/(phase+'-partition.img'),data)
    identity=secondary.filesystem_identity(data,backend)
    if identity!=secondary.filesystem_identity((folder/'initial-secondary.img').read_bytes(),backend):raise ValueError('filesystem identity changed')
    log=folder/(phase+'-checker.log')
    if backend=='mgfs':guest.check_image(build,part,log)
    else:guest.command(['/usr/sbin/fsck.'+('fat' if backend=='fat32' else 'exfat'),'-n',part],log)
    reader=secondary.ExfatReader(data) if backend=='exfat' else None
    def listing(path):
        if backend=='mgfs':
            rows=[(e[4].decode(),int(guest.image_tool.u64(guest.find(data,path+'/'+e[4].decode())[1],0))) for e in guest.image_tool.directory_entries(guest.payload(data,guest.find(data,path)[1])) if e[3]&1]
            if len({n for n,_ in rows})!=len(rows):raise ValueError('duplicate independent MGFS namespace')
            return dict(rows)
        if backend=='exfat':return {n:2 if f['directory'] else 1 for n,f in reader.entries(reader.find(path)['data']).items()}
        target=folder/(phase+'-'+path.replace('/','_')+'-list.log');guest.command(['mdir','-b','-i',part,'::'+path],target)
        rows=[(line.rstrip('/').rsplit('/',1)[-1],2 if line.endswith('/') else 1) for line in target.read_text().splitlines() if line.startswith('::')]
        if len({n for n,_ in rows})!=len(rows):raise ValueError('duplicate independent FAT namespace')
        return dict(rows)
    def content(path):
        if backend=='mgfs':return guest.payload(data,guest.find(data,path)[1])
        if backend=='exfat':return reader.find(path)['data']
        target=folder/(phase+'-'+path.replace('/','_')+'-data.bin');guest.command(['mcopy','-o','-i',part,'::'+path,target],log);return target.read_bytes()
    files,dirs=model(trace);seed=f"{trace['seed']:016x}";hashes={}
    siblings={'seed.txt':1,'stress-'+seed:2}
    if replayed:siblings['replay-'+seed]=2
    if listing('/work')!=siblings or content('/work/seed.txt')!=secondary.SEED:raise ValueError('unrelated parent namespace/content changed')
    for base in [f'/work/stress-{seed}',*([f'/work/replay-{seed}'] if replayed else [])]:
        for directory in sorted(dirs):
            wanted={p.rsplit('/',1)[-1]:2 for p in dirs if p and p.rpartition('/')[0]==directory}
            wanted.update({p.rsplit('/',1)[-1]:1 for p in files if p.rpartition('/')[0]==directory})
            if listing(base+('/'+directory if directory else ''))!=wanted:raise ValueError('independent namespace mismatch '+base+'/'+directory)
        for name,want in files.items():
            actual=content(base+'/'+name)
            if actual!=want:
                at=next((i for i,(a,b) in enumerate(zip(actual,want)) if a!=b),min(len(actual),len(want)))
                if wrong!=(base+'/'+name,at):raise ValueError(f'independent content mismatch {base}/{name} offset={at}')
            hashes[base+'/'+name]=hashlib.sha256(actual).hexdigest()
    return dict(checker='clean',sha256=hashlib.sha256(data).hexdigest(),files=hashes,filesystem_identity=identity)

def corrupt(folder,disk,backend,trace):
    files,_=model(trace);name,want=next((n,b) for n,b in files.items() if b);path=f"/work/stress-{trace['seed']:016x}/"+name
    data=bytearray(secondary.partition(disk));index=len(want)//2
    if backend=='mgfs':
        extents,_=guest.image_tool.record_extents(data,guest.find(data,path)[1]);logical=index//4096
        matches=[e for e in extents if e[0]<=logical<e[0]+e[2]]
        if len(matches)!=1:raise ValueError('wrong-content mapping outside declared layout')
        e=matches[0];at=(e[1]+logical-e[0])*4096+index%4096;data[at]^=1
    elif backend=='exfat':
        r=secondary.ExfatReader(data);entry=r.find(path);first=secondary.u32(entry['entry'],52);cluster=first;which=index//r.cb
        if entry['entry'][33]&2:cluster+=which
        else:
            for _ in range(which):cluster=secondary.u32(data,r.fat+cluster*4)
        data[r.address(cluster)+index%r.cb]^=1
    else:
        image=secondary.save(folder/'wrong-fat.img',data);body=bytearray(want);body[index]^=1;payload=secondary.save(folder/'wrong.bin',body)
        guest.command(['mcopy','-o','-i',image,payload,'::'+path],folder/'wrong-copy.log');data=bytearray(image.read_bytes())
    secondary.write_partition(disk,data);return path,index

def campaign(args,backend,seed,index):
    folder=args.output/f'{backend}-{index}';folder.mkdir();run=f'{seed:016x}'
    disk,planned=prepare(folder,args.build_dir,run,backend,args.operations,args.host)
    secondary_disk=secondary.create_secondary(folder,args.build_dir,backend)
    shutil.copyfile(secondary_disk,folder/'original-secondary.img')
    args.secondary_disk=secondary_disk;args.secondary_readonly=False;args.confirmations=['y'];result=dict(backend=backend,seed=run,status='FAIL',phases={})
    result['initial_secondary_sha256']=guest.digest(secondary_disk);trace=None
    for phase in ('create','verify'):
        args.expected_child_failure=False;result['active_phase']=phase
        try:boot=guest.boot(folder,disk,phase,args)
        except Exception:
            # boot() always terminates/waits for QEMU before returning or raising.
            # Preserve the shortest observed prefix even when Shoot rejects the
            # failed child before normal transcript validation can run.
            try:
                _,failed_text=root_image(folder,disk,phase,args.build_dir)
                failing_prefix(folder,failed_text,planned)
            except Exception as evidence_error:
                (folder/'failure-evidence-error.txt').write_text(str(evidence_error))
            raise
        root,text=root_image(folder,disk,phase,args.build_dir)
        failing_prefix(folder,text,planned)
        native=environment(text,phase,backend,run)
        if guest.payload(root,guest.find(root,f'/temp/{phase}.exit')[1])!=f'child-exit-0-{phase}-{run}\n'.encode():raise ValueError('missing actual waited completion')
        trace_data=guest.payload(root,guest.find(root,f'/temp/stress-{run}.trace')[1]);observed=decode(trace_data)
        if observed!=planned:raise ValueError('native generator diverges from independently staged host plan')
        (folder/'executed.trace').write_bytes(trace_data);trace=observed
        blocks=sections(text)
        if len(blocks)!=(1 if phase=='create' else 3):raise ValueError('missing tool execution')
        for block,(mode,name) in zip(blocks,(('execute','stress'),) if phase=='create' else (('verify','stress'),('execute','replay'),('verify','replay'))):
            validate_log(block,trace,mode,root=f'/vol/{secondary.LABELS[backend]}/work/{name}-{run}')
        media=inspect(folder,secondary_disk,backend,args.build_dir,trace,phase,phase=='verify')
        result['phases'][phase]=dict(guest=boot,native=native,media=media)
    if args.controls and index==0:
        wrong=corrupt(folder,secondary_disk,backend,trace)
        try:inspect(folder,secondary_disk,backend,args.build_dir,trace,'host-negative',True)
        except ValueError as error:
            if f'content mismatch {wrong[0]} offset={wrong[1]}' not in str(error):raise
        else:raise ValueError('host oracle accepted wrong bytes')
        args.expected_child_failure=True;args.confirmations=[]
        boot=guest.boot(folder,disk,'negative',args);root,text=root_image(folder,disk,'negative',args.build_dir);environment(text,'negative',backend,run,True)
        if not re.search(r'^FSFAIL op=0 kind=content file=\d+ offset='+str(wrong[1])+r' expected=\d+ observed=\d+$',text,re.M):raise ValueError('wrong persisted-byte failure')
        if 'FSPASS' in text or 'FSVERIFIED' in text:raise ValueError('incorrect negative success')
        inspect(folder,secondary_disk,backend,args.build_dir,trace,'after-negative',True,wrong)
        result['wrong_content']=dict(status='EXPECTED_FAILURE',path=wrong[0],offset=wrong[1],guest=boot)
    del result['active_phase'];result['status']='PASS'
    (folder/'result.json').write_text(json.dumps(result,indent=2));return result

class StressFaultControl:
    def __init__(self,image,seed,kind):
        from guest_lifetime_test import RecoveryControl
        self.transport=RecoveryControl(image,seed,kind);self.seed=seed;self.kind=kind;self.actions=[]
    def configure(self,invocation,part):self.transport.configure(invocation,part)
    def observe(self,qmp,text,part,started):
        marker=f'HOTPLUG1 stress-ready run={self.seed}'
        if self.actions or marker not in text:return
        stats=qmp.call('query-blockstats');selected=[s for s in stats if s.get('node-name')=='secondary']
        if len(selected)!=1 or any(selected[0]['stats'][k] for k in ('wr_operations','wr_bytes','flush_operations','failed_wr_operations','failed_flush_operations')):raise ValueError('fault not aligned before stress mutation')
        self.actions.append(dict(marker=marker,blockstats=stats,seconds=time.monotonic()-started))
        (part/'stress-ready.txt').write_text(text)
        qmp.call('human-monitor-command',{'command-line':'sendkey ret 30'})
    def finish(self,expected_failure,failed):
        if expected_failure or failed or len(self.actions)!=1:raise ValueError('incomplete stress fault handshake')

def control_campaign(args,mode,kind='write'):
    folder=args.output/('control-'+mode+('-'+kind if mode=='fault' else ''));folder.mkdir()
    seed='0000000000000000';backend='mgfs'
    disk,planned=prepare(folder,args.build_dir,seed,backend,args.operations,args.host)
    secondary_disk=secondary.create_secondary(folder,args.build_dir,backend)
    if mode=='incomplete':
        with disk.open('rb') as f:f.seek(guest.ROOT_OFFSET);initial=bytearray(f.read(guest.PART_BYTES))
        secondary.replace(initial,'/temp/prefix-'+seed+'.trace',encode(planned,planned['ops'][:1]))
        secondary.write_partition(disk,initial,guest.ROOT_OFFSET)
    a=copy.copy(args);a.secondary_disk=secondary_disk;a.secondary_readonly=False;a.confirmations=['y'] if mode=='fault' else []
    a.expected_child_failure=mode!='fault';a.qmp_pipe=mode=='fault'
    if mode=='fault':a.lifetime_control=StressFaultControl(secondary_disk,seed,kind)
    before=guest.digest(secondary_disk);boot=guest.boot(folder,disk,mode,a)
    root,text=root_image(folder,disk,mode,args.build_dir)
    native=environment(text,mode,backend,seed,mode!='fault',mode=='fault')
    if mode in ('model','status','prefix'):
        name='prefix' if mode=='prefix' else mode
        trace=decode(guest.payload(root,guest.find(root,f'/temp/{name}-{seed}.trace')[1]))
        blocks=sections(text)
        if len(blocks)!=1:raise ValueError('missing actual failing stress execution')
        validate_log(blocks[0],trace,failure='model' if mode=='prefix' else mode,root=f'/vol/QUAL_MGFS/work/{name}-{seed}')
        (folder/'failing-prefix.trace').write_bytes(encode(trace,trace['ops'][:6]))
    elif mode=='incomplete':
        if 'FSINCOMPLETE kind=coverage' not in text or 'FSPASS' in text or 'FSFAILED' in text:raise ValueError('wrong incomplete result')
        if len(re.findall('^FSOP ',text,re.M))!=1:raise ValueError('incomplete prefix did not execute exactly one operation')
    else:
        failing_prefix(folder,text,planned)
        if 'FSUNCERTAIN retained-by-filesystem=required' not in text or not re.search(r'^FSOP 1 action=root .*expected=0 observed=-33 count=0 observed_count=0$',text,re.M) or 'FSPASS' in text:raise ValueError('wrong native uncertain operation')
        owners=re.findall(r'^STRESSOWNER id=([1-9]\d*) instance=(\d+) parent=(\d+) state=2 flags=3$',text,re.M)
        if len(owners)!=1 or tuple(map(int,owners[0][1:]))!=(native['instance'],native['parent']):raise ValueError('missing genuine retained owner')
        stats=[s for s in boot['qemu_blockstats'] if s.get('node-name')=='secondary']
        field='failed_wr_operations' if kind=='write' else 'failed_flush_operations'
        if len(stats)!=1 or stats[0]['stats'][field]!=1:raise ValueError('missing intended QEMU fault')
        if guest.payload(root,guest.find(root,f'/temp/{mode}.exit')[1])!=f'child-exit-0-{mode}-{seed}\n'.encode():raise ValueError('missing waited fault supervisor')
    if mode!='fault':
        try:guest.find(root,f'/temp/{mode}.exit')
        except ValueError as error:
            if str(error)!='namespace mismatch: /temp/'+mode+'.exit':raise
        else:raise ValueError('failed stress child continued its script')
    data=secondary.partition(secondary_disk);image=secondary.save(folder/'final-partition.img',data)
    checked=subprocess.run([str(args.build_dir/'mgfsck'),str(image)],text=True,capture_output=True,timeout=30)
    (folder/'checker.log').write_text(checked.stdout+checked.stderr)
    if mode!='fault' and checked.returncode:raise ValueError('model/control changed structurally valid media')
    result=dict(status='PASS' if mode=='fault' else 'EXPECTED_FAILURE',mode=mode,kind=kind,native=native,guest=boot,checker_exit=checked.returncode,initial_media_sha256=before,final_media_sha256=guest.digest(secondary_disk),uncertain_media_preserved=mode=='fault')
    (folder/'result.json').write_text(json.dumps(result,indent=2));return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--host',type=Path,required=True)
    p.add_argument('--output',type=Path,help='new output directory; retained for review (default: temporary)')
    artifacts.add_retention_argument(p)
    p.add_argument('--backend',choices=('mgfs','fat32','exfat','all'),default='all');p.add_argument('--operations',type=int,default=96);p.add_argument('--seed',action='append',help='unsigned 16-digit hexadecimal seed; default fixed zero plus a recorded generated seed')
    p.add_argument('--extra-controls',action='store_true',help='MGFS real model/status/failing-prefix/incomplete controls and write/flush uncertainty');p.add_argument('--controls',action='store_true');p.add_argument('--accel',choices=('tcg','kvm'),default=guest.default_accel());p.add_argument('--qemu',default='qemu-system-x86_64');p.add_argument('--timeout',type=int,default=600)
    p.add_argument('--ovmf-code',type=Path,default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'));p.add_argument('--ovmf-vars',type=Path,default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    a=p.parse_args()
    error=guest.accel_error(a.accel,a.qemu)
    if error: p.error(error)
    a.build_dir=a.build_dir.resolve();a.host=a.host.resolve()
    for path in (a.output,a.ovmf_code,a.ovmf_vars):
        if path is None:continue
        if ',' in str(path) or '\n' in str(path):p.error('QEMU image paths cannot contain commas or newlines')
    if a.extra_controls and a.backend not in ('mgfs','all'):p.error('extra controls require MGFS profile')
    if (a.output is not None and a.output.exists()) or not 64<=a.operations<=256 or not 60<=a.timeout<=600:p.error('new output and bounded guest configuration required')
    seeds=a.seed or ['0000000000000000',secrets.token_hex(8)]
    if len(seeds)>4 or any(not re.fullmatch('[0-9a-f]{16}',s) for s in seeds):p.error('1..4 explicit hexadecimal seeds required')
    workspace=artifacts.ArtifactWorkspace(a.output,name='guest-stress',retain=a.retain_artifacts)
    a.output=workspace.path
    report=dict(status='FAIL',profiles=[],seeds=seeds,operations=a.operations,accel=a.accel);started=time.monotonic();cpu=time.process_time()
    try:
        campaigns=len(seeds)*(3 if a.backend=='all' else 1)+(6 if a.extra_controls else 0)
        if shutil.disk_usage(a.output).free<2*1024**3*campaigns:raise ValueError('insufficient artifact capacity')
        report['binaries']={path:guest.digest(a.build_dir/path) for path in ('Mangrove/pith.elf','fstest/fstest.elf','fsstress/fsstress.elf')}
        for backend in ('mgfs','fat32','exfat') if a.backend=='all' else (a.backend,):
            for i,s in enumerate(seeds):report['profiles'].append(campaign(a,backend,int(s,16),i))
        if a.extra_controls:
            report['controls']=[control_campaign(a,mode) for mode in ('model','status','prefix','incomplete')]
            report['controls'] += [control_campaign(a,'fault',kind) for kind in ('write','flush')]
        report['status']='PASS'
    except Exception as error:report['error']=f'{type(error).__name__}: {error}';raise
    finally:
        report.update(seconds=time.monotonic()-started,runner_cpu_seconds=time.process_time()-cpu,runner_peak_rss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024,artifact_bytes=sum(p.stat().st_size for p in a.output.rglob('*') if p.is_file()))
        report=workspace.finish(report,verified=report.get('status')=='PASS')
        print(json.dumps(report,indent=2))
if __name__=='__main__':main()
