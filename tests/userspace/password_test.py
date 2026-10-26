#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the production password implementation with hashlib and HMAC."""
import ctypes
import hashlib
import hmac
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else 'cc'


class Authentication(ctypes.Structure):
    _fields_ = [('algorithm', ctypes.c_uint32), ('iterations', ctypes.c_uint32),
                ('salt', ctypes.c_ubyte * 16), ('hash', ctypes.c_ubyte * 32)]


wrapper = r'''
#include "crypto/sha256.c"
#include "password.c"
static bool entropy_ok = true;
bool entropy_available(void) {return entropy_ok;}
bool entropy_random_u64(u64 *value) {*value = 0x123456789abcdef0ULL; return entropy_ok;}
void test_entropy(bool available) {entropy_ok = available;}
void test_pbkdf2(const char *password, const u8 *salt, usize length, u32 rounds, u8 *out) {
    pbkdf2_sha256(password, salt, length, rounds, out);
}
void test_hmac(const u8 *key, usize length, const u8 *data, usize size, u8 *out) {
    crypto_hmac_sha256_key_t prepared;
    crypto_hmac_sha256_key_init(&prepared, key, length);
    crypto_hmac_sha256_key_run(&prepared, data, size, out);
    crypto_hmac_sha256_key_clear(&prepared);
}
void test_sha256(const u8 *data, usize size, u8 *out) {
    crypto_sha256_context_t context;
    crypto_sha256_init(&context);
    crypto_sha256_update(&context, data, size);
    crypto_sha256_final(&context, out);
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-password-') as directory:
    path = Path(directory)
    # Use the real password ABI with host integer types, avoiding kernel/libc
    # headers that collide with the host's size_t and string declarations.
    (path / 'types.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize;
''')
    (path / 'password.h').write_text((ROOT / 'kernel/include/password.h').read_text())
    (path / 'entropy.h').write_text((ROOT / 'kernel/include/entropy.h').read_text())
    (path / 'password.c').write_text((ROOT / 'kernel/src/password.c').read_text())
    (path / 'crypto').mkdir()
    (path / 'crypto/sha256.c').write_text(
        (ROOT / 'kernel/src/crypto/sha256.c').read_text())
    (path / 'crypto/sha256.h').write_text(
        (ROOT / 'kernel/include/crypto/sha256.h').read_text())
    (path / 'wrapper.c').write_text(wrapper)
    library = path / 'password.so'
    subprocess.run([CC, '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fPIC', '-shared', '-I' + directory, str(path / 'wrapper.c'),
                    '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    lib.test_pbkdf2.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.c_uint32, ctypes.c_void_p]
    lib.test_hmac.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                             ctypes.c_size_t, ctypes.c_void_p]
    lib.test_sha256.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                ctypes.c_void_p]
    lib.password_auth_verify.argtypes = [ctypes.POINTER(Authentication), ctypes.c_char_p]
    lib.password_auth_verify.restype = ctypes.c_bool
    lib.password_auth_generate.argtypes = [ctypes.POINTER(Authentication), ctypes.c_char_p]
    lib.password_auth_generate.restype = ctypes.c_bool
    lib.password_secure_clear.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.test_entropy.argtypes = [ctypes.c_bool]
    out = (ctypes.c_ubyte * 32)()
    lib.test_sha256(b'abc', 3, out)
    assert bytes(out) == hashlib.sha256(b'abc').digest()
    for length in [0, 1, 63, 64, 65, 128]:
        key = b'k' * length
        for size in [0, 1, 32, 55, 56, 64, 129]:
            data = b'd' * size
            lib.test_hmac(key, length, data, size, out)
            assert bytes(out) == hmac.digest(key, data, 'sha256')
    salt = bytes(range(16))
    for password in [b'', b'password', b'x' * 64, b'x' * 65, b'x' * 128]:
        for rounds in [1, 2, 4096, 10000]:
            lib.test_pbkdf2(password, salt, len(salt), rounds, out)
            assert bytes(out) == hashlib.pbkdf2_hmac('sha256', password, salt, rounds)
    for rounds in [120000, 1000000]:
        auth = Authentication(1, rounds)
        auth.salt[:] = salt
        auth.hash[:] = hashlib.pbkdf2_hmac('sha256', b'password', salt, rounds)
        assert lib.password_auth_verify(ctypes.byref(auth), b'password')
        assert not lib.password_auth_verify(ctypes.byref(auth), b'wrong')
        for index in [0, 15, 31]:
            auth.hash[index] ^= 1
            assert not lib.password_auth_verify(ctypes.byref(auth), b'password')
            auth.hash[index] ^= 1
    auth = Authentication()
    assert lib.password_auth_generate(ctypes.byref(auth), b'password')
    assert auth.iterations == 120000
    assert bytes(auth.hash) == hashlib.pbkdf2_hmac('sha256', b'password', bytes(auth.salt), 120000)
    assert not lib.password_auth_generate(ctypes.byref(auth), b'')
    assert not lib.password_auth_generate(ctypes.byref(auth), b'x' * 129)
    assert not lib.password_auth_verify(ctypes.byref(auth), b'x' * 129)
    auth.iterations = 9999
    assert not lib.password_auth_verify(ctypes.byref(auth), b'password')
    lib.test_entropy(False)
    assert not lib.password_auth_generate(ctypes.byref(auth), b'password')
    secret = ctypes.create_string_buffer(b'secret material')
    lib.password_secure_clear(secret, len(secret))
    assert secret.raw == bytes(len(secret))
    started = time.perf_counter()
    lib.test_pbkdf2(b'benchmark', bytes(16), 16, 120000, out)
    elapsed = time.perf_counter() - started
    print(f'password reference tests passed; native 120000 rounds: {elapsed:.3f}s')
