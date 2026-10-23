#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Timing and negative controls for the physical navigation capture analyzer."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('reed_input', ROOT / 'scripts/diagnose_reed_input.py')
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)


class NavigationCaptureTests(unittest.TestCase):
    def setUp(self):
        self.host = 10**12
        self.real = 2 * 10**12
        self.counter = 10**9
        self.samples = [dict(counter=self.counter + offset // 10,
            before_ns=self.host + offset - 10000, after_ns=self.host + offset + 10000,
            realtime_ns=self.real + offset - 10000) for offset in (0, 2 * 10**9)]
        self.physical = [dict(device='keyboard', code=35, value=value,
            host_ns=self.host + offset * 10**6) for value, offset in ((1, 100), (2, 500), (0, 1000))]
        self.events = [self.event('present', 0, 0, 40, 990),
            self.event('read', 11, 1, 0, 999), self.event('hid', 11, 0, 0, 1003),
            self.event('present', 0, 0, 39, 1004)]
        self.qemu = ('1@2000.100500:input_event_key_qcode con 0, key qcode h, down 1\n'
                     '1@2001.001000:input_event_key_qcode con 0, key qcode h, down 0\n')

    def event(self, kind, key, first, second, ms):
        # The PIT clock deliberately stands still: host comparisons must use HPET.
        return dict(kind=kind, key=key, first=first, second=second,
                    counter=self.counter + ms * 100000, uptime_ms=7)

    def result(self):
        return diag.analyze(dict(events=self.events, dropped=0, period_fs=10000000),
            self.samples, self.physical, self.qemu, self.host, self.host + 2 * 10**9)['holds'][0]

    def test_separate_clock_domains_and_inflight_render(self):
        row = self.result()
        self.assertAlmostEqual(row['physical_to_qemu_up_ms'], 1, places=3)
        self.assertAlmostEqual(row['qemu_to_hid_release_ms'], 2, places=3)
        self.assertAlmostEqual(row['hid_timing_uncertainty_ms'], .01, places=3)
        self.assertAlmostEqual(row['last_cursor_change_after_physical_release_ms'], 4, places=3)
        self.assertEqual(row['host_autorepeats'], 1)
        self.assertEqual(row['generated_after_hid_release'], 0)
        self.assertEqual(row['repeats_read_after_hid_release'], 0)

    def test_stale_guest_generation_and_initial_press_are_distinct(self):
        self.events += [self.event('repeat', 11, 0, 0, 1010),
                        self.event('read', 11, 0, 0, 1011),
                        self.event('read', 11, 1, 0, 1012)]
        row = self.result()
        self.assertEqual(row['generated_after_hid_release'], 1)
        self.assertEqual(row['initial_presses_read_after_hid_release'], 1)
        self.assertEqual(row['repeats_read_after_hid_release'], 1)

    def test_missing_hid_release_does_not_claim_cancellation(self):
        self.events = [e for e in self.events if e['kind'] != 'hid']
        row = self.result()
        self.assertIsNone(row['qemu_to_hid_release_ms'])
        self.assertIsNone(row['hid_timing_uncertainty_ms'])

    def test_next_deliberate_press_ends_release_window(self):
        self.physical.append(dict(device='keyboard', code=36, value=1,
                                 host_ns=self.host + 1002 * 10**6))
        self.assertEqual(self.result()['visible_cursor_changes_after_physical_release'], 0)

    def test_truncated_or_wrong_layout_trace_is_rejected(self):
        header = diag.HEADER.pack(0x3145434152544E49, 1, 0, 0xfed000f0,
                                 10000000, 2**64 - 1, 16384, 32)
        with self.assertRaises(ValueError):
            diag.decode_trace(header)
        with self.assertRaises(ValueError):
            diag.decode_trace(bytes(diag.HEADER.size))

    def test_capture_requires_four_matched_held_keys_without_loss(self):
        rows = [dict(self.result(), key=key) for key in 'hjkl']
        measurement = dict(holds=rows, lost_guest_events=0)
        self.assertTrue(diag.complete_physical_capture(measurement))
        rows[-1]['physical_to_qemu_up_ms'] = None
        self.assertFalse(diag.complete_physical_capture(measurement))
        rows[-1] = dict(self.result(), key='l', hold_ms=20)
        self.assertFalse(diag.complete_physical_capture(measurement))
        rows[-1] = dict(self.result(), key='l')
        measurement['lost_guest_events'] = 1
        self.assertFalse(diag.complete_physical_capture(measurement))


if __name__ == '__main__':
    unittest.main()
