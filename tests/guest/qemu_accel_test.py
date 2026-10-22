#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regressions for native QEMU accelerator policy."""

import errno
import sys
import unittest
from types import SimpleNamespace

sys.dont_write_bytecode = True
import qemu_accel


class QemuAcceleratorTests(unittest.TestCase):
    @staticmethod
    def qemu_with_kvm(*_args, **_kwargs):
        return SimpleNamespace(returncode=0, stdout="kvm\ntcg\n", stderr="")

    def test_linux_defaults_to_kvm(self):
        self.assertEqual(qemu_accel.default_accel("linux"), "kvm")
        self.assertEqual(qemu_accel.default_accel("linux2"), "kvm")

    def test_other_hosts_keep_tcg_default(self):
        for host in ("darwin", "freebsd14", "win32"):
            with self.subTest(host=host):
                self.assertEqual(qemu_accel.default_accel(host), "tcg")

    def test_tcg_does_not_require_kvm_device(self):
        def unexpected_open(*_):
            self.fail("TCG preflight must not access /dev/kvm")

        self.assertIsNone(qemu_accel.accel_error(
            "tcg", "linux", open_device=unexpected_open))

    def test_explicit_kvm_requires_linux(self):
        error = qemu_accel.accel_error("kvm", "darwin")
        self.assertIn("only on Linux", error)
        self.assertIn("--accel tcg", error)

    def test_linux_kvm_preflight_opens_and_closes_device(self):
        opened = []
        closed = []

        def fake_open(path, flags):
            opened.append((path, flags))
            return 41

        def fake_close(descriptor):
            closed.append(descriptor)

        self.assertIsNone(qemu_accel.accel_error(
            "kvm", "linux", open_device=fake_open, close_device=fake_close,
            qemu_runner=lambda *_args, **_kwargs: SimpleNamespace(
                returncode=0, stdout="kvm\ntcg\n", stderr="")))
        self.assertEqual(opened[0][0], "/dev/kvm")
        self.assertEqual(closed, [41])

    def test_missing_kvm_is_actionable_and_never_falls_back(self):
        def missing(*_):
            raise FileNotFoundError(errno.ENOENT, "No such file or directory")

        error = qemu_accel.accel_error(
            "kvm", "linux", open_device=missing,
            qemu_runner=self.qemu_with_kvm)
        self.assertIn("No TCG fallback was attempted", error)
        self.assertIn("approved elevated execution path", error)
        self.assertIn("--accel tcg", error)

    def test_denied_kvm_access_is_actionable(self):
        def denied(*_):
            raise PermissionError(errno.EACCES, "Permission denied")

        error = qemu_accel.accel_error(
            "kvm", "linux", open_device=denied,
            qemu_runner=self.qemu_with_kvm)
        self.assertIn("cannot be opened read/write", error)
        self.assertIn("device permissions", error)

    def test_qemu_without_kvm_support_fails_without_fallback(self):
        error = qemu_accel.accel_error(
            "kvm", "linux",
            qemu_runner=lambda *_args, **_kwargs: SimpleNamespace(
                returncode=0, stdout="tcg\n", stderr=""))
        self.assertIn("does not advertise KVM", error)
        self.assertIn("No TCG fallback was attempted", error)


if __name__ == "__main__":
    unittest.main()
