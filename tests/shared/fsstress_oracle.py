# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent trace/content/namespace oracle; no driver or C-model calls."""
import struct
MASK=(1<<64)-1
ACTIONS=('root','mkdir','create','write','read','reopen','truncate','move','unlink','rmdir','snapshot','replace','stale','contents','case','resize','directory-move')
CAPS=(1,7,7,10)
def random(state):
    state=(state+0x9e3779b97f4a7c15)&MASK;x=state
    x=((x^(x>>30))*0xbf58476d1ce4e5b9)&MASK
    x=((x^(x>>27))*0x94d049bb133111eb)&MASK
    return state,x^(x>>31)
def payload(seed,count):
    out=bytearray()
    for _ in range(count):seed,value=random(seed);out.append(value&255)
    return bytes(out)
def checksum(data):
    h=14695981039346656037
    for byte in data:h=((h^byte)*1099511628211)&MASK
    return h
def decode(data):
    if len(data)<48 or data[:8]!=b'MGFSX001':raise ValueError('trace header')
    seed,profile,caps,count,control,digest,reserved=struct.unpack_from('<QIIIIQQ',data,8)
    if profile>3 or caps!=CAPS[profile] or not 1<=count<=1024 or len(data)!=48+32*count or control>2 or reserved:raise ValueError('trace bounds')
    if checksum(data[:32]+bytes(8)+data[40:])!=digest:raise ValueError('trace checksum')
    ops=[struct.unpack_from('<IIIIIiQ',data,48+i*32) for i in range(count)]
    result=dict(seed=seed,profile=profile,caps=caps,count=count,control=control,ops=ops)
    model(result)  # Reject domain inconsistencies before executing a replay.
    return result

def encode(trace,ops=None):
    ops=trace['ops'] if ops is None else ops
    data=bytearray(b'MGFSX001'+struct.pack('<QIIIIQQ',trace['seed'],trace['profile'],trace['caps'],len(ops),trace['control'],0,0))
    for o in ops:data+=struct.pack('<IIIIIiQ',*o)
    struct.pack_into('<Q',data,32,checksum(data));return bytes(data)

def model(trace,events=None):
    files={};dirs={};root=False;caps=trace['caps']
    def directory(d):
        pieces=[];seen=set()
        while d!=4:
            if d in seen or d not in dirs:raise ValueError('directory chain')
            seen.add(d);pieces.append('directory-'+str(d));d=dirs[d]
        return '/'.join(reversed(pieces))
    def filename(i,parent=None,variant=None):
        f=files.get(i)
        parent=f[0] if parent is None else parent;variant=f[1] if variant is None else variant
        return '/'.join(filter(None,(directory(parent),('moved' if variant else 'ordinary')+'-entry-'+str(i)+'.bin')))
    for number,o in enumerate(trace['ops'],1):
        action,i,arg,offset,length,want,seed=o
        if action>=len(ACTIONS) or i>=8 or arg>4 or length>65536 or offset>65536:raise ValueError('operation bounds')
        live=lambda p:p==4 and root or p in dirs
        f=files.get(i);status=0
        if action==0:status=-9 if root else 0
        elif action==1:
            if i>=4 or not live(arg) or i in dirs and dirs[i]!=arg:raise ValueError('directory domain')
            status=-9 if i in dirs else 0
        elif action in (2,13):
            if not live(arg):raise ValueError('parent domain')
            if action==13 and not caps&1:status=-7
            else:
                if f and (f[0]!=arg or f[1]):raise ValueError('create domain')
                status=-9 if f else 0
        elif action==9:
            if i not in dirs:raise ValueError('rmdir domain')
            status=-16 if any(x[0]==i for x in files.values()) or i in dirs.values() else 0
        elif action==16:
            if i not in dirs or not live(arg) or dirs[i]==arg:raise ValueError('move domain')
            p=arg;seen=set()
            while p!=4:
                if p==i or p in seen or p not in dirs:raise ValueError('ancestry')
                seen.add(p);p=dirs[p]
        else:
            if f is None:raise ValueError('file domain')
            if action==3 and (offset+length>65536 or length and offset>len(f[2]) and not caps&2):raise ValueError('write domain')
            if action==7 and not live(arg):raise ValueError('move parent')
            if action in (10,11,12):status=-7 if not caps&1 else -34 if action==12 else 0
            if action==14:status=0 if caps&4 else -2
            if action==15:status=0 if not length or caps&8 else -7
        if status!=want:raise ValueError(f'independent predicted status op={number}: {status}!={want}')
        if events is not None:
            source='';dest=None
            if action==1:source='/'.join(filter(None,(directory(arg),'directory-'+str(i))))
            elif action in (2,13):source=filename(i,arg,0)
            elif action in (9,16):source=directory(i)
            elif action!=0:source=filename(i)
            if action==7:dest=filename(i,arg,f[1]^1)
            if action==16:dest='/'.join(filter(None,(directory(arg),'directory-'+str(i))))
            if action==14:source=source.upper() # Declared names are ASCII only.
            count=length if action==3 else min(length,max(0,len(f[2])-offset)) if action==4 else 0
            events.append(dict(source=source,destination=dest,count=count,status=status,old_size=len(f[2]) if f else 0))
        if status and status!=-34:continue
        if action==0:root=True
        elif action==1:dirs[i]=arg
        elif action in (2,13):files[i]=[arg,0,payload(seed,length) if action==13 else b'']
        elif action==3 and length:
            old=f[2];f[2]=old[:offset]+bytes(max(0,offset-len(old)))+payload(seed,length)+old[offset+length:]
        elif action==6:f[2]=b''
        elif action==7:f[0]=arg;f[1]^=1
        elif action==8:del files[i]
        elif action==9:del dirs[i]
        elif action in (11,12):f[2]=payload(seed,length)
        elif action==15:f[2]=f[2][:length]+bytes(max(0,length-len(f[2])))
        elif action==16:dirs[i]=arg
    paths={directory(d) for d in dirs};paths.add('')
    contents={}
    for i,(parent,variant,body) in files.items():
        name=('moved' if variant else 'ordinary')+'-entry-'+str(i)+'.bin'
        contents['/'.join(filter(None,(directory(parent),name)))]=body
    return contents,paths

def validate_log(text,trace,mode='execute',failure=None,root=None):
    import re
    header=f"FSBEGIN version=1 mode={mode} profile={trace['profile']} caps={trace['caps']} seed={trace['seed']:016x} ops={trace['count']} control={trace['control']} max_file=65536 model_bytes="
    if not text.startswith(header) or text.count(header)!=1:raise ValueError('missing or wrong tool header')
    rows=re.findall(r'^FSOP (\d+) action=([\w-]+) id=(\d+) parent=(\d+) offset=(\d+) length=(\d+) data=([0-9a-f]{16}) path=(\S+) destination=(\S+) expected=(-?\d+) observed=(-?\d+) count=(\d+) observed_count=(\d+)$',text,re.M)
    if len(re.findall('^FSOP ',text,re.M))!=len(rows):raise ValueError('malformed operation')
    if failure is None and mode=='execute' and len(rows)!=trace['count']:raise ValueError('incomplete execution')
    predicted=[];model(trace,predicted)
    for index,row in enumerate(rows):
        sequence,name,ident,parent,offset,length,data,path,dest,want,got,count,actual=row
        op=trace['ops'][index];action,i,arg,at,n,expected,seed=op
        if (int(sequence),name,int(ident),int(parent),int(offset),int(length),int(data,16))!=(index+1,ACTIONS[action],i,arg,at,n,seed):raise ValueError('trace/operation mismatch')
        event=predicted[index]
        if root is not None:
            if path!=root+('/'+event['source'] if event['source'] else '') or dest!=(root+'/'+event['destination'] if event['destination'] is not None else '-'):raise ValueError('operation pathname mismatch')
        requested=-9 if failure=='status' and index==5 else expected
        if int(want)!=requested or int(got)!=expected or int(count)!=event['count'] or count!=actual:raise ValueError('status/count mismatch')
    if failure is None:
        allowed=r'^FS(?:BEGIN [^\n]+|OP [^\n]+|PASS [^\n]+|VERIFIED [^\n]+)\n'
        if re.sub(allowed,'',text,flags=re.M):raise ValueError('unknown tool output')
        if 'FSFAIL' in text or 'FSINCOMPLETE' in text or 'FSUNCERTAIN' in text:raise ValueError('unsuccessful execution')
        if mode=='execute':
            matches=re.findall(r'^FSPASS ops=(\d+) checks=(\d+) mutations=(\d+) witnesses=(\d+) ranges=(\d+) elapsed_ms=(\d+)$',text,re.M)
            if len(matches)!=1 or int(matches[0][0])!=trace['count'] or int(matches[0][1])<trace['count'] or int(matches[0][2])<12:raise ValueError('missing qualification witnesses')
            if not text.splitlines()[-1].startswith('FSPASS '):raise ValueError('out-of-order completion')
            witnessed=0;ranges=0;mutations=0
            for op,event,row in zip(trace['ops'],predicted,rows):
                action,i,arg,offset,length,status,seed=op;old=event['old_size']
                if not status or status==-34 or action==14:
                    witnessed|=1<<action
                    mutations+=int(action==3 and length>0 or action==6 and old>0 or action in (11,12,13) or action==15 and old!=length)
                if action==3 and length:
                    if offset<old:ranges|=1
                    if offset==old and old:ranges|=2
                    if offset<old<offset+length:ranges|=4
                    if offset//512!=(offset+length-1)//512:ranges|=8
                    if offset//4096!=(offset+length-1)//4096:ranges|=16
                    if offset+length==65536:ranges|=32
                    if offset>old:ranges|=64
                    if offset and offset+length<old:ranges|=128
                if action==4 and length and not event['count']:ranges|=256
                if action==3 and not length:ranges|=512
                if action==9 and status==-16:ranges|=1024
                if action==2 and status==-9:ranges|=2048
                if action==15 and not status and length:ranges|=4096
            if (mutations,witnessed,ranges)!=tuple(map(int,matches[0][2:5])):raise ValueError('invented operation witnesses')
            required=sum(1<<i for i in (0,1,2,3,4,5,6,7,8,9,14,16))
            if trace['caps']&1:required|=sum(1<<i for i in (10,11,12,13))
            wrange=4095 if trace['caps']&2 else 4095&~64
            if trace['caps']&8:required|=1<<15;wrange|=4096
            if int(matches[0][3])&required!=required or int(matches[0][4])&wrange!=wrange:raise ValueError('insufficient coverage')
        elif rows or text.count('FSVERIFIED ')!=1 or not re.fullmatch(r'FSVERIFIED ops='+str(trace['count'])+r' checks=[1-9]\d*',text.splitlines()[-1]):raise ValueError('missing persisted verification')
    else:
        if 'FSPASS ' in text or not re.search(r'^FSFAILED first=\d+ checks=\d+ prefix_ops=\d+$',text,re.M):raise ValueError('missing failure prefix')
        if len(rows)!=6 or len(re.findall('^FSFAIL ',text,re.M))!=1 or not re.search(r'^FSFAILED first=6 checks=[1-9]\d* prefix_ops=6$',text,re.M):raise ValueError('wrong failing prefix')
        if re.sub(r'^FS(?:BEGIN |OP |FAIL |FAILED )[^\n]*\n','',text,flags=re.M):raise ValueError('unknown failure output')
        if failure=='model':
            byte=payload(trace['ops'][5][-1],513)[256]
            if not re.search(r'^FSFAIL op=6 kind=content file=0 offset=256 expected='+str(byte^1)+' observed='+str(byte)+'$',text,re.M):raise ValueError('wrong model failure')
        if failure=='status' and not re.search(r'^FSFAIL op=6 kind=result phase=operation expected=-9 observed=0 count=513 observed_count=513$',text,re.M):raise ValueError('wrong status failure')
    return rows
