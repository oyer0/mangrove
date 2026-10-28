#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Structural regression for the Ring 3 to syscall-stack trust boundary."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
entry = (ROOT / "kernel/src/syscall_entry.s").read_text()
cpu = (ROOT / "kernel/include/cpu.h").read_text()
syscall = (ROOT / "kernel/src/syscall.c").read_text()
idt = (ROOT / "kernel/src/idt_asm.s").read_text()
idt_c = (ROOT / "kernel/src/idt.c").read_text()
probe = (ROOT / "tests/userspace/syscall_entry_probe.s").read_text()

body = entry.split("syscall_entry:", 1)[1].split("1:", 1)[0]
body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
instructions = [line.strip() for line in body.splitlines()
                if line.strip() and not line.strip().startswith(".")]
expected = [
    "swapgs",
    "movq %rsp, %gs:16",
    "movq %gs:8, %rsp",
    "testq %rsp, %rsp",
    "jz 1f",
]
assert instructions[:len(expected)] == expected, instructions[:len(expected)]
transition = instructions.index("movq %gs:8, %rsp")
assert not any(re.match(r"(?:push|call|pop|movq?\s+[^,]+,\s*\(%rsp\))", ins)
               for ins in instructions[:transition]), instructions[:transition]
assert "pushq %gs:16" in instructions[transition + 1:], instructions
assert instructions.index("pushq %gs:16") > transition
assert "call scheduler_kernel_stack_top" not in entry
assert instructions.index("pushq %gs:16") < instructions.index("pushq %r11")
assert "andq $0x200ad5, 112(%r12)" in instructions
assert "orq $0x202, 112(%r12)" in instructions
assert instructions.index("popq %r11") < instructions.index("popq %rsp")

assert "syscall_kernel_stack_top) == 8U" in cpu
assert "syscall_user_rsp) == 16U" in cpu
assert "sizeof(syscall_frame_t) == 128U" in syscall
assert "syscall_frame_on_thread_stack(thread, frame)" in syscall
assert "syscall_return_context_valid(frame)" in syscall
assert "wrmsr(MSR_FMASK" in syscall and "(1ULL << 18)" in syscall
assert "IA32_GS_BASE" in idt and "Lisr_check_gs_base" in idt
assert ".Lisr_restore_registers" in idt
isr = idt.split("common_isr_stub:", 1)[1].split("common_irq_stub:", 1)[0]
assert "subq $8, %rsp" in isr
assert "testq $3, 152(%rsp)" in isr
assert "movq 128(%rsp), %rax" in isr
assert "cmpq $2, %rax" in isr and "cmpq $18, %rax" in isr
assert "rdmsr" in isr and "movq $1, (%rsp)" in isr
assert "cmpq $0, (%rsp)" in isr and "swapgs" in isr
assert "idt_set_gate(IDT_VECTOR_NMI, isr_stub_table[IDT_VECTOR_NMI], 2" in idt_c
assert "idt_set_gate(IDT_VECTOR_MACHINE_CHECK,\n                 isr_stub_table[IDT_VECTOR_MACHINE_CHECK], 3" in idt_c
exhausted_probe = probe.split("threadtest_syscall_then_touch_rsp:", 1)[1]
assert exhausted_probe.index("syscall") < exhausted_probe.index("pushq %rax")

print("syscall entry trust-boundary structure: PASS")
