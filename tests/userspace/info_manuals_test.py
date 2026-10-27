#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check installed manual coverage, section structure, and help alignment."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import populate_mgfs

HELP_DIR = ROOT / "share" / "help"
INFO_DIR = ROOT / "share" / "info"
SECTIONS = (
    "NAME", "SYNOPSIS", "DESCRIPTION", "OPTIONS", "ESCAPES", "KEYBINDINGS",
    "CONFIGURATION", "COMMANDS", "EXAMPLES", "NOTES", "SEE ALSO",
)
HEADING = re.compile(r"^([A-Z][A-Z ]*)$", re.MULTILINE)
OPTION_NAME = re.compile(r"(?<![A-Za-z0-9_])(--[a-z][a-z0-9-]*|-[A-Za-z])")


def records():
    result = {}
    for path in sorted(HELP_DIR.iterdir()):
        if path.is_file() and not path.name.startswith("."):
            result[path.name] = path.read_text(encoding="utf-8")
    return result


def sections(text, path):
    matches = list(HEADING.finditer(text))
    assert matches, f"{path}: no uppercase manual headings"
    found = {}
    order = []
    for index, match in enumerate(matches):
        name = match.group(1)
        assert name in SECTIONS, f"{path}: malformed or unknown heading {name!r}"
        assert match.start() == 0 or text[match.start() - 2:match.start()] == "\n\n", \
            f"{path}: {name} heading must be separated by a blank line"
        assert text[match.end():match.end() + 2] == "\n\n", \
            f"{path}: {name} heading must be followed by a blank line"
        assert name not in found, f"{path}: duplicate {name} section"
        body_start = match.end()
        if text[body_start:body_start + 2] == "\r\n":
            body_start += 2
        elif text[body_start:body_start + 1] == "\n":
            body_start += 1
        next_start = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        body = text[body_start:next_start].strip()
        assert body, f"{path}: empty {name} section"
        found[name] = body
        order.append(SECTIONS.index(name))
    assert order == sorted(order), f"{path}: sections are out of order"
    assert text.endswith("\n"), f"{path}: missing final newline"
    return found


def parse_help(text, name):
    fields = {}
    for line in text.splitlines():
        if not line or line.startswith("//"):
            continue
        key, separator, value = line.partition("=")
        assert separator and value.strip(), f"help/{name}: malformed line {line!r}"
        fields.setdefault(key.strip(), []).append(value.strip())
    return fields


def main():
    help_records = records()
    manual_names = {path.name for path in INFO_DIR.iterdir()
                    if path.is_file() and not path.name.startswith(".")}
    assert manual_names == set(help_records), (
        f"manual coverage mismatch: missing={sorted(set(help_records) - manual_names)}, "
        f"extra={sorted(manual_names - set(help_records))}")
    assert set(populate_mgfs.INFO_FILES) == set(help_records), \
        "image manifest must install a manual for every native help record"
    assert len(populate_mgfs.INFO_RECORD_IDS) == len(help_records)
    assert len(set(populate_mgfs.INFO_RECORD_IDS)) == len(help_records)
    ca_ids = set(range(populate_mgfs.CA_STORE_RECORD_ID,
                       populate_mgfs.CA_STORE_FIRST_FILE_RECORD_ID +
                       len(populate_mgfs.CA_STORE_FILES)))
    assert not ca_ids.intersection(populate_mgfs.INFO_RECORD_IDS), \
        "manual Record IDs overlap the CA trust store"

    for name, raw_help in help_records.items():
        path = INFO_DIR / name
        text = path.read_text(encoding="utf-8")
        section = sections(text, path)
        help_fields = parse_help(raw_help, name)
        for required in ("NAME", "SYNOPSIS", "DESCRIPTION"):
            assert required in section, f"{path}: missing {required}"
        assert re.search(rf"^{re.escape(name)}\s+-\s+\S", section["NAME"], re.M), \
            f"{path}: NAME does not identify the command"
        for usage in help_fields.get("usage", []):
            assert usage in section["SYNOPSIS"], \
                f"{path}: synopsis omits installed usage {usage!r}"

        options = help_fields.get("option", [])
        if options:
            assert "OPTIONS" in section, f"{path}: options missing from manual"
            manual_options = section["OPTIONS"]
            for entry in options:
                for flag in OPTION_NAME.findall(entry):
                    assert flag in manual_options, \
                        f"{path}: manual omits help option {flag}"
        else:
            assert "OPTIONS" not in section, \
                f"{path}: empty options section should be omitted"

        if help_fields.get("keybinding"):
            assert "KEYBINDINGS" in section, \
                f"{path}: interactive controls missing from manual"
        if help_fields.get("command"):
            assert "COMMANDS" in section, \
                f"{path}: internal commands missing from manual"

        lowered = text.lower()
        assert "the invocation forms," not in lowered, \
            f"{path}: generic placeholder text is not manual content"
        assert not any(term in lowered for term in
                       ("\x1b", "mgtc", "ansi", "vt100", "csi", "osc")), \
            f"{path}: contains a retired terminal protocol"

    reed = (INFO_DIR / "reed").read_text(encoding="utf-8")
    reed_sections = sections(reed, INFO_DIR / "reed")
    assert "CONFIGURATION" in reed_sections
    for binding in ("h", "Left", "j", "Down", "k", "Up", "l", "Right",
                    "b", "B", "n", "N", "[", "]", "s", "S", "Alt+S",
                    "o", "O", "u", "U", "Ctrl+Home", "Ctrl+End"):
        assert re.search(rf"^\s*{re.escape(binding)}\s+[-—]", reed_sections["KEYBINDINGS"], re.M), \
            f"info reed: binding {binding} is not listed on its own line"
    assert "wrap=true" in reed_sections["CONFIGURATION"]
    assert "tab_width=4" in reed_sections["CONFIGURATION"]
    assert "127 bytes" not in reed and "1023 bytes" not in reed
    for line in reed_sections["KEYBINDINGS"].splitlines():
        if line.startswith("    ") and " — " in line:
            binding = line.strip().split(" — ", 1)[0]
            assert binding == "/" or "/" not in binding, \
                f"info reed: multiple keybindings share one entry: {binding!r}"

    shoot = sections((INFO_DIR / "shoot").read_text(encoding="utf-8"),
                     INFO_DIR / "shoot")
    assert "CONFIGURATION" in shoot
    for key in ("prompt_color=green", "history_limit=256",
                "history_ignore_space=true"):
        assert key in shoot["CONFIGURATION"]
    assert "Unknown keys are ignored" not in shoot["CONFIGURATION"]

    say = sections((INFO_DIR / "say").read_text(encoding="utf-8"),
                   INFO_DIR / "say")
    for command in ("fg:COLOR", "bg:COLOR", "warn", "err", "ok", "head",
                    "reset", "cls", "cll", "cleol", "at:ROW,COL", "up:N",
                    "down:N", "left:N", "right:N", "hide", "show", "alt",
                    "main"):
        assert command in say["ESCAPES"], \
            f"info say: missing canonical terminal command {command}"
    for color in ("black", "red", "green", "yellow", "blue", "magenta",
                  "cyan", "l_gray", "d_gray", "b_red", "b_green",
                  "b_yellow", "b_blue", "b_magenta", "b_cyan", "white"):
        assert color in say["ESCAPES"], \
            f"info say: missing canonical palette name {color}"
    for escape in (r"\\", r"\n", r"\r", r"\t", r"\e"):
        assert escape in say["ESCAPES"], \
            f"info say: missing backslash escape {escape}"
    assert "Unknown escapes" in say["ESCAPES"]
    assert "remain literal" in say["ESCAPES"]
    assert "ESC byte itself is displayed as U+FFFD" in say["ESCAPES"]


if __name__ == "__main__":
    main()
