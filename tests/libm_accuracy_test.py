#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check Mangrove's original IEEE primitive group against MPFR."""
import ctypes
import ctypes.util
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"
LIBM = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build/userspace/libm.a")
SANITIZED = "--sanitized" in sys.argv[3:]
RNDN = 0


class MpfrValue(ctypes.Structure):
    _fields_ = [("precision", ctypes.c_long), ("sign", ctypes.c_int),
                ("exponent", ctypes.c_long),
                ("limbs", ctypes.POINTER(ctypes.c_ulong))]


class Mpfr:
    def __init__(self):
        name = ctypes.util.find_library("mpfr")
        if not name:
            raise RuntimeError("MPFR runtime is required for independent libm qualification")
        self.lib = ctypes.CDLL(name)
        pointer = ctypes.POINTER(MpfrValue)
        self.init = self.function("mpfr_init2", None, [pointer, ctypes.c_long])
        self.clear = self.function("mpfr_clear", None, [pointer])
        self.set_str = self.function("mpfr_set_str", ctypes.c_int,
                                     [pointer, ctypes.c_char_p, ctypes.c_int, ctypes.c_int])
        self.cmp = self.function("mpfr_cmp", ctypes.c_int, [pointer, pointer])
        self.operations = {name: self.function("mpfr_" + name, ctypes.c_int,
                                               [pointer, pointer, ctypes.c_int])
                           for name in ("floor", "ceil", "trunc", "round", "sqrt")}

    def function(self, name, result, args):
        function = getattr(self.lib, name)
        function.restype = result
        function.argtypes = args
        return function

    def value(self, precision=256):
        value = MpfrValue()
        self.init(ctypes.byref(value), precision)
        return value

    def assign_hex(self, target, text):
        status = self.set_str(ctypes.byref(target), text.encode("ascii"), 0, RNDN)
        if status != 0:
            raise ValueError("MPFR rejected hexadecimal value " + text)


def run(command, expected=0):
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                            timeout=60,
                            env={**os.environ, "LC_ALL": "C",
                                 "ASAN_OPTIONS": "detect_leaks=0"})
    if result.returncode != expected:
        raise RuntimeError(f"command status {result.returncode}, expected {expected}: "
                           f"{' '.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result


def check_row(mpfr, fields):
    kind, operation, input_value, output_value = fields
    precision = {"F": 24, "D": 53, "L": 64}[kind]
    source = mpfr.value()
    target = mpfr.value(precision)
    actual = mpfr.value()
    try:
        mpfr.assign_hex(source, input_value)
        # The hexadecimal source is exact at 256 bits. Round the reference
        # operation directly to the target precision to avoid double rounding.
        mpfr.operations[operation](ctypes.byref(target), ctypes.byref(source), RNDN)
        mpfr.assign_hex(actual, output_value)
        if mpfr.cmp(ctypes.byref(actual), ctypes.byref(target)) != 0:
            return f"{kind} {operation}({input_value}) returned {output_value}; " \
                   f"expected correctly rounded {operation} result"
        return None
    finally:
        for value in (source, target, actual):
            mpfr.clear(ctypes.byref(value))


def main():
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("INCOMPLETE original x87 math tests require an x86-64 host", file=sys.stderr)
        return 2
    if not LIBM.is_file():
        raise RuntimeError("missing production Mangrove libm archive: " + str(LIBM))
    oracle = Mpfr()
    with tempfile.TemporaryDirectory(prefix="mangrove-libm-") as temporary:
        directory = Path(temporary)
        executable = directory / "libm-values"
        generic = directory / "tgmath-probe"
        flags = [CC, "-std=gnu17", "-O1", "-fno-builtin", "-frounding-math",
                 "-ftrapping-math", "-I" + str(ROOT / "libc/include"),
                 str(ROOT / "tests/libm_values_probe.c"), str(LIBM),
                 "-o", str(executable)]
        if SANITIZED:
            flags[1:1] = ["-fsanitize=address,undefined",
                          "-fno-sanitize-recover=all"]
        run(flags)
        result = run([str(executable)])

        generic_command = [CC, "-std=c17", "-pedantic", "-Wall", "-Wextra",
                           "-Werror", "-fno-builtin", "-I" + str(ROOT / "libc/include"),
                           str(ROOT / "tests/tgmath_probe.c"), str(LIBM),
                           "-o", str(generic)]
        if SANITIZED:
            generic_command[1:1] = ["-fsanitize=address,undefined",
                                     "-fno-sanitize-recover=all"]
        run(generic_command)
        run([str(generic)])

        unsupported = directory / "unsupported.c"
        unsupported.write_text("#include <math.h>\n"
                               "int main(void) { return exp(1.0) == 0.0; }\n")
        run([CC, "-fno-builtin", "-I" + str(ROOT / "libc/include"),
             str(unsupported), str(LIBM), "-o", str(directory / "unsupported")],
            expected=1)

    lines = result.stdout.splitlines()
    if not lines or lines[0] != "CHECK 0" or not lines[-1].startswith("COMPLETE records="):
        raise ValueError("math edge controls or deterministic trace completion failed")
    count = int(lines[-1].split("records=")[1].split()[0])
    rows = lines[1:-1]
    if len(rows) != count:
        raise ValueError(f"expected {count} numeric records, received {len(rows)}")
    for line in rows:
        error = check_row(oracle, line.split())
        if error:
            raise AssertionError(error)

    sample = rows[len(rows) // 2].split()
    sample[-1] = "0x1p+20"
    if check_row(oracle, sample) is None:
        raise AssertionError("corrupted numerical-result negative control was accepted")

    print("Mangrove original math primitive MPFR comparison{}: PASS".format(
        " with ASan/UBSan" if SANITIZED else ""))
    print(f"  records={count} deterministic-seed=0x4d414e47524f5645")
    print("  correctly rounded float/double/x87-long-double trunc, floor, ceil, round, sqrt: PASS")
    print("  fenv, classification, subnormal, signed-zero, nextafter and ABI edge controls: PASS")
    print("  unsupported-transcendental link and corrupted-result controls: rejected")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print("libm qualification failed: " + str(error), file=sys.stderr)
        raise
