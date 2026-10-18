#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Acceptance-runner evidence rejection, not a substitute for guest execution."""
import io
from datetime import datetime, timezone
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
sys.dont_write_bytecode = True
import guest_mgfs_test as guest

run='0123456789abcdef'
for phase,count in [('create',57),('verify',46)]:
    rows=guest.expected_operations(phase,run,221)
    assert len(rows)==count
    text=f'GUESTFS1 phase={phase} run={run} bytes=8193\n'
    text+=''.join(f'OP {i} {name} expected={value} observed={value}\n'
                  for i,(name,value) in enumerate(rows,1))
    text+=f'GUESTFS1 complete phase={phase} run={run} ops={count} identifier=221\n'
    guest.validate_transcript(text,phase,run,221)
    for wrong in ['',text.split('GUESTFS1 complete')[0],text.replace('observed=8193','observed=8192',1),
                  text.replace('seek-start','not-a-seek',1),text.replace('expected=8193 observed=8193','expected=8192 observed=8192',1),
                  text.replace('identifier=221','identifier=222'),text+'PASS\n']:
        try: guest.validate_transcript(wrong,phase,run,221)
        except ValueError: pass
        else: raise AssertionError('invalid transcript qualified')

epoch = int(datetime(2025, 3, 1, 0, 0, 0, tzinfo=timezone.utc).timestamp())
later = epoch + 2
time_transcript = (
    f'TIME1 unix={epoch} nanoseconds=0 date=2025-03-01 00:00:00 +0000 UTC '
    'busy_cpu=23 sleep_cpu=24 sleep_elapsed_ms=30 wall_delta_s=1 '
    f'wall_elapsed_ms=1201 wall_start_unix={epoch + 1} later_unix={later} '
    'later_date=2025-03-01 00:00:02\nTIME1 complete\n')
guest.validate_time_transcript(time_transcript, run, epoch - 1, later + 1, 180)
# Scheduler delay may make a nominal 30 ms sleep much longer without making
# monotonic time incorrect. Do not impose an arbitrary upper bound here.
guest.validate_time_transcript(
    time_transcript.replace('sleep_elapsed_ms=30', 'sleep_elapsed_ms=500'),
    run, epoch - 1, later + 1, 180)
for wrong in ('', time_transcript.replace('nanoseconds=0', 'nanoseconds=1'),
              time_transcript.replace('+0000 UTC', '+0100 CET'),
              time_transcript.replace('wall_delta_s=1', 'wall_delta_s=0'),
              time_transcript.replace('busy_cpu=23', 'busy_cpu=0'),
              time_transcript.replace('TIME1 complete\n', ''),
              time_transcript.replace(f'unix={epoch}', f'unix={epoch + 1000}')):
    try: guest.validate_time_transcript(wrong, run, epoch - 1, later + 1, 180)
    except ValueError: pass
    else: raise AssertionError('invalid native time transcript qualified')

network_native = ('NET1 source=10.0.2.2 reply_bytes=36 rx_before=0 '
                  'rx_after=2 tx_before=1 tx_after=3')
network_time = time_transcript.replace(
    'TIME1 complete\n', network_native + '\nTIME1 complete\n')
guest.validate_time_transcript(network_time, run, epoch - 1, later + 1, 180,
                               network_receive_smoke=True)
for invalid, enabled in ((time_transcript, True),
                         (network_time, False)):
    try: guest.validate_time_transcript(invalid, run, epoch - 1, later + 1,
                                        180, enabled)
    except ValueError: pass
    else: raise AssertionError('inconsistent network/time evidence qualified')
network_interfaces = ('NAME TYPE STATE IPV4 RX TX\n'
                      'eth0 ethernet up 10.0.2.15 7 9\n')
network_evidence = guest.validate_network_receive(network_native,
                                                   network_interfaces)
assert network_evidence['gateway_echo_replies'] == 1
assert network_evidence['kernel_rx_packets'] == 7
for bad_native, bad_interfaces in (
        (network_time.replace('rx_after=2', 'rx_after=0'),
         network_interfaces),
        (network_native.replace('source=10.0.2.2', 'source=10.0.2.3'),
         network_interfaces),
        (network_native.replace('reply_bytes=36', 'reply_bytes=0'),
         network_interfaces),
        (network_native.replace('tx_after=3', 'tx_after=1'),
         network_interfaces),
        (network_native, network_interfaces.replace(' 7 9', ' 0 9')),
        (network_native, 'NAME TYPE STATE IPV4 RX TX\n')):
    try: guest.validate_network_receive(bad_native, bad_interfaces)
    except ValueError: pass
    else: raise AssertionError('invalid network receive evidence qualified')

# Process/QMP dependencies below are controlled only to test infrastructure
# failure handling. No filesystem or successful guest behavior is simulated.
class Process:
    def __init__(self,*unused,**kw):
        self.stdin=io.BytesIO(); self.stdout=io.BytesIO(); self.returncode=None
    def poll(self): return self.returncode
    def wait(self,**kw): self.returncode=0; return 0
    def kill(self): self.returncode=-9
class QMP:
    def __init__(self,process,reader=None,writer=None): self.process=process; self.events=[]
    def call(self,name,args=None):
        if name=='query-status': return {'status':'running'}
        if name=='quit': self.process.returncode=0
    def drain_events(self): pass
    def close(self): pass

with tempfile.TemporaryDirectory(prefix='mangrove-guest-evidence-') as temp:
    folder=Path(temp); firmware=folder/'vars'; firmware.write_bytes(b'fixture')
    args=SimpleNamespace(qemu=str(folder/'missing-qemu'),accel='tcg',ovmf_vars=firmware,
                         ovmf_code=firmware,timeout=30)
    try: guest.boot(folder,folder/'disk','missing',args)
    except FileNotFoundError: pass
    else: raise AssertionError('missing launcher qualified')
    ticks=iter([0,0,31])
    with patch.object(guest.subprocess,'Popen',Process), patch.object(guest,'QMP',QMP), \
         patch.object(guest,'screen_text',return_value=''),patch.object(guest.time,'sleep'), \
         patch.object(guest.time,'monotonic',side_effect=lambda:next(ticks)):
        try: guest.boot(folder,folder/'disk','timeout',args)
        except TimeoutError: pass
        else: raise AssertionError('timeout qualified')
    assert guest.qemu_cpu_count(SimpleNamespace()) == 1
    assert guest.qemu_cpu_count(SimpleNamespace(cpus=2)) == 2
    assert guest.qemu_network_arguments(SimpleNamespace()) == [
        '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']
    assert guest.qemu_network_arguments(SimpleNamespace(no_nic=True)) == []
    assert guest.parse_smp_boot_log(
        '[SMP] final discovered=2 online=2\n', 2) == {
            'discovered_cpus': 2, 'online_cpus': 2}
    for bad_smp_log in ('', '[SMP] final discovered=2 online=1\n',
                        '[SMP] final discovered=1 online=1\n'):
        try: guest.parse_smp_boot_log(bad_smp_log, 2)
        except ValueError: pass
        else: raise AssertionError('incomplete SMP startup qualified')
    try: guest.qemu_cpu_count(SimpleNamespace(cpus=3))
    except ValueError: pass
    else: raise AssertionError('unsupported vCPU count accepted')
    with patch.object(guest.subprocess,'Popen',Process),patch.object(guest,'QMP',QMP), \
         patch.object(guest,'screen_text',side_effect=['developer ~:',
             *[('developer ~: '+ '/bin/shoot /temp/unexpected-child-exit.shoot'[:i]).rstrip() for i in range(len('/bin/shoot /temp/unexpected-child-exit.shoot')+1)],
             'Could not run "fstest": exited with status 2.']), \
         patch.object(guest.time,'sleep'):
        try: guest.boot(folder,folder/'disk','unexpected-child-exit',args)
        except RuntimeError as error: assert 'native launch, exit or wait failure' in str(error)
        else: raise AssertionError('unexpected child exit qualified')
    class Finished(Process):
        def __init__(self,*a,**kw): super().__init__(*a,**kw); self.returncode=0
    with patch.object(guest.subprocess,'Popen',Finished):
        try: guest.boot(folder,folder/'disk','no-execution',args)
        except RuntimeError as error: assert 'no native launch evidence' in str(error)
        else: raise AssertionError('empty execution qualified')
print('Guest evidence: missing launch, timeout, empty execution, missing completion and wrong operations/counts rejected')
