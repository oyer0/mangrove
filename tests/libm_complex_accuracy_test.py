#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare original Mangrove complex math with independent MPC/MPFR."""
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
MAX_ERROR = {}


class MpfrValue(ctypes.Structure):
    _fields_ = [("precision", ctypes.c_long), ("sign", ctypes.c_int),
                ("exponent", ctypes.c_long),
                ("limbs", ctypes.POINTER(ctypes.c_ulong))]


class MpcValue(ctypes.Structure):
    _fields_ = [("real", MpfrValue), ("imag", MpfrValue)]


class Oracle:
    def __init__(self):
        mpfr_name = ctypes.util.find_library("mpfr")
        mpc_name = ctypes.util.find_library("mpc")
        if not mpfr_name or not mpc_name:
            raise RuntimeError("host MPFR and MPC runtimes are required")
        self.mpfr = ctypes.CDLL(mpfr_name)
        self.mpc = ctypes.CDLL(mpc_name)
        self.init = self.fn(self.mpfr, "mpfr_init2", None,
                            [ctypes.POINTER(MpfrValue), ctypes.c_long])
        self.clear = self.fn(self.mpfr, "mpfr_clear", None,
                             [ctypes.POINTER(MpfrValue)])
        self.set_str = self.fn(self.mpfr, "mpfr_set_str", ctypes.c_int,
                               [ctypes.POINTER(MpfrValue), ctypes.c_char_p,
                                ctypes.c_int, ctypes.c_int])
        self.set = self.fn(self.mpfr, "mpfr_set", ctypes.c_int,
                           [ctypes.POINTER(MpfrValue), ctypes.POINTER(MpfrValue),
                            ctypes.c_int])
        self.cmp = self.fn(self.mpfr, "mpfr_cmp", ctypes.c_int,
                           [ctypes.POINTER(MpfrValue), ctypes.POINTER(MpfrValue)])
        self.sub = self.fn(self.mpfr, "mpfr_sub", ctypes.c_int,
                           [ctypes.POINTER(MpfrValue), ctypes.POINTER(MpfrValue),
                            ctypes.POINTER(MpfrValue), ctypes.c_int])
        self.abs = self.fn(self.mpfr, "mpfr_abs", ctypes.c_int,
                           [ctypes.POINTER(MpfrValue), ctypes.POINTER(MpfrValue),
                            ctypes.c_int])
        self.scale = self.fn(self.mpfr, "mpfr_mul_2si", ctypes.c_int,
                             [ctypes.POINTER(MpfrValue), ctypes.POINTER(MpfrValue),
                              ctypes.c_long, ctypes.c_int])
        self.set_ui_2exp = self.fn(self.mpfr, "mpfr_set_ui_2exp", ctypes.c_int,
                                   [ctypes.POINTER(MpfrValue), ctypes.c_ulong,
                                    ctypes.c_long, ctypes.c_int])
        self.get_exp = self.fn(self.mpfr, "mpfr_get_exp", ctypes.c_long,
                               [ctypes.POINTER(MpfrValue)])
        self.get_double = self.fn(self.mpfr, "mpfr_get_d", ctypes.c_double,
                                  [ctypes.POINTER(MpfrValue), ctypes.c_int])
        self.is_zero = self.fn(self.mpfr, "mpfr_zero_p", ctypes.c_int,
                               [ctypes.POINTER(MpfrValue)])
        self.is_inf = self.fn(self.mpfr, "mpfr_inf_p", ctypes.c_int,
                              [ctypes.POINTER(MpfrValue)])
        self.mpc_init = self.fn(self.mpc, "mpc_init3", None,
                                [ctypes.POINTER(MpcValue), ctypes.c_long,
                                 ctypes.c_long])
        self.mpc_clear = self.fn(self.mpc, "mpc_clear", None,
                                 [ctypes.POINTER(MpcValue)])
        pointer = ctypes.POINTER(MpcValue)
        self.mpc_ops = {}
        for name in ("exp", "log", "sqrt", "pow", "sin", "cos", "tan",
                     "sinh", "cosh", "tanh", "asin", "acos", "atan",
                     "asinh", "acosh", "atanh", "conj", "proj"):
            arity = 3 if name == "pow" else 2
            args = [pointer, pointer] + ([pointer] if arity == 3 else []) + [ctypes.c_int]
            self.mpc_ops[name] = self.fn(self.mpc, "mpc_" + name, ctypes.c_int, args)
        self.mpc_abs = self.fn(self.mpc, "mpc_abs", ctypes.c_int,
                               [ctypes.POINTER(MpfrValue), pointer, ctypes.c_int])
        self.mpc_arg = self.fn(self.mpc, "mpc_arg", ctypes.c_int,
                               [ctypes.POINTER(MpfrValue), pointer, ctypes.c_int])

    @staticmethod
    def fn(library, name, result, args):
        function = getattr(library, name)
        function.restype = result
        function.argtypes = args
        return function

    def scalar(self, precision=256):
        value = MpfrValue()
        self.init(ctypes.byref(value), precision)
        return value

    def complex(self, precision=256):
        value = MpcValue()
        self.mpc_init(ctypes.byref(value), precision, precision)
        return value

    def assign(self, value, text):
        if self.set_str(ctypes.byref(value), text.encode("ascii"), 0, RNDN):
            raise ValueError("MPFR rejected hexadecimal value " + text)

    def check_component(self, kind, operation, actual_text, reference,
                        target, difference, other_reference=None,
                        comparison=None, other_magnitude=None,
                        count_metric=True):
        self.set(ctypes.byref(target), ctypes.byref(reference), RNDN)
        self.assign(difference, actual_text)
        actual = difference
        if self.cmp(ctypes.byref(actual), ctypes.byref(target)) == 0:
            return None
        # MPC keeps an unbounded exponent range. Its finite reference can
        # therefore lie beyond the tested C format's maximum exponent, while
        # the correctly rounded native result is infinity. Compare that
        # target-format overflow explicitly instead of treating the reference
        # as an ordinary finite ULP target.
        precision = {"F": 24, "D": 53, "L": 64}[kind]
        maximum_exponent = {"F": 128, "D": 1024, "L": 16384}[kind]
        overflow = self.is_inf(ctypes.byref(reference))
        if (not overflow and
                self.get_exp(ctypes.byref(reference)) >= maximum_exponent):
            # Round-to-nearest overflows at the midpoint between max-finite
            # and the next unbounded-exponent value, 2**max_exponent. Build
            # that boundary independently at ample MPFR precision.
            boundary = self.scalar(128)
            half_ulp = self.scalar(128)
            magnitude = self.scalar(128)
            try:
                self.set_ui_2exp(ctypes.byref(boundary), 1,
                                 maximum_exponent, RNDN)
                self.set_ui_2exp(ctypes.byref(half_ulp), 1,
                                 maximum_exponent - precision - 1, RNDN)
                self.sub(ctypes.byref(boundary), ctypes.byref(boundary),
                         ctypes.byref(half_ulp), RNDN)
                self.abs(ctypes.byref(magnitude), ctypes.byref(reference), RNDN)
                overflow = self.cmp(ctypes.byref(magnitude),
                                    ctypes.byref(boundary)) >= 0
            finally:
                self.clear(ctypes.byref(boundary))
                self.clear(ctypes.byref(half_ulp))
                self.clear(ctypes.byref(magnitude))
        if overflow:
            if self.is_inf(ctypes.byref(actual)) and actual.sign == reference.sign:
                return None
            return (f"{kind} {operation} failed target overflow rounding: "
                    f"got {actual_text}, expected signed infinity")
        if self.is_inf(ctypes.byref(reference)):
            if self.is_inf(ctypes.byref(actual)) and actual.sign == reference.sign:
                return None
            return (f"{kind} {operation} returned {actual_text}; "
                    "reference is signed infinity")
        self.sub(ctypes.byref(difference), ctypes.byref(actual),
                 ctypes.byref(target), RNDN)
        self.abs(ctypes.byref(difference), ctypes.byref(difference), RNDN)
        minimum_normal = {"F": -125, "D": -1021, "L": -16381}[kind]
        if other_reference is not None:
            self.abs(ctypes.byref(comparison), ctypes.byref(reference), RNDN)
            self.abs(ctypes.byref(other_magnitude), ctypes.byref(other_reference), RNDN)
            self.scale(ctypes.byref(comparison), ctypes.byref(comparison), 4, RNDN)
            near_cancellation = self.cmp(ctypes.byref(comparison),
                                         ctypes.byref(other_magnitude)) < 0
        else:
            near_cancellation = False
        if near_cancellation:
            exponent = self.get_exp(ctypes.byref(other_magnitude))
            ulp_exponent = max(minimum_normal, exponent) - precision
        elif self.is_zero(ctypes.byref(target)):
            ulp_exponent = {"F": -149, "D": -1074, "L": -16445}[kind]
        else:
            ulp_exponent = max(minimum_normal,
                               self.get_exp(ctypes.byref(target))) - precision
        self.scale(ctypes.byref(difference), ctypes.byref(difference),
                   -ulp_exponent, RNDN)
        error = self.get_double(ctypes.byref(difference), RNDN)
        # x87 complex functions compose separately rounded real operations.
        # Inverse and power formulas are branch/cancellation sensitive, so
        # their 32-ULP bound is separate from the 8-ULP direct-family bound.
        family = operation[1:] if operation.startswith("c") else operation
        limit = 32.0 if family in ("asin", "acos", "atan", "asinh", "acosh",
                                   "atanh", "pow") else 8.0
        if count_metric:
            MAX_ERROR[(kind, operation)] = max(
                MAX_ERROR.get((kind, operation), 0.0), error)
        if error > limit:
            return (f"{kind} {operation} returned {actual_text}; component error "
                    f"{error:.6g} ULP (reference={self.get_double(ctypes.byref(reference), RNDN):.17g}, "
                    f"rounded={self.get_double(ctypes.byref(target), RNDN):.17g}) "
                    f"(limit {limit})")
        return None


def run(command, expected=0):
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                            timeout=120,
                            env={**os.environ, "LC_ALL": "C",
                                 "ASAN_OPTIONS": "detect_leaks=0"})
    if result.returncode != expected:
        raise RuntimeError(f"command status {result.returncode}, expected {expected}: "
                           f"{' '.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result


def check_guest_record(oracle, fields):
    """Independently check one native guest complex hexadecimal record."""
    if len(fields) < 2 or fields[0] not in ("F", "D", "L"):
        return "malformed guest complex record"
    kind, operation = fields[:2]
    left, right, result = oracle.complex(), oracle.complex(), oracle.complex()
    real, imag, difference, scratch, comparison, other, _ = [
        oracle.scalar() for _ in range(7)]
    all_complex = (left, right, result)
    all_scalars = (real, imag, difference, scratch, comparison, other)
    try:
        if operation == "cpow":
            if len(fields) != 8:
                return "malformed native cpow record"
            inputs = fields[2:6]
            output_re, output_im = fields[6:8]
            oracle.assign(left.real, inputs[0]); oracle.assign(left.imag, inputs[1])
            oracle.assign(right.real, inputs[2]); oracle.assign(right.imag, inputs[3])
            oracle.mpc_ops["pow"](ctypes.byref(result), ctypes.byref(left),
                                   ctypes.byref(right), 0)
        elif operation in ("cabs", "carg"):
            if len(fields) != 5:
                return "malformed native complex scalar record"
            oracle.assign(left.real, fields[2]); oracle.assign(left.imag, fields[3])
            function = oracle.mpc_abs if operation == "cabs" else oracle.mpc_arg
            function(ctypes.byref(real), ctypes.byref(left), 0)
            error = oracle.check_component(kind, operation, fields[4], real,
                                           imag, difference, count_metric=False)
            return error
        else:
            if len(fields) != 6:
                return "malformed native complex record"
            output_re, output_im = fields[4:6]
            oracle.assign(left.real, fields[2]); oracle.assign(left.imag, fields[3])
            oracle.mpc_ops[operation[1:]](ctypes.byref(result),
                                           ctypes.byref(left), 0)
        error = oracle.check_component(kind, operation, output_re,
                                       result.real, real, difference,
                                       result.imag, comparison, other,
                                       count_metric=False)
        if error:
            return error
        return oracle.check_component(kind, operation, output_im,
                                      result.imag, imag, scratch,
                                      result.real, comparison, other,
                                      count_metric=False)
    finally:
        for value in all_complex:
            oracle.mpc_clear(ctypes.byref(value))
        for value in all_scalars:
            oracle.clear(ctypes.byref(value))


def main():
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("INCOMPLETE complex x87 checks require an x86-64 host", file=sys.stderr)
        return 2
    if not LIBM.is_file():
        raise RuntimeError("missing Mangrove libm archive: " + str(LIBM))
    oracle = Oracle()
    with tempfile.TemporaryDirectory(prefix="mangrove-complex-") as temporary:
        executable = Path(temporary) / "complex-probe"
        command = [CC, "-no-pie", "-std=gnu17", "-O1", "-Wall", "-Wextra",
                   "-Wformat=2", "-Werror", "-fno-builtin", "-frounding-math",
                   "-I" + str(ROOT / "libc/include"),
                   str(ROOT / "tests/libm_complex_probe.c"), str(LIBM),
                   "-o", str(executable)]
        if SANITIZED:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        run(command)
        output = run([str(executable)]).stdout.splitlines()
        if not output or output[0] != "CHECK 0" or \
                not output[-1].startswith("COMPLETE records="):
            raise ValueError("complex edge checks or deterministic completion failed: " +
                             repr(output[:1]) + " ... " + repr(output[-1:]))
        expected_count = int(output[-1].split("records=")[1].split()[0])
        records = output[1:-1]
        if len(records) != expected_count:
            raise ValueError(f"expected {expected_count} complex records, got {len(records)}")

    precision = {"F": 24, "D": 53, "L": 64}
    values = [oracle.complex() for _ in range(3)]
    scalar_values = [oracle.scalar() for _ in range(7)]
    a, b, reference = values
    target_re, target_im, diff, scratch, comparison, other_magnitude, _ = scalar_values
    coverage = Counter()
    try:
        for line in records:
            fields = line.split()
            if len(fields) < 4 or fields[0] not in precision:
                raise ValueError("malformed complex record: " + line)
            kind, operation = fields[:2]
            coverage[(kind, operation)] += 1
            if operation in ("cabs", "carg"):
                if len(fields) != 5:
                    raise ValueError("malformed complex scalar record: " + line)
                oracle.assign(a.real, fields[2]); oracle.assign(a.imag, fields[3])
                (oracle.mpc_abs if operation == "cabs" else oracle.mpc_arg)(
                    ctypes.byref(target_re), ctypes.byref(a), 0)
                error = oracle.check_component(kind, operation, fields[4],
                                               target_re, target_im, diff)
                if error:
                    raise AssertionError(error)
                continue
            if operation == "cpow":
                if len(fields) != 8:
                    raise ValueError("malformed complex power record: " + line)
                inputs = fields[2:6]
                output_re, output_im = fields[6:8]
                oracle.assign(a.real, inputs[0]); oracle.assign(a.imag, inputs[1])
                oracle.assign(b.real, inputs[2]); oracle.assign(b.imag, inputs[3])
                oracle.mpc_ops["pow"](ctypes.byref(reference), ctypes.byref(a),
                                       ctypes.byref(b), 0)
            else:
                if len(fields) != 6:
                    raise ValueError("malformed complex result record: " + line)
                inputs = fields[2:4]
                output_re, output_im = fields[4:6]
                oracle.assign(a.real, inputs[0]); oracle.assign(a.imag, inputs[1])
                oracle.mpc_ops[operation[1:]](ctypes.byref(reference),
                                               ctypes.byref(a), 0)
            error = oracle.check_component(kind, operation, output_re,
                                           reference.real, target_re, diff,
                                           reference.imag, comparison,
                                           other_magnitude)
            if error:
                raise AssertionError(error + " in " + line)
            error = oracle.check_component(kind, operation, output_im,
                                           reference.imag, target_im, scratch,
                                           reference.real, comparison,
                                           other_magnitude)
            if error:
                raise AssertionError(error + " in " + line)
        sample = next(line.split() for line in records if line.startswith("D cexp "))
        oracle.assign(a.real, sample[2]); oracle.assign(a.imag, sample[3])
        oracle.mpc_ops["exp"](ctypes.byref(reference), ctypes.byref(a), 0)
        if oracle.check_component("D", "cexp", "0x1p+20", reference.real,
                                  target_re, diff, count_metric=False) is None:
            raise AssertionError("corrupted complex component negative control was accepted")
    finally:
        for value in values:
            oracle.mpc_clear(ctypes.byref(value))
        for value in scalar_values:
            oracle.clear(ctypes.byref(value))

    for kind in precision:
        for operation in ("cexp", "clog", "csqrt", "csin", "ccos", "ctan",
                          "csinh", "ccosh", "ctanh", "casin", "cacos", "catan",
                          "casinh", "cacosh", "catanh", "cabs", "carg", "cpow"):
            if coverage[(kind, operation)] < 512:
                raise ValueError(f"incomplete {kind} {operation} coverage: "
                                 f"{coverage[(kind, operation)]}")

    print("Mangrove complex math MPC/MPFR comparison{}: PASS".format(
        " with ASan/UBSan" if SANITIZED else ""))
    print(f"  deterministic-seed=0x636f6d706c65782d records={expected_count}")
    print("  binary32, binary64 and x87 extended complex ABI and components: PASS")
    print("  exp/log/power/roots/trig/hyperbolic/inverses vs MPC: PASS")
    print("  magnitude/phase vs MPC; axes, branch cuts and signed-zero checks: PASS")
    print("  sampled maximum component ULP error: " + ", ".join(
        f"{kind}/{name}={value:.3g}" for (kind, name), value in sorted(MAX_ERROR.items())))
    print("  corrupted-result and malformed-completion controls: rejected")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print("complex qualification failed: " + str(error), file=sys.stderr)
        raise
