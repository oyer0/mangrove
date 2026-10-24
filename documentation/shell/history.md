# Shoot command history

Interactive Shoot sessions keep an independent, bounded in-memory history.
Normal command execution performs no history-file writes. Startup reads the
current user's `~/.state/shoot/history` when persistence is enabled; normal
exit appends only retained, pending session entries. The state directory is
created only when a history write is needed. An orderly system shutdown or
reboot asks Shoot to take the same exit path before the kernel synchronizes
mounted filesystems, so pending history is saved without an explicit `exit`
command. If Shoot must be forcibly terminated after its bounded cleanup window,
its in-memory pending entries cannot be saved.

On startup, Shoot reads the new history file when present. A legacy
`~/.shoot/history` file is also read into memory without modifying either path.
If it is the only saved file, the first history write attempts to move it to
the new path without replacement, then appends under the existing serialized
history operation. If both files exist, entries from the legacy file are
loaded before new-path entries; writes go only to the new file and leave the
legacy file intact. The ordinary bounded history limit still applies in
memory. A move or append failure retains pending entries and preserves
uncertain-mutation handling; an uncertain move blocks automatic append. Missing
or unreadable legacy paths are never treated as empty after an I/O error.

## Commands

| Command | Effect |
| --- | --- |
| `history` | Display current memory, with one-based entry numbers. |
| `history -c` | Clear memory and its pending entries; leave the saved file alone. |
| `history -d N` | Delete displayed entry N from memory, including its pending status. |
| `history -a` | Append retained pending entries to the latest saved contents. |
| `history -n` | Import only reliably new saved entries since the comparison baseline. |
| `history -r` | Append all saved entries to memory, allowing duplicates. |
| `history -w` | Replace saved contents with current memory, including an empty history. |

History commands themselves are recorded before execution, subject to ordinary
recording settings. Thus `-c` removes its own entry; `-a` may save its own entry;
and `-w` may include itself. Explicit file commands work even when automatic
persistence is disabled. Disabling history recording stops automatic recording;
it does not prevent explicit reads or inspection of retained entries.

The default limit is 256 entries and the maximum is 4096. Oldest entries are
removed when memory exceeds the limit, including pending entries. Deletion,
clearing, and limit changes affect memory without immediately changing the file.
Successful append/overwrite operations bound the file by the writer's configured
limit. A session with no pending entries does not trim another session's file on
exit. Consecutive-duplicate and leading-space settings apply to commands recorded
in this session; explicit imports allow duplicates.

## Pending entries and imports

Imported entries are never pending. Pending state belongs to each retained
entry, so importing saved commands after a local command does not cause that
local command to be skipped or cause imported commands to be saved again.
Successful `-a` clears only the pending markers it saved. Successful `-w`
clears all retained pending markers. Failed operations retain them.

`-n` uses a bounded, locked snapshot with a file identifier and MGFS revision.
It imports a suffix only when the previous snapshot remains an unchanged prefix
of the current file and both snapshots have complete record boundaries. Known
positions of this session's successful writes prevent replaying its own saved
entries while they are still retained in memory.

Truncation, identifier changes, changed prefixes, same-size revision changes,
and incomplete/invalid records are ambiguous. `-n` warns and imports nothing;
it refreshes its comparison baseline. A later strict append to a valid baseline
can be imported. `-r` explicitly imports the whole file; a legacy unterminated
final line is accepted by `-r` and startup, but is not an append baseline.

Textual history has no persistent per-command identifiers. Repeated command
text is legitimate, and file revision/identity information cannot reconstruct
arbitrary intervening edits. Prefix-preserving rewrites are indistinguishable
from appends at the content level. Identifier reuse across filesystem changes
is also outside this session-local contract. `-n` is conservative, not a
cross-session event log; it never uses a full-file import as a fallback.

## Concurrency and failures

MGFS appends read the latest persistent Record, merge additions, trim the
oldest records, and write under one superblock operation lock. Snapshots hold
that same lock for their complete read. Different sessions' normal appends do
not overwrite each other from stale snapshots. A smaller writer limit can
remove older entries; active sessions do not automatically import other saves.
The filesystem's private history-content update explicitly sets the merged
length; ordinary positional writes preserve trailing contents and do not
perform this trimming. The history update remains an in-place, quarantined
operation rather than the native staged conditional-save interface.

Explicit `-w` is intentionally destructive to the saved file: it can overwrite
entries saved by another session before it acquires the lock. `-c` and `-d`
never remove saved entries. Later normal appends merge with the latest contents.

These operations are serialized, not crash-atomic transactions. MGFS writes may
change data or commit the new Record before reclamation or state cleaning fails.
Once a mutating callback reports an uncertain outcome, Shoot keeps entries in
memory and blocks manual `-a` and automatic exit append. Clearing, deleting, or
reading history does not clear that uncertainty. An explicit successful `-w`
resolves it. Internal write failures are classified conservatively; an append
can be blocked even if that particular failed write changed no file data.

Known pre-write failures, such as opening the file or allocating merge buffers,
retain pending entries without setting uncertainty. No implicit destroy-time
retry is performed. Exit reports saving failures, but process exit necessarily
loses any remaining in-memory entries. Crashes and forced termination also lose
unsaved entries. A failed `-w` can itself leave an uncertain/partially changed
file; neither `-w` nor snapshots guarantee power-failure recovery.

## Verification

`gmake test-history` runs separate Shoot-state tests with mocked persistence,
VFS/MGFS merge and snapshot tests with mocked reads/writes, and production MGFS
write/truncate control flow against fault-injected block and allocation fixtures.
The latter verifies failures after Record mutation and allocation cleanup; it
is not a real-disk crash-recovery test. Guest QEMU checks cover the actual syscall,
MGFS and interactive-shell path. See [test coverage](../development/tests.md).
