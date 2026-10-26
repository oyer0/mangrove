#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate and reproducibly import Mangrove's Mozilla-derived CA bundle.

This host-side tool checks certificate encoding and provenance metadata. It is
not a TLS verifier and is never linked into the kernel or native programs.
"""
import argparse
import base64
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import ssl
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / "share" / "ca-certificates"
BUNDLE_NAME = "ca-bundle.pem"
MANIFEST_NAME = "manifest.json"
LICENSE_NAME = "MPL-2.0.txt"
SOURCE_URL = "https://curl.se/ca/cacert.pem"
MAX_IMPORT_BYTES = 16 * 1024 * 1024
CERTIFICATE = re.compile(
    rb"(-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----)",
    re.DOTALL,
)


def _manifest_for(bundle):
    if len(bundle) > MAX_IMPORT_BYTES:
        raise ValueError("CA bundle exceeds the host import bound")
    snapshot = re.search(rb"^## Certificate data from Mozilla as of: (.+)$",
                         bundle, re.MULTILINE)
    source_hash = re.search(rb"^## SHA256: ([0-9a-f]{64})$", bundle,
                            re.MULTILINE)
    converter = re.search(rb"^## Conversion done with (mk-ca-bundle\.pl version [0-9]+(?:\.[0-9]+)*)\.$",
                          bundle, re.MULTILINE)
    if not snapshot or not source_hash or not converter:
        raise ValueError("bundle lacks the expected curl/Mozilla provenance header")
    source_hash_text = source_hash.group(1).decode("ascii")
    try:
        snapshot_text = snapshot.group(1).decode("ascii")
        converter_text = converter.group(1).decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("bundle provenance header is not ASCII") from error
    if not snapshot_text.endswith(" GMT"):
        raise ValueError("unsupported CA source snapshot")

    begins = bundle.count(b"-----BEGIN CERTIFICATE-----")
    ends = bundle.count(b"-----END CERTIFICATE-----")
    blocks = CERTIFICATE.findall(bundle)
    if not blocks or begins != len(blocks) or ends != len(blocks):
        raise ValueError("CA bundle is empty or contains malformed PEM framing")
    pem_blocks = []
    for full_block, body in blocks:
        compact = re.sub(rb"\s+", b"", body)
        try:
            der = base64.b64decode(compact, validate=True)
        except (ValueError, binascii.Error) as error:
            raise ValueError("CA bundle contains invalid base64") from error
        if not der or der[0] != 0x30:
            raise ValueError("CA bundle contains a non-certificate DER object")
        pem_blocks.append(full_block)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    try:
        context.load_verify_locations(
            cadata=b"\n".join(pem_blocks).decode("ascii"))
    except (ssl.SSLError, ValueError) as error:
        raise ValueError("OpenSSL could not parse every CA certificate") from error
    parsed_count = context.cert_store_stats().get("x509", 0)
    if parsed_count != len(blocks):
        raise ValueError("CA parser count differs from PEM certificate count")
    return {
        "schema_version": 1,
        "source": "Mozilla Root Store through curl CA Extract",
        "source_url": SOURCE_URL,
        "source_snapshot": snapshot_text,
        "mozilla_certdata_sha256": source_hash_text,
        "converter": converter_text,
        "bundle_sha256": hashlib.sha256(bundle).hexdigest(),
        "certificate_count": parsed_count,
        "license": "MPL-2.0",
        "license_file": LICENSE_NAME,
    }


def verify_store(bundle_path=None, manifest_path=None, license_path=None):
    bundle_path = Path(bundle_path or STORE / BUNDLE_NAME)
    manifest_path = Path(manifest_path or STORE / MANIFEST_NAME)
    license_path = Path(license_path or STORE / LICENSE_NAME)
    try:
        bundle = bundle_path.read_bytes()
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        license_text = license_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("CA store file is missing, inaccessible, or malformed") from error
    expected = _manifest_for(bundle)
    if manifest != expected:
        raise ValueError("CA store manifest does not match the validated bundle")
    if "Mozilla Public License" not in license_text or \
            "Version 2.0" not in license_text:
        raise ValueError("Mozilla Public License 2.0 text is missing or invalid")
    return expected


def _atomic_write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix="." + path.name + ".",
                                              dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def import_bundle(source_path):
    source_path = Path(source_path)
    bundle = source_path.read_bytes()
    manifest = _manifest_for(bundle)
    # Keep the upstream PEM byte-for-byte. Replacing the bundle before its
    # manifest can leave a detectable mismatch after interruption, which the
    # image builders reject rather than packaging as an accepted trust store.
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    _atomic_write(STORE / BUNDLE_NAME, bundle)
    _atomic_write(STORE / MANIFEST_NAME, manifest_bytes)
    verify_store()
    return manifest


def load_payloads():
    """Return verified installed file bytes for fresh/update image builders."""
    verify_store()
    return {
        BUNDLE_NAME: (STORE / BUNDLE_NAME).read_bytes(),
        MANIFEST_NAME: (STORE / MANIFEST_NAME).read_bytes(),
        LICENSE_NAME: (STORE / LICENSE_NAME).read_bytes(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("verify", help="validate the checked-in trust store")
    importer = subparsers.add_parser("import", help="import a reviewed curl Mozilla CA Extract PEM")
    importer.add_argument("bundle", type=Path)
    args = parser.parse_args()
    try:
        result = verify_store() if args.action == "verify" else import_bundle(args.bundle)
    except (OSError, ValueError) as error:
        parser.exit(1, "ca_store: " + str(error) + "\n")
    print("CA store valid: %s certificates, %s, bundle sha256=%s" % (
        result["certificate_count"], result["source_snapshot"],
        result["bundle_sha256"]))


if __name__ == "__main__":
    main()
