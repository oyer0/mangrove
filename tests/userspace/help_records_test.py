#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check help-record section semantics against the installed source records."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELP_DIR = ROOT / "share" / "help"
SECTIONS = {"usage", "option", "keybinding", "command", "note"}
OPTION_FLAGS = {
    "del": {"-h", "-r"},
    "fetch": {"-h", "-o", "-q", "-i", "--timings"},
    "history": {"-h", "-c", "-d", "-a", "-n", "-r", "-w"},
    "list": {"-h", "-l", "-t", "--tree"},
    "lsdsk": {"-h", "-v"},
    "lspci": {"-h", "-v"},
    "lsusb": {"-h", "-v"},
    "ping": {"-h", "-c", "-i"},
    "say": {"-h", "-e", "-n"},
    "user": {"-h", "-p"},
}


def load_records():
    records = {}
    for path in sorted(HELP_DIR.iterdir()):
        if not path.is_file() or path.name.startswith("."):
            continue
        fields = {"usage": [], "option": [], "keybinding": [],
                  "command": [], "note": []}
        metadata = {}
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("//"):
                continue
            key, separator, value = line.partition("=")
            assert separator and value.strip(), f"malformed {path.name}: {raw_line}"
            key, value = key.strip(), value.strip()
            assert key in SECTIONS | {"name", "category", "description"}, \
                f"unknown field {key!r} in {path.name}"
            if key in SECTIONS:
                fields[key].append(value)
            else:
                assert key not in metadata, f"duplicate {key} in {path.name}"
                metadata[key] = value
        assert metadata.get("name") == path.name, f"name mismatch in {path.name}"
        assert all(metadata.get(key) for key in ("category", "description")), \
            f"missing metadata in {path.name}"
        assert fields["usage"], f"missing Usage in {path.name}"
        assert len(fields["usage"]) == len(set(fields["usage"])), \
            f"duplicate Usage line in {path.name}"
        for entry in fields["option"]:
            assert entry.startswith("-"), \
                f"non-flag classified as an option in {path.name}: {entry}"
        records[path.name] = fields
    return records


def option_flags(record):
    return {entry.split()[0].rstrip(",") for entry in record["option"]}


def require(records, name, section, text):
    assert any(text in entry for entry in records[name][section]), \
        f"{name} missing {section} entry containing {text!r}"


def main():
    records = load_records()
    assert len(records) == 44, f"expected 44 installed records, found {len(records)}"
    for name, record in records.items():
        assert option_flags(record) == OPTION_FLAGS.get(name, {"-h"}), \
            f"{name} quick-help flags do not match its invocation parser"

    for name in ("info", "view"):
        assert records[name]["usage"] == [f"{name} " +
                                          ("PROGRAM" if name == "info" else "FILE")]
        assert option_flags(records[name]) == {"-h"}
        assert records[name]["keybinding"]
        assert not any("j / Down" in item or "info PROGRAM" in item or
                       "view FILE" in item for item in records[name]["option"])
        for key in ("j / Down", "k / Up", "Page Up/Down", "g / Home",
                    "G / End", "?", "n / b", "q", "Escape"):
            require(records, name, "keybinding", key)

    assert records["reed"]["usage"] == ["reed [FILE]"]
    assert option_flags(records["reed"]) == {"-h"}
    for command in ("/s", "/q", "/sq", "/saveas", "/open", "/find"):
        require(records, "reed", "command", command)
    require(records, "reed", "keybinding", "Alt+S")
    assert not any(entry.startswith("FILE") or entry.startswith("/")
                   for entry in records["reed"]["option"])

    assert records["shoot"]["usage"] == ["shoot [SCRIPT]"]
    assert option_flags(records["shoot"]) == {"-h"}
    require(records, "shoot", "note", "SCRIPT")

    assert records["say"]["usage"] == [
        "say [-e|--escapes] [-n|--no-newline] [--] [text ...]"]
    require(records, "say", "option", "-e, --escapes")
    require(records, "say", "option", "-n, --no-newline")
    require(records, "say", "note", "without it, backslashes stay literal")
    require(records, "say", "note", "Redirected output receives emitted bytes unchanged")
    assert not any("Example:" in item for item in records["say"]["note"])

    assert records["fetch"]["usage"] == [
        "fetch [OPTIONS] URL...", "fetch [OPTIONS] --input-file=FILE"]
    require(records, "fetch", "option", "--output=FILE")
    require(records, "fetch", "option", "--quiet")
    require(records, "fetch", "option", "--input-file=FILE")
    require(records, "fetch", "note", "HTTPS")
    require(records, "fetch", "option", "--timings")

    for name in ("copy", "move"):
        assert option_flags(records[name]) == {"-h"}
        require(records, name, "note", "Multiple sources")
    assert option_flags(records["del"]) == {"-r", "-h"}
    assert len(records["del"]["note"]) == 4
    require(records, "del", "note", "pathname patterns")
    require(records, "diskutil", "command", "format <target>")
    assert len(records["diskutil"]["command"]) == 11
    require(records, "tmon", "keybinding", "q")


if __name__ == "__main__":
    main()
