#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Acceptance-runner evidence rejection, not a substitute for guest execution."""
import io
import json
import os
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

shoot_v1_report = guest.SHOOT_V1_REPORT
shoot_v1_transcript = ('Shoot scripting test for Mangrove\n'
                       'report created\n' + shoot_v1_report)
shoot_v1_completion = f'child-exit-0-shoot-v1-{run}\n'
shoot_v1_evidence = guest.validate_shoot_v1_evidence(
    shoot_v1_transcript, shoot_v1_report, shoot_v1_completion, run)
assert shoot_v1_evidence['external_child_commands'] == 15
assert shoot_v1_evidence['child_status'] == 0
for invalid in (
        (shoot_v1_transcript.replace('round 3\n', ''), shoot_v1_report,
         shoot_v1_completion),
        (shoot_v1_transcript, shoot_v1_report.replace('round 2\n', ''),
         shoot_v1_completion),
        (shoot_v1_transcript, shoot_v1_report,
         shoot_v1_completion.replace(run, 'fedcba9876543210'))):
    try:
        guest.validate_shoot_v1_evidence(*invalid, run)
    except ValueError:
        pass
    else:
        raise AssertionError('incomplete Shoot script evidence qualified')
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

network_native = ('NET1 source=10.0.2.2 reply_bytes=36 echoes=16 '
                  'rx_before=0 rx_after=17 tx_before=1 tx_after=17')
network_time = time_transcript.replace(
    'TIME1 complete\n', 'NET1 begin\n' + network_native +
    '\nTIME1 complete\n')
guest.validate_time_transcript(network_time, run, epoch - 1, later + 1, 180,
                               network_receive_smoke=True)
for invalid, enabled in ((time_transcript, True),
                         (network_time.replace('NET1 begin\n', ''), True),
                         (network_time.replace('NET1 begin\n',
                                                'NET1 begin\nNET1 begin\n'),
                          True),
                         (network_time, False)):
    try: guest.validate_time_transcript(invalid, run, epoch - 1, later + 1,
                                        180, enabled)
    except ValueError: pass
    else: raise AssertionError('inconsistent network/time evidence qualified')
network_interfaces = ('NAME TYPE STATE IPV4 RX TX\n'
                      'eth0 ethernet up 10.0.2.15 17 17\n')
network_transcript = ('NET1 begin\n' + network_native +
                      '\nNET1 complete\n')
standalone_evidence = guest.validate_network_transcript(
    network_transcript, network_interfaces)
assert standalone_evidence['gateway_echo_replies'] == 16
for incomplete in ('', network_native + '\nNET1 complete\n',
                   'NET1 begin\n' + network_native + '\n',
                   network_transcript + 'PASS\n',
                   network_transcript.replace('NET1 begin\n',
                                              'NET1 begin\nNET1 begin\n')):
    try: guest.validate_network_transcript(incomplete, network_interfaces)
    except ValueError: pass
    else: raise AssertionError('incomplete native network transcript qualified')
network_evidence = guest.validate_network_receive(network_native,
                                                   network_interfaces)
assert network_evidence['gateway_echo_replies'] == 16
assert network_evidence['kernel_rx_packets'] == 17
for bad_native, bad_interfaces in (
        (network_time.replace('rx_after=17', 'rx_after=0'),
         network_interfaces),
        (network_time.replace('echoes=16', 'echoes=15'),
         network_interfaces),
        (network_native.replace('source=10.0.2.2', 'source=10.0.2.3'),
         network_interfaces),
        (network_native.replace('reply_bytes=36', 'reply_bytes=0'),
         network_interfaces),
        (network_native.replace('tx_after=17', 'tx_after=1'),
         network_interfaces),
        (network_native, network_interfaces.replace(' 17 17', ' 0 17')),
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
    (folder/'disk').write_bytes(b'disk fixture')
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
    assert guest.parse_smp_boot_log(
        '[SMP] discovered=1 online=1\n', 1) == {
            'discovered_cpus': 1, 'online_cpus': 1}
    for bad_smp_log in ('', '[SMP] final discovered=2 online=1\n',
                        '[SMP] final discovered=1 online=1\n',
                        '[SMP] discovered=1 online=1\n',
                        '[SMP] discovered=2 online=1\n'):
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

    # Host-populated images must be committed before QEMU starts. The first
    # guest flush must not also pay for the host's initial image construction.
    secondary = folder/'secondary'; secondary.write_bytes(b'secondary fixture')
    replacement = folder/'replacement'; replacement.write_bytes(b'replacement fixture')
    sync_args = SimpleNamespace(**vars(args), secondary_disk=secondary,
                               hotplug_replacement=replacement)
    synced = []
    real_fsync = os.fsync
    def record_sync(descriptor):
        synced.append(os.pread(descriptor, 32, 0))
        real_fsync(descriptor)
    class SyncChecked(Finished):
        def __init__(self,*a,**kw):
            assert synced == [b'disk fixture', b'fixture',
                              b'secondary fixture', b'replacement fixture']
            evidence = json.loads((folder/'sync-before-launch'/'image-sync.json').read_text())
            assert set(evidence) == {'boot_disk', 'firmware_variables',
                                     'secondary_disk', 'hotplug_replacement'}
            assert evidence['boot_disk']['bytes'] == len(b'disk fixture')
            assert all(v['seconds'] >= 0 for v in evidence.values())
            super().__init__(*a,**kw)
    with patch.object(guest.os,'fsync',side_effect=record_sync), \
         patch.object(guest.subprocess,'Popen',SyncChecked):
        try: guest.boot(folder,folder/'disk','sync-before-launch',sync_args)
        except RuntimeError as error: assert 'no native launch evidence' in str(error)
        else: raise AssertionError('synthetic process qualified a guest')
    with patch.object(guest.os,'fsync',side_effect=OSError('injected image sync failure')), \
         patch.object(guest.subprocess,'Popen') as launch:
        try: guest.boot(folder,folder/'disk','sync-failed',args)
        except OSError as error: assert str(error) == 'injected image sync failure'
        else: raise AssertionError('failed host preparation qualified')
        launch.assert_not_called()
    assert (folder/'disk').read_bytes() == b'disk fixture'
    assert secondary.read_bytes() == b'secondary fixture'
    assert replacement.read_bytes() == b'replacement fixture'
    secondary.chmod(0o444)
    assert guest.sync_image_for_boot(secondary)['bytes'] == len(b'secondary fixture')
    assert secondary.stat().st_mode & 0o777 == 0o444
    fifo = folder/'not-an-image'; os.mkfifo(fifo)
    for non_image in (folder, fifo):
        try: guest.sync_image_for_boot(non_image)
        except ValueError: pass
        else: raise AssertionError('non-regular guest backing accepted')
    panic_reason = 'scheduler: restored stack outside kernel stack'
    evidence = folder / 'panic-parser'; evidence.mkdir()
    assert guest.kernel_failure(evidence) is None
    (evidence / 'debugcon.log').write_text('ordinary output\nPITH_PANIC ')
    assert guest.kernel_failure(evidence) is None  # incomplete producer write
    (evidence / 'debugcon.log').write_text('PITH_PANIC scheduler: restored stack')
    assert guest.kernel_failure(evidence) is None  # reason not complete yet
    (evidence / 'debugcon.log').write_text('PITH_PANIC ' + panic_reason + '\n')
    assert guest.kernel_failure(evidence) == 'guest kernel panic: ' + panic_reason
    (evidence / 'debugcon.log').unlink()
    (evidence / 'serial.log').write_text('=============== KERNEL PANIC ===============\n')
    assert guest.kernel_failure(evidence) == 'guest kernel panic; see serial.log'
    with (evidence / 'serial.log').open('wb') as oversized:
        oversized.truncate(8 * 1024 * 1024 + 1)
    try: guest.kernel_failure(evidence)
    except ValueError: pass
    else: raise AssertionError('unbounded diagnostic accepted')

    class Panicked(Process):
        last = None
        exited = False
        def __init__(self, command, **kw):
            super().__init__(command, **kw)
            Panicked.last = self
            path = Path(command[command.index('-debugcon') + 1].removeprefix('file:'))
            path.write_text('PITH_PANIC ' + panic_reason + '\n')
            if self.exited: self.returncode = 1
    with patch.object(guest.subprocess,'Popen',Process), \
         patch.object(guest,'QMP',QMP), patch.object(guest,'screen_text',return_value=
             '[FAIL] Ring 3 process creation failed at root working-directory lookup (status=-11)'):
        try: guest.boot(folder, folder/'disk', 'startup-failed', args)
        except RuntimeError as error:
            assert 'root working-directory lookup (status=-11)' in str(error)
        else: raise AssertionError('failed PID1 startup qualified or waited for timeout')
    for phase, exited, capture_error in (('panic-live', False, False),
                                        ('panic-capture-failure', False, True),
                                        ('panic-exited', True, False)):
        Panicked.exited = exited
        capture = OSError('framebuffer unavailable') if capture_error else 'guard fields\n'
        with patch.object(guest.subprocess,'Popen',Panicked), \
             patch.object(guest,'QMP',QMP), \
             patch.object(guest,'screen_text',side_effect=capture if capture_error else None,
                          return_value=capture):
            try: guest.boot(folder, folder/'disk', phase, args)
            except RuntimeError as error:
                assert str(error) == 'guest kernel panic: ' + panic_reason
            else: raise AssertionError('kernel panic qualified')
        assert Panicked.last.poll() is not None  # child stopped before cleanup
        assert (folder/phase/'kernel-panic.txt').read_text().endswith(panic_reason+'\n')
        if not exited and not capture_error:
            assert (folder/phase/'screen.txt').read_text() == 'guard fields\n'
        if capture_error:
            assert (folder/phase/'panic-capture-error.txt').read_text() == 'framebuffer unavailable'
power_history = ('/bin/shoot /temp/shutdown-history.shoot\n'
                '/bin/shoot /temp/reboot-history.shoot\n'
                '/bin/shoot /temp/reboot-verify.shoot\n').encode()
records = {
    '/home/developer/.state/shoot/history': 'history',
    '/temp/reboot-history.snapshot': 'snapshot',
    '/temp/history-marker-shutdown-history': 'shutdown-marker',
    '/temp/history-marker-reboot-history': 'reboot-marker',
}
payloads = {
    'history': power_history,
    'snapshot': b''.join(power_history.splitlines(keepends=True)[:2]),
    'shutdown-marker': f'shutdown-history-marker-shutdown-history-{run}\n'.encode(),
    'reboot-marker': f'shutdown-history-marker-reboot-history-{run}\n'.encode(),
}
with tempfile.TemporaryDirectory() as tmp:
    folder = Path(tmp); disk = folder/'disk.img'; disk.touch()
    def fake_find(image, path):
        assert path in records, path
        return 1, records[path]
    with patch.object(guest, 'ROOT_OFFSET', 0), \
         patch.object(guest, 'PART_BYTES', 0), \
         patch.object(guest, 'check_image'), \
         patch.object(guest, 'find', side_effect=fake_find), \
         patch.object(guest, 'payload', side_effect=lambda image,item: payloads[item]), \
         patch.object(guest, 'digest', return_value='digest'):
        assert guest.inspect_shutdown_history(folder,disk,folder,run,
                                              'shutdown-history')['shutdown_command_persisted']
        assert guest.inspect_shutdown_history(folder,disk,folder,run,
                                              'reboot-history')['shutdown_command_persisted']
        assert guest.inspect_shutdown_history(folder,disk,folder,run,
                                              'reboot-verify')['post_transition_history_readback']
        payloads['history'] = power_history[:-1]
        try: guest.inspect_shutdown_history(folder,disk,folder,run,'reboot-verify')
        except ValueError: pass
        else: raise AssertionError('unterminated shutdown history qualified')
print('Guest evidence: host image sync before launch, failed sync, non-regular media, missing launch, timeout, panic, shutdown history, and reboot readback validated')
