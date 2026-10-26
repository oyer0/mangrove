# Shoot shell

Shoot is Mangrove's interactive command shell and script runner. It reads the
current identity and working directory, edits one command line at a time, and
executes shell built-ins in the Shoot process. Other commands are loaded from
`/bin` as native programs. Shoot reports launch, crash, and wait failures;
ordinary nonzero child status remains a command failure without a duplicate
generic diagnostic.

Shoot has a small command language of its own. It is not Bash or a POSIX shell:
there are no pipelines, background jobs, command chaining, field splitting,
or environment-variable vector. Quoted and escaped wildcard characters are
literal; unquoted argument patterns expand against directory entries. See
[language](language.md) for the grammar and
[interactive use](interactive.md) for editing and configuration.

Use `help` in Shoot to list commands implemented inside the shell. Standalone
commands provide concise quick-help through `-h` or `--help`, backed by
records in `/share/help`. Use `info PROGRAM` for a fullscreen application
manual installed under `/share/info/`. The
[native command reference](../userspace/commands.md) groups commands by use.

Built-ins are `jump`, `exit`, `logout`, `help`, `history`, and `reload`.
Standalone utilities with the same names are not implicitly treated as
built-ins. Bare external command names resolve under `/bin`; an absolute
executable path may also be given.

Shoot keeps command history in memory during a session and, when enabled,
loads and saves it under the current user's home directory. Its persistence
and conflict behavior is specified separately in [command history](history.md).

## Implementation

- `userspace/shoot/` contains the parser, shell, built-ins, scripts, history,
  configuration, and completion.
- `libc/src/line_editor.c` implements the reusable native single-line editor.
- `userspace/common/help.c` reads the installed help records.
- [Terminal behavior](../architecture/terminal.md) describes the native
  console and terminal-control interface.

Interactive prompt input retains its 511-byte line-editor bound. The command
parser and its word storage grow dynamically, including for script statements.
Pathname expansion grows its vector independently of the parsed input;
hundreds or thousands of matches are supported within the native process
argument budget and available memory. Script limits and expansion failure
behavior are described in [language](language.md).
