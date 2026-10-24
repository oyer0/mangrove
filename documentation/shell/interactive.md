# Interactive Shoot

With no argument, `shoot` starts the interactive shell. `shoot --help` prints
its invocation form. `shoot FILE.shoot` runs one script and exits with status
zero on success or nonzero on a read, parse, or command failure.

The prompt shows the current username and working directory; a home-directory
path is abbreviated as `~`. `jump` changes the process working-directory
binding. A removed working directory does not silently bind to another volume
at the same pathname; use an absolute path to recover. See
[processes and syscalls](../architecture/processes-and-syscalls.md#working-directory-identity-and-path-resolution)
for the VFS identity rules.

## Line editing and completion

Shoot uses loam's native line editor over standard output and the
application-facing key-event input mode. Its nonmodal keymap is:

| Key | Behavior |
| --- | --- |
| Left / Right | Move one UTF-8 scalar. |
| Up / Down | Move through history and restore the unfinished draft. |
| Alt+Left / Alt+Right | Move to the previous / next word. |
| Home / End | Move to the beginning / end of the command. |
| Backspace / Delete | Delete the previous / next UTF-8 scalar. |
| Alt+Backspace / Alt+Delete | Delete to the previous / next word boundary. |
| Tab | Complete a built-in or installed command at command position, or a filesystem path elsewhere. |
| Enter | Submit the command. |
| Ctrl+C | Discard the current input and start a fresh prompt. In an incomplete interactive block, discard the block as well. |
| Ctrl+D | Return end-of-file when the input is empty; otherwise delete the scalar under the cursor, if any. |
| Ctrl+L | Clear the visible terminal and redraw the prompt and current input. |

Shift+Arrow moves the cursor without selecting text. Shoot has no command-line
selection mode. Modifier combinations that have no binding are ignored rather
than inserted as printable text. Cooked byte readers, including login input,
retain their existing input path.

Shoot's word model groups consecutive ASCII letters, digits, and underscore
with any valid non-ASCII Unicode scalar. ASCII punctuation, spaces, and tabs
are separators. Alt+Left skips separators and moves to the start of the
preceding word. If the cursor is strictly inside a word, it skips that whole
word before moving to the preceding one; if there is no preceding word, it
stops at the containing word's start. At the start of a word it moves to the
preceding word, while at the end of a word it moves to that word's start.
Alt+Right skips separators and moves to the end of the next word. If the cursor
is strictly inside a word, it skips that whole word before moving to the next
one; if there is no next word, it stops at the containing word's end. From a
word's start it moves to that word's end. From a separator it skips the
separators; if no word exists in that direction, it moves to the corresponding
command boundary. The corresponding word deletions remove from the cursor to
the same destination as their movement. This is scalar-safe but is not Unicode
word segmentation.

Tab completion uses an overlay that is temporary display state; it is not
written into terminal scrollback.

The line editor accepts valid UTF-8 key-event text and drops malformed cooked
byte sequences. Its display model counts one terminal cell per Unicode scalar.
It does not provide grapheme-cluster editing or East Asian width-aware layout. See
[terminal behavior](../architecture/terminal.md) for renderer limits.

## Configuration

Shoot optionally reads `~/.config/shoot/config`. If it is absent, built-in
defaults are used and no configuration directory or file is created. On first
use after upgrading, if only `~/.shoot/config` exists, Shoot reads it and
attempts an exclusive copy to the new path. The old file is retained. If both
paths exist, the new path takes precedence and the old file is left untouched.
If the copy cannot be completed, valid settings are still used from the old
file for that run and Shoot reports the migration failure; it never truncates
either file. The currently recognized keys are:

| Key | Default | Meaning |
| --- | --- | --- |
| `prompt_color` | `green` | Prompt foreground palette entry. |
| `completion_color` | `dark_gray` | Completion text palette entry. |
| `completion_case_sensitive` | `true` | Command/path completion matching. |
| `history_enabled` | `true` | Record and navigate session history. |
| `history_limit` | `256` | In-memory entries, from 1 through 4096. |
| `history_persist` | `true` | Load and save the user's history file. |
| `history_ignore_duplicates` | `true` | Ignore adjacent repeated commands. |
| `history_ignore_space` | `true` | Do not record commands beginning with a space. |

Values are written as `key=value`; booleans are `true` or `false`. Lines
beginning with `//` are comments. Unknown keys are ignored. A malformed line
or invalid value for a recognized key makes reload fail without replacing the
previous settings. Run the `reload` built-in to apply a valid edited
configuration.

History operations and the behavior when concurrent sessions change the saved
file are specified in [command history](history.md).
