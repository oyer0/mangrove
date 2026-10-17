# Reed text editor

Reed is a modal, full-screen text editor for Mangrove. Run `reed` to edit a
new, unnamed buffer or `reed FILE` to open an existing file or prepare a new
buffer at that pathname. Changes to a missing file are not published until a
successful save. Reed uses the native terminal-control interface and exclusive
raw-key input while it owns the alternate screen.

## Editing

Reed has normal, insert, command, and search modes. In normal mode, `h`, `j`,
`k`, and `l` move; `i` enters insert mode; `o` opens a line below and `O` a
line above; `x` deletes the character under the cursor; `dd`, `dw`, and `db`
delete a line, forward word, or backward word. `u` undoes and `U` redoes.
`w` and `b` move by word; Alt-`w` and Alt-`b` select the opposite word
boundaries. `a` and `e` move to the line start and end; Alt-`a` and Alt-`e`
enter insert mode at those positions. Alt-`o` opens a line above. Arrow, page,
home, and end keys are also handled where applicable.

`/` opens the command prompt. `/s` saves, `/sq` saves and quits, `/q` asks
whether to save, `/saveas PATH` saves to a new path, `/open PATH` opens another
file, and `/find TEXT` searches for literal text. `;` starts forward search,
Shift-`;` starts backward search, and `n`/`N` repeat the active search. Save,
open, and quit prompts preserve edits when the requested action fails or is
cancelled. `reed --help` prints the command summary.

## Text and limits

Reed edits valid UTF-8 text. It rejects file contents containing NUL or
malformed UTF-8 and keeps byte offsets at Unicode scalar boundaries. The
renderer and cursor treat each scalar as one cell; movement is not
grapheme-aware and wide or combining characters do not receive terminal-width
layout. Reed preserves whether a file had a final newline.

Files are limited to 2 MiB and pathnames to 255 bytes. Undo and redo are
bounded to 64 snapshots and 8 MiB of snapshot data per stack. Search is literal
text, not a regular-expression engine.

## Safe saving

Reed opens existing files with a handle and records a coherent file snapshot.
Saving uses the native conditional-replacement operation against the opened
file generation. If another writer changed the file, Reed reports the stale
save and offers cancellation, Save As, or an explicit overwrite after it has
read a new complete version. The ordinary stale-save path never silently
replaces a newer file.

Save As uses exclusive create-with-content semantics; an existing destination
is left untouched and the edits remain in Reed. If an operation has an
uncertain outcome, Reed preserves the buffer and blocks retry to that same
existing target; Save As to another path or discarding edits remains available.
These are runtime failure and conflict guarantees, not crash atomicity,
power-loss durability, or post-reboot repair. See
[safe-file qualification](../development/guest-safe-files.md) and the
[storage lifecycle contract](../storage/lifecycle.md).

## Implementation

The editor and UTF-8 gap buffer are in `userspace/reed/`. Native snapshot,
replacement, creation, handle, and terminal contracts are documented in
[processes and syscalls](../architecture/processes-and-syscalls.md),
[terminal behavior](../architecture/terminal.md), and the relevant filesystem
pages. Host buffer and safe-save tests are listed in the
[test-suite inventory](../development/test-suite.md).
