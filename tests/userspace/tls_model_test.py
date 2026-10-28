#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the production errno TLS cell and native local-exec ELF contract."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"
OBJ = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build/tests/runtime-api-link-probe.o"
ELF = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / "build/tests/runtime-api-link-probe.elf"
PING = Path(sys.argv[4]) if len(sys.argv) > 4 else ROOT / "build/ping/ping.elf"

source = (ROOT / "libc/src/errno.c").read_text()
source = source.replace('#include <errno.h>\n', '', 1)
source = source.replace("__errno_location", "mangrove_errno_location")
assert "static _Thread_local int process_errno;" in source
with tempfile.TemporaryDirectory(prefix="mangrove-errno-tls-") as folder:
    folder = Path(folder)
    cfile = folder / "errno_tls_test.c"
    binary = folder / "errno_tls_test"
    test_source = (ROOT / "tests/userspace/errno_tls_test.c").read_text()
    test_source = test_source.replace("__errno_location",
                                     "mangrove_errno_location")
    cfile.write_text(source + "\n" + test_source)
    subprocess.run([CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-pthread", str(cfile),
        "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True,
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

assert OBJ.is_file() and ELF.is_file(), "native TLS link probe was not built"
assert PING.is_file(), "errno TLS executable was not built"
relocations = subprocess.run(["objdump", "-r", str(OBJ)], check=True,
    text=True, capture_output=True).stdout
assert "R_X86_64_TPOFF32" in relocations, relocations
program_headers = subprocess.run(["readelf", "-lW", str(ELF)], check=True,
    text=True, capture_output=True).stdout
assert " TLS " in program_headers, program_headers
errno_disassembly = subprocess.run(["objdump", "-d", str(PING)], check=True,
    text=True, capture_output=True).stdout
errno_start = errno_disassembly.find("<__errno_location>:")
assert errno_start >= 0 and "%fs:0x0" in errno_disassembly[errno_start:errno_start + 512], \
    "linked errno accessor does not use the expected TLS thread-pointer word"
symbols = subprocess.run(["nm", "-n", str(PING)], check=True,
    text=True, capture_output=True).stdout
assert "0000000000400000 T __mangrove_thread_return" in symbols, symbols
return_start = errno_disassembly.find("<__mangrove_thread_return>:")
assert return_start >= 0, "worker return trampoline is missing from linked CRT"
return_code = errno_disassembly[return_start:return_start + 256]
assert "mov    $0x69,%eax" in return_code and "syscall" in return_code and \
       "ud2" in return_code, return_code
print("Native local-exec TLS image, errno accessor, and worker-return CRT ABI PASS")
