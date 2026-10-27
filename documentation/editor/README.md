# Reed text editor

Reed is a modal, full-screen editor for UTF-8 text files. Run `reed` for a new
buffer or `reed FILE` to open or create a file. Reed uses the alternate screen
while editing and restores the terminal when it exits.

## Modes

| Mode | Purpose |
| --- | --- |
| Normal | Navigate, edit, search, and run file commands. |
| Insert | Enter or remove text. |
| Selection | Select text or lines for editing and clipboard operations. |
| Command | Enter a save, open, quit, or search command. |
| Search | Review matches and move between them. |

## Normal mode

Each key below is listed separately; equivalent keys have the same action.

| Key | Action |
| --- | --- |
| `h` | Move left one character. |
| Left | Move left one character. |
| `l` | Move right one character. |
| Right | Move right one character. |
| `j` | Move down one visual row when wrapping, otherwise one line. |
| Down | Move down one visual row when wrapping, otherwise one line. |
| `k` | Move up one visual row when wrapping, otherwise one line. |
| Up | Move up one visual row when wrapping, otherwise one line. |
| Page Up | Move up one viewport. |
| Page Down | Move down one viewport. |
| `a` | Move to the start of the current line. |
| Home | Move to the start of the current line. |
| `e` | Move to the end of the current line. |
| End | Move to the end of the current line. |
| `A` | Move to the start of the current line and enter Insert mode. |
| `E` | Move to the end of the current line and enter Insert mode. |
| `g` | Move to the start of the document. |
| Ctrl+Home | Move to the start of the document. |
| `G` | Move to the end of the document. |
| Ctrl+End | Move to the end of the document. |
| `b` | Move backward by word. |
| `B` | Move to the start of the current or previous word. |
| `n` | Move forward by word. |
| `N` | Move to the start of the next word. |
| `[` | Move to the previous paragraph. |
| `]` | Move to the next paragraph. |
| `i` | Enter Insert mode at the cursor. |
| `o` | Open a line below and enter Insert mode. |
| Shift+`O` | Open a line above and enter Insert mode. |
| `d` | Delete the character at the cursor. |
| Backspace | Delete the character before the cursor. |
| Delete | Delete the character at the cursor. |
| `u` | Undo. |
| Shift+`U` | Redo. |
| `s` | Start character selection. |
| Shift+`S` | Start line selection. |
| Alt+`S` | Select the whole document. |
| `c` | Start a copy motion; `cc` copies the current line. |
| `x` | Start a cut motion; `xx` cuts the current line. |
| `v` | Paste clipboard text at the cursor. |
| `/` | Open the command prompt. |
| `?` | Open incremental search. |

## Insert mode

Type text to insert it. `Enter` inserts a newline. Left, Right, Up, Down,
Home, End, Backspace, and Delete move or edit text. `Escape` returns to Normal
mode. Shift+Arrow does not select text.

## Selection mode

Navigation extends the selection. `s` selects characters and Shift+`S`
selects complete lines. Pressing the active selection key cancels selection;
pressing the other changes its granularity. `Escape` cancels selection.

| Key | Action |
| --- | --- |
| `d` | Delete the selection. |
| `c` | Copy the selection to the session clipboard. |
| `x` | Copy the selection, then delete it. |
| `v` | Replace the selection with clipboard text. |

Clipboard operations publish or read complete UTF-8 text. If a copy, cut, or
paste fails, Reed leaves the document unchanged. Successful edits can be
undone. Clipboard contents are shared within the session, survive the source
editor exiting, and are cleared when the session ends or the clipboard service
restarts. Ordinary edits and undo/redo do not replace the clipboard.

## Search

In the search prompt, type or edit a literal query. Matches are highlighted as
the query changes. `Enter` accepts the query and selects the nearest match; if
there is no match, Reed returns to Normal mode at the starting cursor.
`Escape` cancels and restores the starting cursor.

In Search mode, `n` moves to the next match and `b` to the previous match.
Neither wraps at document boundaries. `?` or `Escape` exits Search mode at the
current cursor. Ordinary cursor movement remains available; editing is inactive
until Search mode ends. Uppercase `N` and `B` retain their Normal-mode word
navigation meanings and are not search-navigation keys.

## Commands

Enter `/` followed by a command and press `Enter`. Command names are
case-insensitive and must match exactly. Tab completes an unambiguous command
name, not a path.

| Command | Action |
| --- | --- |
| `/s` | Save the current file. |
| `/q` | Quit, prompting if there are unsaved changes. |
| `/sq` | Save and quit only after a successful save. |
| `/saveas PATH` | Save to a new path without replacing an existing file. |
| `/open PATH` | Open another file after resolving unsaved changes. |
| `/find TEXT` | Find literal text forward, wrapping to the document start. |

## Configuration

Reed optionally reads `~/.config/reed/config` at startup. It does not create
the file or reload settings during an editing session. Defaults are:

```ini
wrap=true
tab_width=4
```

`wrap` accepts `true` or `false`; `tab_width` accepts integers from 1 through
16. Whitespace around `=` is accepted. Blank lines and `#`, `;`, or `//`
comments are allowed. Unknown or duplicate keys, malformed values, and read
errors reject the whole file; Reed keeps defaults and reports the path and
error in its status row. Parse warnings include a line number.

## Files and limitations

Reed accepts valid UTF-8 text without NUL bytes and preserves whether the file
ends in a newline. Each Unicode scalar occupies one terminal cell; wide
characters, combining-mark composition, and grapheme editing are not
supported. Soft wrapping is enabled by default. Set `wrap=false` to scroll
horizontally instead. Tabs display at the configured tab stops but remain tab
bytes in the file.

There is no fixed 2 MiB document limit. Loading, editing, undo/redo, and saving
use available process memory, and very large single-line edits and literal
searches can require linear work.

Saving an existing file detects changes made since it was opened. Reed does
not silently overwrite a conflicting version. Failed or uncertain saves keep
the edits in the buffer; Save As can target another path. These safeguards do
not promise power-loss durability.

See the installed [Reed manual](../../share/info/reed) and the
[terminal behavior](../architecture/terminal.md) reference.
