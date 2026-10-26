#!/usr/bin/env python3
"""Local, deterministic TLS 1.3 client fixtures for the production mgTLS API."""
import argparse
import base64
import hashlib
import json
import os
import pathlib
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
MG_NOT_FOUND = -2
MG_CERTIFICATE_INVALID = -40
MG_TLS_PROTOCOL = -41
MG_TRUST_STORE_INVALID = -42
MG_CERTIFICATE_SIGNATURE = -43
MG_TIME_UNAVAILABLE = -30
MG_TIMEOUT = -18
MG_CONNECTION_RESET = -19
MG_INTERRUPTED = -39


def run(command, *, input_text=None):
    subprocess.run(command, check=True, input=input_text, text=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def generate_key(directory, name):
    key = directory / f'{name}.key'
    run(['openssl', 'genpkey', '-algorithm', 'EC', '-pkeyopt',
         'ec_paramgen_curve:P-256', '-out', str(key)])
    return key


def generate_rsa_key(directory, name, bits=2048):
    key = directory / f'{name}.key'
    run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
         f'rsa_keygen_bits:{bits}', '-out', str(key)])
    return key


def root_ca(directory, name):
    key = generate_key(directory, name)
    cert = directory / f'{name}.pem'
    run(['openssl', 'req', '-new', '-x509', '-key', str(key), '-out',
         str(cert), '-subj', f'/CN={name}', '-days', '36500', '-sha256',
         '-addext', 'basicConstraints=critical,CA:TRUE', '-addext',
         'keyUsage=critical,keyCertSign,cRLSign', '-addext',
         'subjectKeyIdentifier=hash'])
    return key, cert


def create_csr(directory, name, common_name):
    key = generate_key(directory, name)
    csr = directory / f'{name}.csr'
    run(['openssl', 'req', '-new', '-key', str(key), '-out', str(csr),
         '-subj', f'/CN={common_name}'])
    return key, csr


def create_rsa_csr(directory, name, common_name):
    key = generate_rsa_key(directory, name)
    csr = directory / f'{name}.csr'
    run(['openssl', 'req', '-new', '-key', str(key), '-out', str(csr),
         '-subj', f'/CN={common_name}'])
    return key, csr


def sign_x509(directory, csr, name, ca_cert, ca_key, extensions):
    cert = directory / f'{name}.pem'
    ext = directory / f'{name}.ext'
    ext.write_text(extensions)
    run(['openssl', 'x509', '-req', '-in', str(csr), '-CA', str(ca_cert),
         '-CAkey', str(ca_key), '-set_serial', str(100 + len(name)),
         '-days', '3000', '-sha256', '-extfile', str(ext), '-extensions',
         'server', '-out', str(cert)])
    return cert


def sign_ca(directory, csr, name, ca_cert, ca_key, *, pathlen):
    """Sign a test intermediate CA with the requested path length."""
    ext = directory / f'{name}.ext'
    ext.write_text(
        '[ca]\n'
        f'basicConstraints=critical,CA:TRUE,pathlen:{pathlen}\n'
        'keyUsage=critical,keyCertSign,cRLSign\n'
        'subjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid,issuer\n')
    cert = directory / f'{name}.pem'
    run(['openssl', 'x509', '-req', '-in', str(csr), '-CA', str(ca_cert),
         '-CAkey', str(ca_key), '-set_serial', str(200 + len(name)),
         '-days', '3000', '-sha256', '-extfile', str(ext), '-extensions',
         'ca', '-out', str(cert)])
    return cert


SERVER_EXT = """[server]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
"""


def sign_with_ca_cli(directory, csr, name, ca_cert, ca_key, extensions,
                     start, end):
    database = directory / f'{name}-index.txt'
    new_certs = directory / f'{name}-newcerts'
    new_certs.mkdir()
    serial = directory / f'{name}-serial'
    serial.write_text('1000\n')
    database.write_text('')
    ext = directory / f'{name}.ext'
    ext.write_text(extensions)
    config = directory / f'{name}-ca.cnf'
    config.write_text(f"""[ca]
default_ca=CA_default
[CA_default]
dir={directory}
database={database}
new_certs_dir={new_certs}
certificate={ca_cert}
private_key={ca_key}
serial={serial}
default_md=sha256
default_days=30
policy=policy_any
x509_extensions=server
copy_extensions=copy
unique_subject=no
[policy_any]
commonName=supplied
[server]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
""")
    cert = directory / f'{name}.pem'
    run(['openssl', 'ca', '-batch', '-config', str(config), '-in', str(csr),
         '-out', str(cert), '-startdate', start, '-enddate', end,
         '-extensions', 'server'])
    return cert


def make_manifest(bundle_path, count=1):
    content = bundle_path.read_bytes()
    manifest_path = bundle_path.with_suffix('.manifest.json')
    manifest_path.write_text(json.dumps({
        'bundle_sha256': hashlib.sha256(content).hexdigest(),
        'certificate_count': count,
    }))
    return manifest_path


def corrupt_certificate_signature(work, certificate, name):
    """Flip a signature octet without changing a certificate's DER framing."""
    encoded = b''.join(line for line in certificate.read_bytes().splitlines()
                       if not line.startswith(b'-----'))
    der = bytearray(base64.b64decode(encoded, validate=True))

    def element(offset):
        tag = der[offset]
        first = der[offset + 1]
        if first < 0x80:
            length = first
            header = 2
        else:
            size = first & 0x7f
            if size == 0 or size > 4:
                raise ValueError('unexpected DER length width')
            length = int.from_bytes(der[offset + 2:offset + 2 + size], 'big')
            header = 2 + size
        start = offset + header
        return tag, start, start + length

    tag, outer_start, outer_end = element(0)
    if tag != 0x30 or outer_end != len(der):
        raise ValueError('certificate DER outer sequence is malformed')
    _, _, tbs_end = element(outer_start)
    _, _, algorithm_end = element(tbs_end)
    signature_tag, signature_start, signature_end = element(algorithm_end)
    if signature_tag != 0x03 or signature_end != outer_end or \
            signature_end - signature_start < 2:
        raise ValueError('certificate signature BIT STRING is malformed')
    der[signature_end - 1] ^= 1
    body = base64.b64encode(der)
    output = work / f'{name}.pem'
    output.write_bytes(b'-----BEGIN CERTIFICATE-----\n' +
        b'\n'.join(body[index:index + 64]
                   for index in range(0, len(body), 64)) +
        b'\n-----END CERTIFICATE-----\n')
    return output


def write_bundle(work, name, certs):
    path = work / f'{name}-bundle.pem'
    path.write_bytes(b''.join(cert.read_bytes() for cert in certs))
    manifest = make_manifest(path, len(certs))
    return path, manifest


def make_fixtures(work):
    trusted_key, trusted_ca = root_ca(work, 'trusted-root')
    second_trusted_key, second_trusted_ca = root_ca(work, 'second-trusted-root')
    untrusted_key, untrusted_ca = root_ca(work, 'other-root')
    leaf_key, leaf_csr = create_csr(work, 'valid-server', 'localhost')
    valid = sign_x509(work, leaf_csr, 'valid-server', trusted_ca,
                      trusted_key, SERVER_EXT)
    wrong_key, wrong_csr = create_csr(work, 'wrong-host', 'wrong.test')
    wrong_ext = SERVER_EXT.replace('DNS:localhost', 'DNS:wrong.test')
    wrong = sign_x509(work, wrong_csr, 'wrong-host', trusted_ca,
                      trusted_key, wrong_ext)
    bad_signature = corrupt_certificate_signature(
        work, valid, 'bad-signature')
    other_key, other_csr = create_csr(work, 'untrusted-server', 'localhost')
    other = sign_x509(work, other_csr, 'untrusted-server', untrusted_ca,
                      untrusted_key, SERVER_EXT)
    rsa_key, rsa_csr = create_rsa_csr(work, 'rsa-server', 'localhost')
    rsa_server = sign_x509(work, rsa_csr, 'rsa-server', trusted_ca,
                           trusted_key, SERVER_EXT)
    second_key, second_csr = create_csr(work, 'second-root-server',
                                        'localhost')
    second_server = sign_x509(work, second_csr, 'second-root-server',
                              second_trusted_ca, second_trusted_key,
                              SERVER_EXT)

    int_key, int_csr = create_csr(work, 'intermediate', 'Mangrove Test Intermediate')
    int_ext = work / 'intermediate.ext'
    int_ext.write_text("""[intermediate]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
""")
    intermediate = work / 'intermediate.pem'
    run(['openssl', 'x509', '-req', '-in', str(int_csr), '-CA',
         str(trusted_ca), '-CAkey', str(trusted_key), '-set_serial', '44',
         '-days', '3000', '-sha256', '-extfile', str(int_ext),
         '-extensions', 'intermediate', '-out', str(intermediate)])
    chain_key, chain_csr = create_csr(work, 'incomplete-chain', 'localhost')
    chain_leaf = sign_x509(work, chain_csr, 'incomplete-chain', intermediate,
                           int_key, SERVER_EXT)
    chain_full = work / 'complete-chain.pem'
    chain_full.write_bytes(chain_leaf.read_bytes() + intermediate.read_bytes())

    legacy_key, legacy_root = root_ca(work, 'legacy-root')
    cross_csr = work / 'trusted-root-cross.csr'
    run(['openssl', 'req', '-new', '-key', str(trusted_key), '-out',
         str(cross_csr), '-subj', '/CN=trusted-root'])
    cross_ext = work / 'trusted-root-cross.ext'
    cross_ext.write_text("[root]\nbasicConstraints=critical,CA:TRUE,pathlen:1\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
    cross_root = work / 'trusted-root-cross.pem'
    run(['openssl', 'x509', '-req', '-in', str(cross_csr), '-CA',
         str(legacy_root), '-CAkey', str(legacy_key), '-set_serial', '45',
         '-days', '3000', '-sha256', '-extfile', str(cross_ext),
         '-extensions', 'root', '-out', str(cross_root)])
    alternate_chain = work / 'alternate-chain.pem'
    alternate_chain.write_bytes(chain_leaf.read_bytes() +
                                intermediate.read_bytes() +
                                cross_root.read_bytes())

    # Reproduce current public chains that include a several-level ECC
    # hierarchy and a cross-signed 4096-bit RSA root.  The trust bundle holds
    # both the RSA root and the self-signed ECC anchor, as the system bundle
    # does for the live 5x40.org chain.
    rsa_root_key = work / 'rsa-root-4096.key'
    rsa_root_cert = work / 'rsa-root-4096.pem'
    run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
         'rsa_keygen_bits:4096', '-out', str(rsa_root_key)])
    run(['openssl', 'req', '-new', '-x509', '-key', str(rsa_root_key),
         '-out', str(rsa_root_cert), '-subj', '/CN=4096-bit test root',
         '-days', '36500', '-sha256', '-addext',
         'basicConstraints=critical,CA:TRUE', '-addext',
         'keyUsage=critical,keyCertSign,cRLSign', '-addext',
         'subjectKeyIdentifier=hash'])
    path_root_key, path_root_cert = root_ca(work, 'path-root-x2')
    root_cross_csr = work / 'path-root-x2-cross.csr'
    run(['openssl', 'req', '-new', '-key', str(path_root_key), '-out',
         str(root_cross_csr), '-subj', '/CN=path-root-x2'])
    root_cross_ext = work / 'path-root-x2-cross.ext'
    root_cross_ext.write_text(
        '[root]\nbasicConstraints=critical,CA:TRUE,pathlen:2\n'
        'keyUsage=critical,keyCertSign,cRLSign\n'
        'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n')
    root_cross = work / 'path-root-x2-cross.pem'
    run(['openssl', 'x509', '-req', '-in', str(root_cross_csr), '-CA',
         str(rsa_root_cert), '-CAkey', str(rsa_root_key), '-set_serial',
         '81', '-days', '3000', '-sha256', '-extfile', str(root_cross_ext),
         '-extensions', 'root', '-out', str(root_cross)])

    path_int1_key, path_int1_csr = create_csr(
        work, 'path-intermediate-one', 'Root YE test')
    path_int1 = sign_ca(work, path_int1_csr, 'path-intermediate-one',
                        path_root_cert, path_root_key, pathlen=1)
    path_int2_key, path_int2_csr = create_csr(
        work, 'path-intermediate-two', 'YE2 test')
    path_int2 = sign_ca(work, path_int2_csr, 'path-intermediate-two',
                        path_int1, path_int1_key, pathlen=0)
    path_leaf_key, path_leaf_csr = create_csr(
        work, 'path-leaf', 'localhost')
    path_leaf = sign_x509(work, path_leaf_csr, 'path-leaf', path_int2,
                          path_int2_key, SERVER_EXT)
    deep_chain = work / 'rsa4096-cross-chain.pem'
    deep_chain.write_bytes(path_leaf.read_bytes() + path_int2.read_bytes() +
                           path_int1.read_bytes() + root_cross.read_bytes())
    deep_store = write_bundle(work, 'deep-trusted',
                              [rsa_root_cert, path_root_cert])

    expired_key, expired_csr = create_csr(work, 'expired-server', 'localhost')
    expired = sign_with_ca_cli(work, expired_csr, 'expired-server', trusted_ca,
        trusted_key, SERVER_EXT, '20190101000000Z', '20200101000000Z')
    future_key, future_csr = create_csr(work, 'future-server', 'localhost')
    future = sign_with_ca_cli(work, future_csr, 'future-server', trusted_ca,
        trusted_key, SERVER_EXT, '20990101000000Z', '21000101000000Z')

    valid_store = write_bundle(work, 'trusted',
                               [trusted_ca, second_trusted_ca])
    return {
        'valid': (valid, leaf_key),
        'rsa-server': (rsa_server, rsa_key),
        'second-root-server': (second_server, second_key),
        'wrong': (wrong, wrong_key),
        'bad-signature': (bad_signature, leaf_key),
        'untrusted': (other, other_key),
        'incomplete': (chain_leaf, chain_key),
        'complete': (chain_full, chain_key),
        'alternate': (alternate_chain, chain_key),
        'expired': (expired, expired_key),
        'future': (future, future_key),
        'trusted_ca': trusted_ca,
        'untrusted_store': write_bundle(work, 'other', [untrusted_ca]),
        'trusted_store': valid_store,
        'deep-chain': (deep_chain, path_leaf_key),
        'deep_store': deep_store,
        'empty_store': write_bundle(work, 'empty', []),
    }


class LocalTLSServer:
    def __init__(self, cert_path, key_path, behavior='exchange'):
        self.behavior = behavior
        self.sni = []
        self.accepted = False
        self.failure = None
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(3.0)
        self.port = self.listener.getsockname()[1]
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_3
        self.context.maximum_version = ssl.TLSVersion.TLSv1_3
        self.context.set_ecdh_curve('prime256v1')
        self.context.load_cert_chain(str(cert_path), str(key_path))
        self.context.set_servername_callback(
            lambda _ssl, name, _ctx: self.sni.append(name))
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def _serve(self):
        try:
            raw, _ = self.listener.accept()
            self.accepted = True
            if self.behavior in ('timeout', 'interrupted'):
                time.sleep(0.6)
                raw.close()
                return
            try:
                conn = self.context.wrap_socket(raw, server_side=True)
            except (ssl.SSLError, ConnectionError) as error:
                self.failure = error
                raw.close()
                return
            if self.behavior == 'reset':
                time.sleep(0.15)
                fd = conn.detach()
                raw_socket = socket.socket(fileno=fd)
                raw_socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                      struct.pack('ii', 1, 0))
                raw_socket.close()
                return
            request = bytearray()
            while len(request) < 4:
                part = conn.recv(4 - len(request))
                if not part:
                    break
                request.extend(part)
            if bytes(request) == b'PING':
                for byte in b'PONG':
                    conn.sendall(bytes((byte,)))
                try:
                    conn.unwrap()
                except (ssl.SSLError, ConnectionError, OSError):
                    conn.close()
            elif bytes(request) == b'PULL':
                total = 3 * 1024 * 1024
                for start in range(0, total, 4093):
                    end = min(start + 4093, total)
                    conn.sendall(bytes(((index * 31 + index // 257) & 0xff)
                                       for index in range(start, end)))
                try:
                    conn.unwrap()
                except (ssl.SSLError, ConnectionError, OSError):
                    conn.close()
            else:
                conn.close()
        except socket.timeout:
            pass
        except BaseException as error:
            self.failure = error
        finally:
            self.listener.close()

    def finish(self):
        self.thread.join(timeout=4.0)
        if self.thread.is_alive():
            raise AssertionError('TLS fixture server did not terminate')
        if self.failure:
            raise AssertionError(f'TLS fixture server failed: {self.failure}')


def client(binary, mode, host, port, expected, bundle, manifest, timeout=2000):
    env = os.environ.copy()
    env['MGTLS_TEST_CA_BUNDLE'] = str(bundle)
    env['MGTLS_TEST_CA_MANIFEST'] = str(manifest)
    return subprocess.run([str(binary), mode, host, str(port), str(expected),
                           str(timeout)], env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=8)


def assert_client(result, label):
    if result.returncode != 0:
        raise AssertionError(f'{label}: client exit {result.returncode}; '
                             f'stdout={result.stdout!r}; stderr={result.stderr!r}')


def exercise_aes128_gcm_server(binary, fixtures):
    cert, key = fixtures['valid']
    bundle, manifest = fixtures['trusted_store']
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    server = subprocess.Popen([
        'openssl', 's_server', '-accept', f'127.0.0.1:{port}',
        '-tls1_3', '-ciphersuites', 'TLS_AES_128_GCM_SHA256',
        '-cert', str(cert), '-key', str(key), '-www', '-naccept', '1'],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.15)
        if server.poll() is not None:
            raise AssertionError('AES-128-only OpenSSL server exited before handshake')
        result = client(binary, 'http', 'localhost', port, 0, bundle, manifest)
        assert_client(result, 'TLS_AES_128_GCM_SHA256-only server')
        try:
            _, error = server.communicate(timeout=5.0)
        except subprocess.TimeoutExpired:
            server.kill()
            _, error = server.communicate()
            raise AssertionError('AES-128-only OpenSSL server did not finish')
        if server.returncode != 0:
            raise AssertionError('AES-128-only OpenSSL server exited with '
                                 f'{server.returncode}: {error!r}')
    finally:
        if server.poll() is None:
            server.kill()
            server.communicate()


def exercise(binary, work):
    fixtures = make_fixtures(work)
    trusted_bundle, trusted_manifest = fixtures['trusted_store']

    def server_case(name, host='localhost', expected=0, behavior='exchange',
                    timeout=2000, store=None, client_mode=None):
        cert, key = fixtures[name]
        active_store = store or fixtures['trusted_store']
        fixture_server = LocalTLSServer(cert, key, behavior).start()
        mode = client_mode or ('timeout' if behavior == 'timeout' else
                               'interrupted' if behavior == 'interrupted' else
                               'reset' if behavior == 'reset' else 'exchange')
        result = client(binary, mode,
                        host, fixture_server.port, expected,
                        active_store[0], active_store[1], timeout)
        try:
            fixture_server.finish()
        except AssertionError as error:
            if expected == 0 or not fixture_server.accepted:
                raise AssertionError(f'{name}: server-side TLS failure: '
                                     f'{error}; client stdout={result.stdout!r}; '
                                     f'stderr={result.stderr!r}') from error
        assert_client(result, name)
        return fixture_server

    successful = server_case('valid')
    if successful.sni != ['localhost']:
        raise AssertionError(f'DNS SNI was not sent: {successful.sni!r}')
    ip_server = server_case('valid', host='127.0.0.1')
    if ip_server.sni != [None]:
        raise AssertionError(f'IP literal unexpectedly sent DNS SNI: {ip_server.sni!r}')
    server_case('valid', client_mode='stream')
    server_case('valid', client_mode='large')
    server_case('rsa-server')
    server_case('second-root-server')
    exercise_aes128_gcm_server(binary, fixtures)
    server_case('wrong', host='localhost', expected=MG_CERTIFICATE_INVALID)
    server_case('bad-signature', expected=MG_CERTIFICATE_SIGNATURE)
    server_case('untrusted', expected=MG_CERTIFICATE_INVALID)
    server_case('expired', expected=MG_CERTIFICATE_INVALID)
    server_case('future', expected=MG_CERTIFICATE_INVALID)
    server_case('incomplete', expected=MG_CERTIFICATE_INVALID)
    server_case('complete')
    server_case('alternate')
    server_case('deep-chain', store=fixtures['deep_store'])
    server_case('alternate', expected=MG_CERTIFICATE_INVALID,
                store=fixtures['untrusted_store'])
    server_case('valid', behavior='timeout', expected=MG_TIMEOUT, timeout=150)
    server_case('valid', behavior='interrupted', expected=MG_INTERRUPTED)
    server_case('valid', behavior='reset', expected=MG_CONNECTION_RESET)

    result = client(binary, 'untrusted-time', 'localhost', 443,
                    MG_TIME_UNAVAILABLE, trusted_bundle, trusted_manifest)
    assert_client(result, 'untrusted realtime')

    empty_bundle, empty_manifest = fixtures['empty_store']
    result = client(binary, 'context', 'localhost', 443,
                    MG_TRUST_STORE_INVALID, empty_bundle, empty_manifest)
    assert_client(result, 'empty trust store')

    corrupt_bundle = work / 'corrupt-bundle.pem'
    corrupt_bundle.write_bytes(b'-----BEGIN CERTIFICATE-----\nnot-base64\n'
                               b'-----END CERTIFICATE-----\n')
    corrupt_manifest = make_manifest(corrupt_bundle, 1)
    result = client(binary, 'context', 'localhost', 443,
                    MG_TRUST_STORE_INVALID, corrupt_bundle, corrupt_manifest)
    assert_client(result, 'malformed trust store')

    altered_bundle = work / 'altered-bundle.pem'
    altered_bundle.write_bytes(trusted_bundle.read_bytes() + b'\ncorrupt')
    result = client(binary, 'context', 'localhost', 443,
                    MG_TRUST_STORE_INVALID, altered_bundle, trusted_manifest)
    assert_client(result, 'trust bundle digest mismatch')

    missing_bundle = work / 'missing-bundle.pem'
    missing_manifest = work / 'missing-manifest.json'
    result = client(binary, 'context', 'localhost', 443, MG_NOT_FOUND,
                    missing_bundle, missing_manifest)
    assert_client(result, 'missing trust bundle')
    print('MGTLS HOST PASS TLS1.3 ECDSA/RSA servers AES-128-GCM-only peer '
          'multiple trust roots '
          'DNS/IP SAN multi-megabyte fragmented records trust time '
          '4096-bit cross-sign signature rejection timeout interrupt reset')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', required=True, type=pathlib.Path)
    args = parser.parse_args()
    if shutil.which('openssl') is None:
        raise SystemExit('openssl is required to generate temporary TLS test certificates')
    with tempfile.TemporaryDirectory(prefix='mgtls-tls-fixture-') as folder:
        exercise(args.client.resolve(), pathlib.Path(folder))


if __name__ == '__main__':
    main()
