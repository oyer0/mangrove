#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Import the reviewed wolfSSL source subset used by Mangrove mgTLS.

The source archive is never trusted by filename alone. The caller supplies the
expected digest from the reviewed upstream release page, and imports always
create a new versioned destination instead of overwriting an existing tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import zipfile


def selected(path: PurePosixPath, root: str) -> bool:
    if len(path.parts) < 2 or path.parts[0] != root:
        return False
    rel = PurePosixPath(*path.parts[1:])
    if rel.parts[0] in ("src", "wolfssl"):
        return True
    if rel.parts[0] == "wolfcrypt" and len(rel.parts) >= 3:
        # Mangrove uses wolfCrypt's generic C implementation. Hardware/OS
        # ports under wolfcrypt/src/port are intentionally not imported.
        return rel.parts[1] == "src" and len(rel.parts) == 3
    return rel.as_posix() in ("COPYING", "LICENSING", "README", "ChangeLog.md")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--version", required=True,
                        help="upstream version, for example 5.9.4")
    parser.add_argument("--sha256", required=True,
                        help="reviewed SHA-256 for the complete source archive")
    parser.add_argument("--destination", type=Path, required=True,
                        help="new versioned snapshot directory (must not exist)")
    args = parser.parse_args()

    if not args.version or any(c not in "0123456789." for c in args.version):
        parser.error("version must contain only digits and dots")
    expected = args.sha256.lower()
    if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
        parser.error("--sha256 must be 64 hexadecimal characters")

    archive = args.archive.resolve(strict=True)
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual != expected:
        raise SystemExit(f"archive SHA-256 mismatch: expected {expected}, got {actual}")

    root_name = f"wolfssl-{args.version}"
    destination = args.destination.absolute()
    if destination.exists():
        raise SystemExit(f"refusing to replace existing snapshot: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{root_name}-", dir=destination.parent))
    try:
        copied = []
        with zipfile.ZipFile(archive) as bundle:
            for info in bundle.infolist():
                name = PurePosixPath(info.filename)
                if name.is_absolute() or ".." in name.parts or not name.parts:
                    raise SystemExit(f"unsafe archive path: {info.filename!r}")
                if name.parts[0] != root_name:
                    raise SystemExit(f"unexpected archive root: {info.filename!r}")
                if info.is_dir() or not selected(name, root_name):
                    continue
                mode = (info.external_attr >> 16) & 0xFFFF
                if stat.S_ISLNK(mode) or not (stat.S_ISREG(mode) or mode == 0):
                    raise SystemExit(f"unsupported archive entry type: {info.filename!r}")
                relative = Path(*name.parts[1:])
                target = stage / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with bundle.open(info) as src, target.open("xb") as dst:
                    shutil.copyfileobj(src, dst, 1024 * 1024)
                os.chmod(target, 0o644)
                copied.append(relative.as_posix())

        required = {"COPYING", "LICENSING", "src/ssl.c", "src/tls13.c",
                    "wolfssl/ssl.h", "wolfssl/wolfcrypt/settings.h",
                    "wolfcrypt/src/aes.c", "wolfcrypt/src/random.c"}
        missing = required.difference(copied)
        if missing:
            raise SystemExit("archive is missing required source entries: " +
                             ", ".join(sorted(missing)))
        manifest = {
            "upstream": "wolfSSL",
            "version": args.version,
            "archive": f"{root_name}.zip",
            "archive_sha256": actual,
            "source_url": "https://www.wolfssl.com/download/",
            "license": "GPL-3.0-only; see LICENSING and COPYING",
            "imported_components": [
                "src/ (upstream TLS/X.509 implementation and headers)",
                "wolfssl/ (upstream public/internal headers)",
                "wolfcrypt/src/* (upstream generic crypto implementation files)",
                "COPYING, LICENSING, README, ChangeLog.md",
            ],
            "excluded_components": [
                "wolfcrypt/src/port/ platform and hardware-specific ports",
                "examples, certificates, build outputs, and unrelated tooling",
            ],
            "files": sorted(copied),
        }
        (stage / "MG_IMPORT.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        os.replace(stage, destination)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    print(f"Imported {len(copied)} upstream files into {destination}")
    print(f"Verified SHA-256 {actual}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
