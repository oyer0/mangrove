#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check Mangrove's original IEEE primitives against independent MPFR."""
import ctypes
import ctypes.util
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"
LIBM = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build/userspace/libm.a")
SANITIZED = "--sanitized" in sys.argv[3:]
RNDN = 0
ACCURACY_MAX = {}


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
        self.unary = {"cbrt": self.function("mpfr_cbrt", ctypes.c_int,
                                             [pointer, pointer, ctypes.c_int])}
        self.unary.update({name: self.function("mpfr_" + name, ctypes.c_int,
                                               [pointer, pointer, ctypes.c_int])
                           for name in ("exp", "exp2", "expm1", "log", "log2",
                                        "log10", "log1p")})
        self.unary.update({name: self.function("mpfr_" + name, ctypes.c_int,
                                               [pointer, pointer, ctypes.c_int])
                           for name in ("sinh", "cosh", "tanh", "asinh",
                                        "acosh", "atanh", "atan", "asin", "acos",
                                        "sin", "cos", "tan")})
        self.binary = {name: self.function("mpfr_" + name, ctypes.c_int,
                                           [pointer, pointer, pointer, ctypes.c_int])
                       for name in ("fmod", "remainder", "atan2", "pow")}
        self.remquo = self.function("mpfr_remquo", ctypes.c_int,
                                    [pointer, ctypes.POINTER(ctypes.c_long),
                                     pointer, pointer, ctypes.c_int])
        self.fma = self.function("mpfr_fma", ctypes.c_int,
                                 [pointer, pointer, pointer, pointer, ctypes.c_int])
        self.mul_2si = self.function("mpfr_mul_2si", ctypes.c_int,
                                     [pointer, pointer, ctypes.c_long, ctypes.c_int])
        self.hypot = self.function("mpfr_hypot", ctypes.c_int,
                                   [pointer, pointer, pointer, ctypes.c_int])
        self.sub = self.function("mpfr_sub", ctypes.c_int,
                                 [pointer, pointer, pointer, ctypes.c_int])
        self.absolute = self.function("mpfr_abs", ctypes.c_int,
                                      [pointer, pointer, ctypes.c_int])
        self.get_exp = self.function("mpfr_get_exp", ctypes.c_long, [pointer])
        self.get_double = self.function("mpfr_get_d", ctypes.c_double,
                                        [pointer, ctypes.c_int])
        self.is_infinite = self.function("mpfr_inf_p", ctypes.c_int, [pointer])
        self.is_zero = self.function("mpfr_zero_p", ctypes.c_int, [pointer])

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
    kind, operation = fields[:2]
    precision = {"F": 24, "D": 53, "L": 64}[kind]
    rounding = RNDN
    if operation in ("pow-round", "pow-negative-round",
                     "pow-negative-large-round"):
        if len(fields) != 6:
            return "malformed directed power record: " + " ".join(fields)
        rounding = {"0": 0, "3072": 1, "2048": 2, "1024": 3}.get(
            fields[4], -1)
        if rounding < 0:
            return "invalid rounding mode in power record"
        fields = [kind, "pow", fields[2], fields[3], fields[5]]
        operation = "pow"
    elif operation in ("cbrt-round", "exp2-round", "log1p-round", "tanh-round",
                     "cosh-round", "fma-round"):
        expected_length = 7 if operation == "fma-round" else 5
        if len(fields) != expected_length:
            return "malformed directed-rounding record: " + " ".join(fields)
        rounding_index = 5 if operation == "fma-round" else 3
        rounding = {"0": 0, "3072": 1, "2048": 2, "1024": 3}.get(
            fields[rounding_index], -1)
        if rounding < 0:
            return "invalid rounding mode in math record"
        if operation == "fma-round":
            fields = [kind, "fma", *fields[2:5], fields[6]]
            operation = "fma"
        else:
            fields = [kind, operation[:-6], fields[2], fields[4]]
            operation = fields[1]
    source = mpfr.value()
    second = mpfr.value()
    third = mpfr.value()
    target = mpfr.value(precision)
    actual = mpfr.value()
    try:
        if operation == "fma":
            if len(fields) != 6:
                return "malformed fused result record: " + " ".join(fields)
            input_value, second_value, third_value, output_value = fields[2:]
            mpfr.assign_hex(source, input_value)
            mpfr.assign_hex(second, second_value)
            mpfr.assign_hex(third, third_value)
            mpfr.fma(ctypes.byref(target), ctypes.byref(source),
                     ctypes.byref(second), ctypes.byref(third), rounding)
            quotient = None
        elif operation in ("fmod", "remainder", "remquo", "atan2", "pow"):
            if len(fields) != (6 if operation == "remquo" else 5):
                return "malformed binary result record: " + " ".join(fields)
            input_value, second_value = fields[2:4]
            output_value = fields[4]
            mpfr.assign_hex(source, input_value)
            mpfr.assign_hex(second, second_value)
            quotient = ctypes.c_long()
            if operation == "remquo":
                mpfr.remquo(ctypes.byref(target), ctypes.byref(quotient),
                            ctypes.byref(source), ctypes.byref(second), RNDN)
            else:
                mpfr.binary[operation](ctypes.byref(target), ctypes.byref(source),
                                       ctypes.byref(second), RNDN)
        elif operation in ("scalbn", "scalbln", "ldexp"):
            if len(fields) != 5:
                return "malformed scale result record: " + " ".join(fields)
            input_value, exponent_text, output_value = fields[2:]
            mpfr.assign_hex(source, input_value)
            mpfr.mul_2si(ctypes.byref(target), ctypes.byref(source),
                         int(exponent_text), RNDN)
            quotient = None
        elif operation in ("floor", "ceil", "trunc", "round", "sqrt"):
            if len(fields) != 4:
                return "malformed unary result record: " + " ".join(fields)
            input_value, output_value = fields[2:]
            mpfr.assign_hex(source, input_value)
            # The hexadecimal source is exact at 256 bits. Round the reference
            # operation directly to target precision to avoid double rounding.
            mpfr.operations[operation](ctypes.byref(target), ctypes.byref(source), RNDN)
            quotient = None
        elif operation in ("cbrt", "hypot", "exp", "exp2", "expm1",
                           "log", "log2", "log10", "log1p", "sinh", "cosh",
                           "tanh", "asinh", "acosh", "atanh", "atan", "asin",
                           "acos", "sin", "cos", "tan"):
            if operation == "cbrt" and len(fields) == 4:
                input_value, output_value = fields[2:]
                mpfr.assign_hex(source, input_value)
                mpfr.unary[operation](ctypes.byref(target), ctypes.byref(source), rounding)
            elif operation == "hypot" and len(fields) == 5:
                input_value, second_value, output_value = fields[2:]
                mpfr.assign_hex(source, input_value)
                mpfr.assign_hex(second, second_value)
                mpfr.hypot(ctypes.byref(target), ctypes.byref(source),
                           ctypes.byref(second), RNDN)
            elif operation in ("exp", "exp2", "expm1", "log", "log2",
                               "log10", "log1p", "sinh", "cosh", "tanh",
                               "asinh", "acosh", "atanh", "atan", "asin",
                               "acos", "sin", "cos", "tan") and len(fields) == 4:
                input_value, output_value = fields[2:]
                mpfr.assign_hex(source, input_value)
                mpfr.unary[operation](ctypes.byref(target), ctypes.byref(source), rounding)
            else:
                return "malformed root result record: " + " ".join(fields)
            quotient = None
        mpfr.assign_hex(actual, output_value)
        if mpfr.cmp(ctypes.byref(actual), ctypes.byref(target)) != 0:
            if operation not in ("cbrt", "hypot", "exp", "exp2", "expm1", "pow",
                                 "log", "log2", "log10", "log1p", "sinh",
                                 "cosh", "tanh", "asinh", "acosh", "atanh",
                                 "atan", "asin", "acos", "atan2", "sin", "cos", "tan"):
                arguments = (f"{input_value}, {second_value}, {third_value}"
                             if operation == "fma" else input_value)
                return f"{kind} {operation}({arguments}) returned {output_value}; " \
                       f"expected correctly rounded {operation} result"
            difference = mpfr.value(256)
            try:
                mpfr.sub(ctypes.byref(difference), ctypes.byref(actual),
                         ctypes.byref(target), RNDN)
                mpfr.absolute(ctypes.byref(difference), ctypes.byref(difference), RNDN)
                if mpfr.is_zero(ctypes.byref(target)) or \
                        mpfr.is_infinite(ctypes.byref(target)):
                    return f"{kind} {operation}({input_value}" + \
                           (f", {second_value}" if operation in ("hypot", "pow") else "") + \
                           f") returned {output_value}; " \
                           "the MPFR reference is zero or infinite"
                minimum_exponent = {"F": -125, "D": -1021, "L": -16381}[kind]
                ulp_exponent = max(minimum_exponent,
                                   mpfr.get_exp(ctypes.byref(target))) - precision
                mpfr.mul_2si(ctypes.byref(difference), ctypes.byref(difference),
                             -ulp_exponent, RNDN)
                ulp_error = mpfr.get_double(ctypes.byref(difference), RNDN)
                maximum = {"cbrt": 1.0, "hypot": 1.5, "exp": 2.0,
                           "exp2": 2.0, "expm1": 2.0, "log": 2.0,
                           "log2": 2.0, "log10": 2.0, "log1p": 2.0,
                           "sinh": 2.0, "cosh": 2.0, "tanh": 2.0,
                           "asinh": 2.0, "acosh": 2.0, "atanh": 2.0,
                           "atan": 2.0, "asin": 2.0, "acos": 2.0,
                           "atan2": 2.0, "pow": 2.0, "sin": 2.0, "cos": 2.0,
                           "tan": 2.0}[operation]
                if ulp_error > maximum:
                    return f"{kind} {operation}({input_value}" + \
                           (f", {second_value}" if operation in ("hypot", "atan2", "pow") else "") + \
                           f") returned {output_value}; " \
                           f"error is {ulp_error:.6g} ulp (limit {maximum})"
                key = (kind, operation)
                ACCURACY_MAX[key] = max(ACCURACY_MAX.get(key, 0.0), ulp_error)
            finally:
                mpfr.clear(ctypes.byref(difference))
        if operation == "remquo":
            observed_quotient = int(fields[5])
            expected_low = abs(quotient.value) & 7
            observed_low = abs(observed_quotient) & 7
            expected_sign = -1 if ((input_value.startswith("-") !=
                                    second_value.startswith("-"))) else 1
            if observed_low != expected_low or (expected_low != 0 and
                    ((observed_quotient < 0) != (expected_sign < 0))):
                return (f"{kind} remquo({input_value}, {second_value}) returned quotient "
                        f"{observed_quotient}; MPFR low bits/sign require "
                        f"{expected_sign * expected_low}")
        return None
    finally:
        for value in (source, second, third, target, actual):
            mpfr.clear(ctypes.byref(value))


def main():
    ACCURACY_MAX.clear()
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
        fma_executable = directory / "libm-fma-values"
        flags = [CC, "-no-pie", "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Wformat=2",
                 "-Werror", "-fno-builtin", "-frounding-math",
                 "-ftrapping-math", "-I" + str(ROOT / "libc/include"),
                 str(ROOT / "tests/libm_values_probe.c"), str(LIBM),
                 "-o", str(executable)]
        if SANITIZED:
            flags[1:1] = ["-fsanitize=address,undefined",
                          "-fno-sanitize-recover=all"]
        run(flags)
        result = run([str(executable)])

        generic_command = [CC, "-no-pie", "-std=c17", "-pedantic", "-Wall", "-Wextra",
                           "-Werror", "-fno-builtin", "-I" + str(ROOT / "libc/include"),
                           str(ROOT / "tests/tgmath_probe.c"), str(LIBM),
                           "-o", str(generic)]
        if SANITIZED:
            generic_command[1:1] = ["-fsanitize=address,undefined",
                                     "-fno-sanitize-recover=all"]
        run(generic_command)
        run([str(generic)])

        fma_command = [CC, "-no-pie", "-std=gnu17", "-O1", "-Wall", "-Wextra",
                       "-Wformat=2", "-Werror", "-fno-builtin", "-frounding-math",
                       "-ftrapping-math", "-I" + str(ROOT / "libc/include"),
                       str(ROOT / "tests/libm_fma_probe.c"), str(LIBM),
                       "-o", str(fma_executable)]
        if SANITIZED:
            fma_command[1:1] = ["-fsanitize=address,undefined",
                                "-fno-sanitize-recover=all"]
        run(fma_command)
        fma_result = run([str(fma_executable)])

        unsupported = directory / "unsupported.c"
        for name in ("erf", "erfc", "tgamma", "lgamma"):
            unsupported.write_text("#include <math.h>\n"
                                   f"int main(void) {{ return {name}(1.0, 2.0) == 0.0; }}\n")
            run([CC, "-fno-builtin", "-I" + str(ROOT / "libc/include"),
                 str(unsupported), str(LIBM),
                 "-o", str(directory / ("unsupported-" + name))], expected=1)

    lines = result.stdout.splitlines()
    if not lines or lines[0] != "CHECK 0" or not lines[-1].startswith("COMPLETE records="):
        raise ValueError("math edge controls or deterministic trace completion failed")
    count = int(lines[-1].split("records=")[1].split()[0])
    rows = lines[1:-1]
    if len(rows) != count:
        raise ValueError(f"expected {count} numeric records, received {len(rows)}")
    fma_lines = fma_result.stdout.splitlines()
    if not fma_lines or fma_lines[0] != "CHECK 0" or \
            not fma_lines[-1].startswith("COMPLETE records="):
        raise ValueError("FMA edge controls or deterministic trace completion failed")
    fma_count = int(fma_lines[-1].split("records=")[1].split()[0])
    fma_rows = fma_lines[1:-1]
    if len(fma_rows) != fma_count:
        raise ValueError(f"expected {fma_count} FMA records, received {len(fma_rows)}")
    for kind in ("F", "D", "L"):
        observed = sum(line.startswith(kind + " fma ") for line in fma_rows)
        if observed < 500:
            raise ValueError(f"incomplete {kind} FMA coverage: {observed} records")
        if sum(line.startswith(kind + " fma-round ") for line in fma_rows) != 16:
            raise ValueError(f"incomplete {kind} directed FMA coverage")
    rows += fma_rows
    coverage = Counter((line.split()[0], line.split()[1]) for line in rows)
    for kind in ("F", "D", "L"):
        for operation in ("cbrt", "hypot", "exp", "exp2", "expm1", "pow", "log",
                          "log2", "log10", "log1p", "sinh", "cosh", "tanh",
                          "asinh", "acosh", "atanh", "atan", "asin", "acos",
                          "atan2", "sin", "cos", "tan"):
            ACCURACY_MAX.setdefault((kind, operation), 0.0)
        for operation in ("cbrt", "hypot", "pow"):
            if coverage[(kind, operation)] < 512:
                raise ValueError(f"incomplete {kind} {operation} coverage: "
                                 f"{coverage[(kind, operation)]} records")
        if coverage[(kind, "cbrt-round")] != 4:
            raise ValueError(f"incomplete {kind} cbrt rounding-mode coverage")
        if coverage[(kind, "exp2-round")] != 4 or \
                coverage[(kind, "log1p-round")] != 8 or \
                coverage[(kind, "tanh-round")] != 4 or \
                coverage[(kind, "cosh-round")] != 4:
            raise ValueError(f"incomplete {kind} transcendental rounding coverage")
        if coverage[(kind, "pow-round")] != 4:
            raise ValueError(f"incomplete {kind} power rounding coverage")
        if coverage[(kind, "pow-negative-round")] != 4:
            raise ValueError(f"incomplete {kind} negative power rounding coverage")
        expected_large_power_rounds = 0 if kind == "F" else 4
        if coverage[(kind, "pow-negative-large-round")] != expected_large_power_rounds:
            raise ValueError(f"incomplete {kind} large negative power rounding coverage")
        for operation in ("exp", "exp2", "expm1", "log", "log2", "log10", "log1p",
                           "sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
                           "atan", "asin", "acos", "atan2", "sin", "cos", "tan"):
            if coverage[(kind, operation)] < 512:
                raise ValueError(f"incomplete {kind} {operation} coverage: "
                                 f"{coverage[(kind, operation)]} records")
    for line in rows:
        error = check_row(oracle, line.split())
        if error:
            raise AssertionError(error)

    fma_sample = next(line.split() for line in fma_rows if line.startswith("D fma "))
    fma_sample[-1] = "0x1p+20"
    if check_row(oracle, fma_sample) is None:
        raise AssertionError("corrupted FMA result negative control was accepted")
    if check_row(oracle, ["D", "fma", "0x1p+0", "0x1p+0", "0x1p+0"]) is None:
        raise AssertionError("incomplete FMA record was accepted")

    sample = next(line.split() for line in rows if line.startswith("D sqrt "))
    sample[-1] = "0x1p+20"
    if check_row(oracle, sample) is None:
        raise AssertionError("corrupted numerical-result negative control was accepted")

    cbrt_sample = next(line.split() for line in rows if line.startswith("D cbrt "))
    cbrt_sample[-1] = "0x1p+20"
    if check_row(oracle, cbrt_sample) is None:
        raise AssertionError("corrupted cbrt result was accepted")
    hypot_sample = next(line.split() for line in rows if line.startswith("L hypot "))
    hypot_sample[-1] = "0x0p+0"
    if check_row(oracle, hypot_sample) is None:
        raise AssertionError("corrupted hypot result was accepted")
    hyperbolic_sample = next(line.split() for line in rows
                             if line.startswith("L asinh "))
    hyperbolic_sample[-1] = "0x0p+0"
    if check_row(oracle, hyperbolic_sample) is None:
        raise AssertionError("corrupted hyperbolic result was accepted")
    inverse_sample = next(line.split() for line in rows
                          if line.startswith("D atan "))
    inverse_sample[-1] = "0x0p+0"
    if check_row(oracle, inverse_sample) is None:
        raise AssertionError("corrupted inverse-trigonometric result was accepted")
    trig_sample = next(line.split() for line in rows
                       if line.startswith("L sin "))
    trig_sample[-1] = "0x0p+0"
    if check_row(oracle, trig_sample) is None:
        raise AssertionError("corrupted trigonometric result was accepted")

    quotient_sample = next(line.split() for line in rows
                           if line.startswith("D remquo ") and
                           int(line.split()[-1]) != 0)
    quotient_sample[-1] = str(int(quotient_sample[-1]) ^ 1)
    if check_row(oracle, quotient_sample) is None:
        raise AssertionError("corrupted remquo quotient negative control was accepted")
    if check_row(oracle, ["D", "fmod", "0x1p+0", "0x1p+1"]) is None:
        raise AssertionError("incomplete operation record was accepted")
    if check_row(oracle, ["D", "atan2", "0x1p+0"]) is None:
        raise AssertionError("incomplete atan2 result record was accepted")

    print("Mangrove original math MPFR comparison{}: PASS".format(
        " with ASan/UBSan" if SANITIZED else ""))
    print(f"  records={count} deterministic-seed=0x4d414e47524f5645")
    print("  trunc/floor/ceil/round/sqrt, exponent scaling and exact remainders: PASS")
    print("  float/double/x87-long-double scaling, fmod, remainder and remquo: PASS")
    print("  float/double/x87-long-double cbrt (1 ULP), hypot (1.5 ULP), pow (2 ULP), and signed pow in four rounding modes: PASS")
    print("  fenv, classification, subnormal, signed-zero, nextafter and ABI edge controls: PASS")
    print("  exp/log families (2 ULP), subnormal and log1p-boundary vectors: PASS")
    print("  hyperbolic, inverse-trigonometric and trigonometric families against MPFR: PASS")
    print(f"  exact single-round FMA: {fma_count} float/double/x87-long-double records, PASS")
    print("  maximum sampled ULP error: " + ", ".join(
        f"{kind}/{operation}={error:.3g}"
        for (kind, operation), error in sorted(ACCURACY_MAX.items())))
    print("  unsupported-symbol, corrupted-result and coverage controls: PASS")
    print("  corrupted-result/quotient and incomplete-record controls: rejected")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print("libm qualification failed: " + str(error), file=sys.stderr)
        raise
