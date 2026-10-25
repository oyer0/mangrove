# Reed text editor

Reed is a modal, full-screen text editor for Mangrove. Run `reed` to edit a
new, unnamed buffer or `reed FILE` to open an existing file or prepare a new
buffer at that pathname. Changes to a missing file are not published until a
successful save. Reed uses the native terminal-control interface and exclusive
raw-key input while it owns the alternate screen.

## Editing

Reed has Normal, Insert, Selection, Command, and Search modes. Cursor positions
and edits use UTF-8 scalar boundaries. The cursor is an insertion boundary, so
line and document end motions can sit immediately after the last scalar.

### Normal mode

| Key | Action |
| --- | --- |
| `h` / Left, `l` / Right | Move one Unicode scalar left or right. |
| `j` / Down, `k` / Up | With wrapping enabled, move one visual row; with wrapping disabled, move one logical line. Retain the preferred screen-cell column. |
| `b` | Move to the final scalar of the previous word. |
| `B` | Move to the current word's beginning; when already there, move to the previous word's beginning. |
| `n` | Move to the final scalar of the current or next word. |
| `N` | Skip the word under the cursor and move to the next word start. |
| `a` / Home, `e` / End | Move to the beginning or end boundary of the logical line. |
| `A` / `E` | Move to the line beginning or end boundary and enter Insert mode. |
| `g` / Ctrl+Home, `G` / Ctrl+End | Move to the beginning or end boundary of the document. |
| `[` / `]` | Move to the previous or next paragraph as defined below. |
| Page Up / Page Down | Move by one visible text viewport, preserving the preferred screen-cell column. |
| `i` | Enter Insert mode at the cursor. |
| `o` | Insert a blank logical line below the current line and enter Insert mode on it. |
| `O` (Shift+`O`) | Insert a blank logical line above the current line and enter Insert mode on it. |
| `s` / `S` / Alt+`S` | Begin character selection, line selection, or select the whole document. |
| `d` / Backspace / Delete | Delete the scalar at the cursor, before it, or at it, respectively. |
| `u` / Shift+`U` | Undo / redo. |
| `/` / `?` | Enter the command prompt / incremental search prompt. |

For Stage 5, words are runs of ASCII letters, digits, underscore, and valid
non-ASCII scalars. ASCII punctuation, spaces, tabs, carriage returns, and line
breaks separate words. This is scalar-safe but is not Unicode word
segmentation. `b` and `n` place the cursor at the start of the final scalar of
their target word, so the target character is inclusive. `n` from before that
final scalar reaches it; when already on it, at the word's exclusive end, or
on a separator, `n` advances to the next word's final scalar. If there is no
next word, it stays put. `b` skips the word containing the cursor, including
one whose exclusive end is exactly at the cursor, and reaches the preceding
word's final scalar. If there is no preceding word, it stays put. `N` skips
the containing word and moves to the next word start; if no next word exists,
it stays put. `B` from inside a word reaches its beginning; pressing `B` again
there moves to the previous word's beginning. From a separator it reaches the
preceding word's beginning. If there is no previous word, it stays put. No
word motion wraps at document edges.

In Selection mode, inclusive `b`/`n` destinations include the target scalar in
the highlighted range. Edit ranges remain half-open byte ranges; a
clipboard operator using a forward inclusive `n` destination must extend the
high endpoint through that scalar with Reed's UTF-8 next-boundary helper.
Backward `b` already places the target at the low endpoint of the half-open
range. `B`/`N` remain word-start motions. Clipboard-dependent operators are
not implemented.

Paragraphs are runs of nonblank logical lines. Empty lines and lines containing
only ASCII spaces, tabs, or carriage returns are blank separators. `[` from
inside a paragraph moves to that paragraph's first line; when already at that
line's beginning, or when on a separator, it moves to the previous nonblank
paragraph's first line. `]` moves from a paragraph to the following
nonblank paragraph's first line, or from a separator to the next one. At the
document boundary, either motion stays put.

`{` and `}` are reserved for future delimiter-aware structural navigation and
currently have no action.

### Selection mode

`s` starts a character selection and `S` starts a line selection. Navigation
keys extend the selection from its original anchor. Pressing the active `s` or
`S` cancels selection; pressing the other switches character/line granularity
without changing the anchor or endpoint. Escape also cancels and keeps the
current endpoint. Alt+`S` selects the complete document as a character range.
Line selection includes complete logical lines and their separating newline
where present; selecting through the final line consumes the preceding newline
when needed. It never invents a file-final newline.

Selection is visibly highlighted. `d` deletes it as one undoable edit and
returns to Normal mode. `i` does not enter Insert mode from Selection mode.
Backspace and Delete do not act on a selection. Reed uses the
[session clipboard](../architecture/clipboard.md), with no private clipboard or yank register. In Normal mode, `c` or `x` followed by
`b/B/n/N`, `a/e`, `g/G`, or `[`/`]` copies or cuts a motion range without
first moving the document cursor. Escape cancels a pending operator; an
unsupported suffix cancels with feedback. A motion operator's range is
the half-open byte range between the starting cursor boundary and the motion
destination, using the same scalar-safe motion rules. `cc` / `xx` operate on
the complete logical line and its separator under the line-selection newline
rule. In Selection mode, `c` / `x` publish the selected range, and `v`
replaces the selection only after the complete clipboard value is available.
Clipboard failures leave text, cursor, selection and undo/redo unchanged.
Cut prepares its undo delta before publication and deletes only after the
service confirms publication; paste obtains a complete snapshot and prepares
one range edit before modifying the buffer. Successful selection copy/cut/paste
returns to Normal mode. Cut and paste each form one undoable edit. `v` in Normal
mode inserts at the current insertion boundary. Empty text is valid: it
replaces a selection with nothing, or is a no-op without a selection. Reed
rejects NUL-bearing clipboard data, matching its file-loading contract.
Clipboard insertion is limited by checked address arithmetic and available
editor/history memory. A failed paste leaves the document unchanged, without
truncation. Clipboard
content survives editor exit within the same session and is lost on logout or
service restart. Ordinary deletion, insertion and undo/redo never publish it.

### Insert, commands, and search

Insert mode accepts valid text and supports arrows, Home/End, Backspace, and
Delete. Shift+Arrow does not select text. Enter inserts a newline. `o` and
Shift+`O` create a blank line below or above the current logical line and enter
Insert mode on it. They work at document boundaries, in an empty buffer, and
when the final line has no trailing newline. Opening a blank line at EOF keeps
that line distinct from the file's final-newline marker when saved; Undo
restores the previous marker. Alt+`O` has no binding.

`/` opens a command prompt. The first token ends at ASCII space or tab and is
matched case-insensitively; command names must match exactly. Tab completes an
unambiguous command name, but does not complete paths. Existing commands are:

- `/s` saves.
- `/q` quits with unsaved-change protection.
- `/sq` quits only after a successful save.
- `/saveas PATH` saves to a new path.
- `/open PATH` opens another file.
- `/find TEXT` performs the existing literal forward search from the cursor,
  wrapping to the start when needed.

Command errors are shown in the status row. Save, open, and quit prompts
preserve edits when the requested action fails or is cancelled. `Alt+S` is
Select All, not Save; save and quit are available through the command prompt.

`?` opens an incremental literal search prompt. Every match is highlighted as
the query changes, while the original cursor remains the search anchor. Enter
selects the match nearest that anchor by byte position (ties choose the later
match), then places the cursor immediately after it and enters Search mode. If
there is no match, Reed returns to Normal mode at the anchor. Escape cancels
and restores the anchor. In Search mode, ordinary movement remains available;
`n`/`N` move to the next match and `b`/`B` to the previous match. These motions
use the current cursor, do not wrap, and leave it unchanged when no match
exists. Uppercase `N` and `B` do not change the active search match; they keep
their normal-mode word-motion meanings. `?` or Escape exits Search mode at the
current cursor and removes highlights. Editing keys are inactive until Search
mode is exited.

`reed --help` prints the command summary.

## Text and limits

Reed edits valid UTF-8 text. It rejects file contents containing NUL or
malformed UTF-8 and keeps byte offsets at Unicode scalar boundaries. The
renderer and cursor treat each scalar as one cell; movement is not
grapheme-aware and wide or combining characters do not receive terminal-width
layout. Reed preserves whether a file had a final newline.

Soft wrapping is enabled by default. A long logical line continues on later
visual rows at character-cell boundaries; wrapping does not insert bytes or
change saved file contents. In wrapped mode, `j`/`k`, arrows, and Page Up/Down
move through visual rows, while `a`/`e`, Home/End, and paragraph motions keep
their logical-line meanings. Tabs expand to the next configured tab stop.
Tabs remain literal tab bytes in the buffer; expansion affects display only.
Because the current renderer assigns one cell to each Unicode scalar, tabs and
UTF-8 layout do not yet account for grapheme clusters, combining marks, or
wide-script display widths.

When a logical line ends exactly at the right edge, its end insertion boundary
is drawn on the final occupied cell, matching the terminal's pending-wrap
cursor rule. The buffer cursor remains after the final scalar.

Horizontal scrolling is available by setting `wrap = false`. It uses one
document-wide horizontal offset: every logical line is clipped against the
same left edge, including shorter neighboring lines. The status row and Reed's
prompts remain fixed while the document viewport moves.

### Configuration

Reed reads `~/.config/reed/config` once at startup, using the home directory
from the current process identity. Settings are not reloaded during an editing
session. The file is optional; when absent, Reed uses soft wrapping and a tab
width of four cells:

```ini
wrap = true
tab_width = 4
```

Blank lines and whitespace around keys and values are allowed. Lines may use
`#`, `;`, or `//` comments. `wrap` accepts `true` or `false`; `tab_width` must
be an integer from 1 through 16. Unknown or duplicate keys, malformed values,
overlong lines, and files over 1023 bytes are rejected as a whole. Reed keeps
the built-in defaults and displays a status warning rather than applying a
partial configuration. It reads but never creates or rewrites the file. To
configure Reed, create the parent directories and file yourself; configuration
write policy is not part of this editor interface.

Documents have no fixed 2 MiB ceiling. Native pathnames allow up to 65,535
UTF-8 bytes before NUL; each FAT32/exFAT component may use up to 765 UTF-8
bytes (255 UTF-16 code units). MGFS retains its independent 255-byte on-disk
component limit.
Text, a saved baseline, layout indexes and any serialized save must fit available
memory; Reed is an in-memory editor. UTF-8 and NUL validation still apply.

Undo/redo retain grouped splice deltas (removed and inserted bytes), not complete
document snapshots. History arrays and deltas grow dynamically; there is no
fixed group-count or byte-retention ceiling. The process remains subject to
available address space and allocation. A failed edit or history replay leaves
the document and the history group being applied unchanged. A new edit retires
the redo branch; closing or replacing the editing buffer releases its history.
Reed does not silently discard older undo groups to meet a hidden quota. Every
edit reserves its delta and gap/index capacity before changing text. Clipboard
cut reserves before publication; commit allocates nothing. Undo/redo preflight
their destination history and working capacity before replay. Dirty checks
compare the two gap spans against the saved baseline without a complete
temporary copy.

A newline index supports direct logical-line lookup. Cached cell widths,
visual-row prefixes and sparse scalar-safe checkpoints avoid rescanning earlier
text for every movement or wrapped-row repaint. Within-line edits reflow that
line and shift later offsets. Newline edits, grouped replay and configuration
changes can rebuild layout linearly; cache allocation failure uses a slower
streaming fallback. Very long single-line edits and absent literal searches
still require linear work. Search is literal, not a regular-expression engine.
Loading retries a growing-file snapshot at most four times; repeated growth
returns an error while preserving the previous buffer. Saving reuses its
serialized allocation as the new saved baseline after confirmed success.
Kernel input staging remains a separate immutable copy; format, free-space,
ownership-proof and memory limits can reject an otherwise valid large save.
See [safe-file resource rules](../storage/lifecycle.md).

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
