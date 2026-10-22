# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioral evidence from executed responses, separate from profile labels."""
from collections import Counter
from model import Model, BACKENDS
from images import initial_files

BOUNDARIES = (0, 1, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 65535, 65536)
BASE = {"exclusive-empty", "append", "overwrite", "zero-write", "eof-read",
        "namespace-list", "remount", "busy-unmount", "reopen-verified",
        "missing-open", "exclusive-collision"}
GENERATED = BASE | {"ordinary-create", "mkdir", "rmdir", "nonempty-rmdir", "rename",
    "unlink", "recreation", "directory-stat", "interior-write", "overlap-extend",
    "repeated-open-overwrite", "nonempty-exclusive", "snapshot", "safe-replace",
    "stale-refusal", "rebind", "mixed-safe-lifecycle", "size-65535", "size-65536",
    "shrink-from-65536"}


def evidence(trace, observed, backend, layout, campaign):
    model = Model(backend, initial_files(layout))
    witnesses = set()
    frequencies, statuses = Counter(), Counter()
    content = namespace = verified = 0
    ranges, offsets, lengths, resize_lengths = set(), set(), set(), set()
    sizes = set()
    phase, pending, spans = {}, {}, {}
    removed = set()
    session_writes = []
    seen_paths = set(initial_files(layout))
    session_reopen = False
    stale_handle = rebound = False
    imported_untouched = set(initial_files(layout))
    preallocated_empty = preallocated_started = False
    preallocated_path = None
    preallocated_listed = preallocated_remounted = False
    missed_checks = 0
    remount_pending = set()
    missed_remount_checks = 0
    for command, got in zip(trace, observed):
        op = command["op"]
        frequencies[op] += 1
        statuses[str(got["status"])] += 1
        opened = model.opened
        old = model.files.get(opened, b"")
        offset = model.offset
        path = model.key(command["path"]) if "path" in command else None
        size = len(bytes.fromhex(command.get("data", "")))
        expected = model.apply(command)
        success = got["status"] == 0 and expected["status"] == 0
        if op == "WRITE":
            success = success and got["count"] == size
        mutation = False
        if success:
            if layout == "preallocated-directory":
                if op == "LIST" and command.get("path", "/") == "/":
                    if not preallocated_started and not got["count"]:
                        preallocated_empty = True
                    names = bytes.fromhex(got["data"]).split(b"\0")
                    if preallocated_path and preallocated_path[1:].encode() in names:
                        preallocated_listed = True
                        if preallocated_remounted:
                            witnesses.add("preallocated-directory-persistence")
                if (not preallocated_started and
                    op in ("CREATE", "CREATE_ORDINARY", "CREATE_CONTENTS", "MKDIR") and
                    path.count("/") == 1):
                    preallocated_started = True
                    if preallocated_empty: preallocated_path = path
                if op in ("UNLINK", "RMDIR", "RENAME") and path == preallocated_path:
                    preallocated_path = None  # Recreation cannot prove this identity persisted.
                if op == "REMOUNT" and preallocated_listed:
                    preallocated_remounted = True
            if op in ("CREATE", "CREATE_ORDINARY", "CREATE_CONTENTS"):
                namespace += 1
                witnesses.add({"CREATE": "exclusive-empty", "CREATE_ORDINARY": "ordinary-create",
                               "CREATE_CONTENTS": "nonempty-exclusive" if size else "exclusive-empty"}[op])
                if path in removed: witnesses.add("recreation")
                session_writes = []
                seen_paths.add(path); session_reopen = False
                stale_handle = rebound = False
                if size: opened = path; mutation = True
            elif op == "OPEN":
                session_writes = []
                session_reopen = path in seen_paths
                seen_paths.add(path)
                stale_handle = rebound = False
            elif op == "WRITE":
                if size:
                    end = offset + size
                    ranges.add((offset, size)); offsets.add(offset); lengths.add(size)
                    category = ("gap" if offset > len(old) else "append" if offset == len(old)
                                else "overlap-extend" if end > len(old) else "overwrite")
                    witnesses.add(category)
                    if 0 < offset < len(old) and offset not in BOUNDARIES:
                        witnesses.add("interior-write")
                    if any(offset < b and a < end for a, b in session_writes):
                        witnesses.add("repeated-open-overwrite")
                    session_writes.append((offset, end))
                    prior = phase.get(opened, 0)
                    phase[opened] = (4 if prior in (3, 4) else
                                     1 if end > len(old) or prior in (1, 2) else 0)
                    mutation = True
                else: witnesses.add("zero-write")
            elif op in ("TRUNCATE", "RESIZE"):
                target = command["size"]
                resize_lengths.add(target)
                if target != len(old):
                    witnesses.add("shrink" if target < len(old) else "resize-grow")
                    if len(old) == 65536 and target < 65536: witnesses.add("shrink-from-65536")
                    prior = phase.get(opened, 0)
                    phase[opened] = (4 if prior in (3, 4) else
                                     1 if target > len(old) or prior in (1, 2) else 0)
                    mutation = True
            elif op == "REPLACE":
                witnesses.add("safe-replace")
                if phase.get(opened) == 2: phase[opened] = 3
                mutation = bool(old or size)
            elif op == "SNAPSHOT":
                # Count and complete byte payload were independently compared.
                if got["count"] == len(old) and got["data"] == old.hex():
                    witnesses.add("snapshot")
                    remount_pending.discard(opened)
                    if phase.get(opened) == 6: phase[opened] = 7
                    rebound = stale_handle
                    if phase.get(opened) == 1: phase[opened] = 2
                    if opened in pending:
                        verified += 1; pending.pop(opened); spans.pop(opened, None)
            elif op == "READ":
                end = offset + got["count"]
                if got["count"] == 0 and command["size"] and offset >= len(old):
                    witnesses.add("eof-read")
                if offset == 0: spans[opened] = end
                elif offset == spans.get(opened): spans[opened] = end
                if spans.get(opened) == len(old):
                    remount_pending.discard(opened)
                    if phase.get(opened) == 6: phase[opened] = 7
                    if session_reopen: witnesses.add("reopen-verified")
                    if opened in pending:
                        verified += 1; pending.pop(opened)
                    if opened in imported_untouched: witnesses.add("imported-read")
            elif op == "LIST": witnesses.add("namespace-list")
            elif op == "STAT" and got["count"] == 2: witnesses.add("directory-stat")
            elif op == "MKDIR": namespace += 1; witnesses.add("mkdir")
            elif op == "RMDIR": namespace += 1; witnesses.add("rmdir")
            elif op == "RENAME":
                namespace += 1; witnesses.add("rename")
                target = model.key(command["destination"])
                phase[target] = 5 if phase.pop(path, 0) == 4 else 0
                if path in imported_untouched:
                    imported_untouched.remove(path); imported_untouched.add(target)
                if path in remount_pending:
                    remount_pending.remove(path); remount_pending.add(target)
                if path in pending: pending[target] = pending.pop(path)
                if path in spans: spans[target] = spans.pop(path)
            elif op == "UNLINK":
                namespace += 1; witnesses.add("unlink"); removed.add(path)
                imported_untouched.discard(path)
                if phase.pop(path, 0) == 7: witnesses.add("mixed-safe-lifecycle")
                if path in pending: missed_checks += 1; pending.pop(path)
                if path in remount_pending:
                    missed_remount_checks += 1; remount_pending.remove(path)
            elif op == "REMOUNT":
                witnesses.add("remount")
                missed_remount_checks += len(remount_pending)
                remount_pending = set(model.files)
                phase = {p: 6 if value == 5 else value for p, value in phase.items()}
            if model.opened is not None:
                current_size = len(model.files[model.opened])
                sizes.add(current_size)
                if got["size"] == current_size and current_size in (65535, 65536):
                    witnesses.add("size-" + str(current_size))
        if op == "REMOUNT" and got["status"] == expected["status"] == -10:
            witnesses.add("busy-unmount")
        if op == "OPEN" and got["status"] == expected["status"] == -2:
            witnesses.add("missing-open")
        if op in ("CREATE", "CREATE_CONTENTS") and got["status"] == expected["status"] == -13:
            witnesses.add("exclusive-collision")
        if op == "RMDIR" and got["status"] == expected["status"] == -7:
            witnesses.add("nonempty-rmdir")
        if op == "RESIZE" and command["size"] and got["status"] == expected["status"] == -6:
            witnesses.add("unsupported-resize")
        if op in ("CHECK", "REPLACE") and got["status"] == expected["status"] == -16:
            witnesses.add("stale-refusal")
            stale_handle = True; rebound = False
        if op == "CHECK" and success and rebound:
            witnesses.add("rebind")
            stale_handle = rebound = False
        if mutation:
            content += 1
            if opened in remount_pending:
                missed_remount_checks += 1; remount_pending.remove(opened)
            imported_untouched.discard(opened)
            if opened in pending: missed_checks += 1
            pending[opened] = True; spans.pop(opened, None)
    required = set(GENERATED if campaign else BASE)
    if backend != "mgfs": required.add("gap")
    if not campaign and backend != "mgfs": required.add("ordinary-create")
    if initial_files(layout): required.add("imported-read")
    if layout == "preallocated-directory": required.add("preallocated-directory-persistence")
    if campaign:
        if BACKENDS[backend]["resize"] == "arbitrary": required |= {"resize-grow", "shrink"}
        else: required.add("unsupported-resize")
    missing = sorted(required - witnesses)
    if campaign == "extended" and content < 128:
        missing.append("at-least-128-content-mutations")
    if pending or missed_checks: missing.append("every-content-mutation-verified")
    if remount_pending or missed_remount_checks: missing.append("every-remount-file-verified")
    return dict(witnesses=sorted(witnesses), required=sorted(required), missing=missing,
        substantive_content_mutations=content, verified_content_mutations=verified,
        substantive_namespace_mutations=namespace, operation_frequencies=dict(frequencies),
        observed_status_counts=dict(statuses), distinct_write_offsets=len(offsets),
        distinct_write_ranges=len(ranges), write_offsets=sorted(offsets),
        write_lengths=sorted(lengths), resize_lengths=sorted(resize_lengths),
        boundary_sizes=sorted(sizes & set(BOUNDARIES)), missed_content_checks=missed_checks,
        missed_remount_checks=missed_remount_checks, unverified_remount_files=sorted(remount_pending))
