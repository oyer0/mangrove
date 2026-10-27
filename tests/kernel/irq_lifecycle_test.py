#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production IRQ dispatch and verify NIC teardown ordering."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def function(source, name):
    pattern = re.compile(
        r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + re.escape(name) + r"\s*\(",
        re.M,
    )
    for match in pattern.finditer(source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end]
    raise AssertionError(f"production function not found: {name}")


e1000 = (ROOT / "kernel/src/net/e1000.c").read_text()
rtl8168 = (ROOT / "kernel/src/net/rtl8168.c").read_text()
timer = (ROOT / "kernel/src/timer.c").read_text()
vmm = (ROOT / "kernel/src/vmm.c").read_text()
e1000_irq = function(e1000, "e1000_irq_handler")
e1000_detach = function(e1000, "e1000_detach")
e1000_probe = function(e1000, "e1000_probe_device")
rtl_prepare = function(rtl8168, "rtl8168_prepare_msix")
rtl_cleanup = function(rtl8168, "rtl8168_cleanup_failed_start")
rtl_detach = function(rtl8168, "rtl8168_detach")

assert "irq_gate_enter" in e1000_irq and "irq_gate_leave" in e1000_irq
assert e1000_detach.index("E1000_REG_IMC") < e1000_detach.index("e1000_irq_quiesce")
assert e1000_detach.index("e1000_irq_quiesce") < e1000_detach.index(
    "e1000_release_buffers")
assert "cpu_by_index(0)" in e1000_probe and "bsp->apic_id" in e1000_probe
assert "lapic_current_id()" not in e1000_probe

assert "cpu_by_index(0)" in rtl_prepare and "bsp->apic_id" in rtl_prepare
assert "lapic_current_id()" not in rtl_prepare
assert rtl_cleanup.index("RTL8168_REG_INTERRUPT_MASK") < rtl_cleanup.index(
    "irq_unregister_vector") < rtl_cleanup.index("rtl8168_release_rings")
assert rtl_detach.index("RTL8168_REG_INTERRUPT_MASK") < rtl_detach.index(
    "irq_unregister_vector") < rtl_detach.index("rtl8168_release_rings")
assert "irq_register_permanent_vector(IRQ_VECTOR_PIT" in timer
assert "irq_register_permanent_vector(IRQ_VECTOR_LAPIC_TIMER" in timer
assert "irq_register_permanent_vector(IRQ_VECTOR_TLB_SHOOTDOWN" in vmm

with tempfile.TemporaryDirectory(prefix="mangrove-irq-lifecycle-") as temp_dir:
    binary = Path(temp_dir) / "irq-lifecycle-test"
    subprocess.run(
        [CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-fsanitize=address,undefined", "-Ikernel/include", "-Iinclude",
         "tests/kernel/irq_lifecycle_test.c", "-o", str(binary)],
        cwd=ROOT, check=True,
    )
    subprocess.run([str(binary)], cwd=ROOT,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                   check=True, timeout=30)
