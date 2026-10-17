# Interactive Shoot

With no argument, `shoot` starts the interactive shell. `shoot --help` prints
its invocation form. `shoot FILE.shoot` runs one script and exits with status
zero on success or nonzero on a read, parse, or command failure.

The prompt shows the current username and working directory; a home-directory
path is abbreviated as `~`. `cd` changes the process working-directory
binding. A removed working directory does not silently bind to another volume
at the same pathname; use an absolute path to recover. See
[processes and syscalls](../architecture/processes-and-syscalls.md#working-directory-identity-and-path-resolution)
for the VFS identity rules.

## Line editing and completion

Shoot uses loam's native line editor over standard input and output. It
supports UTF-8 scalar-boundary insertion and deletion, left/right and
word-oriented movement, Home/End, selection, deletion, and in-memory history
navigation. Tab completes built-in and installed command names at the command
position, or filesystem paths elsewhere. Completion uses an overlay that is
temporary display state; it is not written into terminal scrollback.

The line editor accepts strict UTF-8 sequences and drops malformed input. Its
display model counts one terminal cell per Unicode scalar. It does not provide
grapheme-cluster editing or East Asian width-aware layout. See
[terminal behavior](../architecture/terminal.md) for renderer limits.

## Configuration

Shoot reads `~/.shoot/config`, creating a default file and directory when
permitted. The currently recognized keys are:

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
