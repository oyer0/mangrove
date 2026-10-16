#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recovery runner evidence controls; not simulated successful guest boots."""
import copy
import sys
sys.dont_write_bytecode = True
import guest_lifetime_test as lifetime
import guest_mgfs_test as guest

RUN='0123456789abcdef'
def fixture(active, replacement=False):
    root_rows=guest.expected_operations('create',RUN,221)
    root=f'GUESTFS1 phase=create run={RUN} bytes=8193\n'
    root+=''.join(f'OP {i} {name} expected={n} observed={n}\n' for i,(name,n) in enumerate(root_rows,1))
    root+=f'GUESTFS1 complete phase=create run={RUN} ops={len(root_rows)} identifier=221\n'
    child=[row for row in lifetime.CHILD if not active or row[0]!='unmount-busy']
    if replacement:
        at=child.index(('ready',1));child[at:at]=[('bind',0),('length',8193)]
        child[child.index(('write',-33))]=('replace',-33)
    text=f'RECOVERY1 parent-start run={RUN}\n'
    text+=''.join(f'VOP {i} recovery-parent-{name} expected={n} observed={n}\n'
                  for i,(name,n) in enumerate(lifetime.PARENT[:2],1))
    text+=f'RECOVERY1 child-start run={RUN}\n'
    text+=''.join(f'VOP {i} recovery-child-{name} expected={n} observed={n}\n'
                  for i,(name,n) in enumerate(child,1))
    text+=f'ROWNER stage={"returned" if active else "online"} run={RUN} owner=4 instance=5 physical=6 state=2 flags=3\n'
    text+=f'ROWNER stage=offline run={RUN} owner=4 instance=5 physical=6 state=2 flags=8\n'
    text+=f'RECOVERY1 child-complete run={RUN}\n'
    text+=''.join(f'VOP {i} recovery-parent-{name} expected={n} observed={n}\n'
                  for i,(name,n) in enumerate(lifetime.PARENT[2:],3))
    text+=f'ROWNER stage=exited run={RUN} owner=4 instance=5 physical=6 state=2 flags=8\n'
    text+=root+f'RECOVERY1 parent-complete run={RUN}\n'
    events=[{'event':'DEVICE_DELETED','data':{'device':'secondary-storage'}}]
    actions=[dict(stage=i,marker=f'RECOVERY1 {name} run={RUN}',qmp_status={'status':'running'})
             for i,name in enumerate(('ready','uncertain','retained'))]
    return text,events,actions

controls=0
for active in (False,True):
    text,events,actions=fixture(active)
    lifetime.validate(text,RUN,events,actions,active)
    spawn='VOP 2 recovery-parent-spawn expected=1 observed=1\n'
    child=f'RECOVERY1 child-start run={RUN}\n'
    lifetime.validate(text.replace(spawn+child,child+spawn),RUN,events,actions,active)
    wrong_texts=['',text.replace('observed=-33','observed=16'),
        text.replace('stage=exited','stage=offline'),text.replace('state=2','state=1'),
        text.replace('physical=6','physical=5'),text.replace('flags=8','flags=3'),
        text.replace('owner=4','owner=0'),text.replace('run='+RUN,'run=fedcba9876543210',1),
        text+ 'VOP 99 unknown expected=0 observed=0\n',text+'ROWNER malformed\n',
        text+f'RECOVERY1 extra run={RUN}\n',text.replace('child-complete','child-incomplete'),
        text.replace('observed=8193','observed=8192',1),text.replace('parent-complete','parent-incomplete')]
    done=f'RECOVERY1 child-complete run={RUN}\n'
    wait='VOP 3 recovery-parent-wait expected=0 observed=0\n'
    wrong_texts.append(text.replace(done+wait,wait+done))
    bad_events=[[],events*2,[{'event':'DEVICE_DELETED','data':{'device':'system-storage'}}]]
    bad_actions=[[],actions[:2],list(reversed(actions))]
    stopped=copy.deepcopy(actions);stopped[1]['qmp_status']['status']='paused';bad_actions.append(stopped)
    for t,e,a in [(t,events,actions) for t in wrong_texts]+[(text,e,actions) for e in bad_events]+[(text,events,a) for a in bad_actions]:
        try:lifetime.validate(t,RUN,e,a,active)
        except (ValueError,IndexError):controls+=1
        else:raise AssertionError('invalid recovery evidence qualified')
print(f'Recovery evidence: {controls} missing, malformed, wrong-status/identity, removal and completion controls rejected')

read_text=f'READFAIL1 start run={RUN}\n'+''.join(
    f'VOP {i} readfault-{n} expected={v} observed={v}\n' for i,(n,v) in enumerate(lifetime.READ_ROWS,1))
read_text+=f'READFAIL1 complete run={RUN} ops={len(lifetime.READ_ROWS)}\n'
lifetime.validate_read(read_text,RUN)
for value in ['',read_text+'PASS\n',read_text.replace('observed=-12','observed=0'),
              read_text.replace('observed=0','observed=-12'),read_text.replace('no-owner','owner')]:
    try:lifetime.validate_read(value,RUN)
    except ValueError:pass
    else:raise AssertionError('invalid online-read evidence qualified')
print('Read-fault evidence: missing, wrong EOF/error, unknown operations and trailing success rejected')

lookup_text=f'LOOKUPFAIL1 start run={RUN}\n'+''.join(
    f'VOP {i} lookupfault-{n} expected={v} observed={v}\n' for i,(n,v) in enumerate(lifetime.LOOKUP_ROWS,1))
lookup_text+=f'LOOKUPFAIL1 complete run={RUN} ops={len(lifetime.LOOKUP_ROWS)}\n'
lifetime.validate_lookup(lookup_text,RUN)
for value in ('',lookup_text+'PASS\n',lookup_text.replace('observed=-12','observed=-2'),
              lookup_text.replace('observed=-12','observed=0'),lookup_text.replace('no-owner','owner'),
              lookup_text.replace('lookupfault-unmount expected=0 observed=0\n',''),
              lookup_text.replace('LOOKUPFAIL1 complete','LOOKUPFAIL1 incomplete')):
    try:lifetime.validate_lookup(value,RUN)
    except ValueError:pass
    else:raise AssertionError('invalid pathname metadata-fault evidence qualified')
print('Lookup-fault evidence: false absence/success, missing owner/unmount/completion and trailing markers rejected')

text,events,actions=fixture(False,True)
lifetime.validate(text,RUN,events,actions,replacement=True)
for wrong in (text.replace('recovery-child-replace','recovery-child-write'),
              text.replace('observed=-33','observed=0'),
              text.replace('recovery-child-bind','recovery-child-unbound'),
              text.replace('state=2','state=1'),text.replace('owner=4','owner=0')):
    try:lifetime.validate(wrong,RUN,events,actions,replacement=True)
    except ValueError:pass
    else:raise AssertionError('incorrect replacement recovery qualified')
print('Replacement recovery: real snapshot binding, UNCERTAIN, exact retained-owner identity and completion required')
