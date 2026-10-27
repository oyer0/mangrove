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

## Terminal scrollback

When Shoot owns the normal console screen, `PgUp` and `PgDn` move through
retained output by one visible page. `Alt+PgUp` jumps to the oldest retained
output and `Alt+PgDn` returns to the live prompt. New output continues to be
recorded while viewing older rows; if output evicts the displayed oldest row,
the view advances to the oldest row still retained. The live cursor is hidden
while viewing history. The next ordinary editing key returns to the live
prompt before the key is processed. Fullscreen applications own their Page
keys while using the alternate screen.

The terminal retains at most 512 total rows in its cell ring, including the
currently visible rows. The additional history capacity is therefore at most
512 minus the visible row count. Logout clears the ring before the next login
prompt, so previous-session output is not available through scrollback.

`clear` and Ctrl+L clear the live viewport while keeping its former visible
rows as the newest scrollback page, then move the cursor to the top-left.
Repeated clears use the same ring capacity and can evict only the oldest
retained rows. A clear issued while already viewing history blanks that
displayed page without deleting it; the next scrollback navigation redraws
the retained page.

Shift+Arrow moves the cursor without selecting text. Shoot has no command-line
selection mode. Modifier combinations that have no binding are ignored rather
than inserted as printable text. Cooked byte readers, including login input,
retain their existing input path.

While Shoot waits for a foreground command, Ctrl+C requests cooperative
interruption of that command and returns to the prompt after its cleanup. This
is not POSIX signal or process-group support; blocking console, IPC, network,
and sleep operations are interruptible, while CPU-only work must reach an
interruptible operation. See the
[process interruption contract](../architecture/processes-and-syscalls.md#foreground-interruption).

`exit` leaves the current login session; `logout` is its no-status spelling.
Ctrl+D at an empty prompt has the same end-of-input effect. Ctrl+D on a
nonempty command still deletes the scalar under the cursor. When a session
ends, logind retires its remaining processes and clears the terminal before
showing the next `Username:` prompt. The reset clears the terminal's retained
cell ring as well as its visible framebuffer, so output from the previous
session is not available through the console after logout. An unexpected
shell exit is recorded by the logging service and follows the same reset path.

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
beginning with `//` are comments. Optional spaces and tabs around `=` are
accepted, but generated configuration and examples use the canonical form
without spaces. Duplicate or unknown keys, malformed assignments, invalid
values, and read failures reject the complete file. Shoot keeps every setting
at its built-in default instead of applying a valid prefix or preserving a
previously loaded subset. It prints the path and, for parser errors, the line
number; it continues to run. A missing optional file is normal. Run the
`reload` built-in to read the current file again.

```ini
prompt_color=green
completion_color=dark_gray
completion_case_sensitive=true
history_enabled=true
history_limit=256
history_persist=true
history_ignore_duplicates=true
history_ignore_space=true
```

The current file has no fixed small size limit; reading is dynamically
allocated and remains subject to available process memory. A failed read,
allocation, or close is reported and leaves defaults active. Shoot never
rewrites an invalid file. During a legacy migration, a valid old file remains
usable for the current run if an exclusive copy cannot be completed; the
warning names both paths and the legacy file is preserved.

History operations and the behavior when concurrent sessions change the saved
file are specified in [command history](history.md).
