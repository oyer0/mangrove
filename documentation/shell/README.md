# Shoot shell

Shoot is Mangrove's interactive command shell and script runner. It reads the
current identity and working directory, edits one command line at a time, and
executes shell built-ins in the Shoot process. Other commands are loaded from
`/bin` as native programs. Shoot waits for each child and reports launch,
crash, wait, and nonzero-exit failures.

Shoot has a small command language of its own. It is not Bash or a POSIX shell:
there are no pipelines, background jobs, command chaining, globbing, or
environment-variable vector. See [language](language.md) for its grammar and
[interactive use](interactive.md) for editing and configuration.

Use `help`, `help files`, `help network`, or `help system` in Shoot for the
installed command descriptions. `help` lists only commands implemented inside
Shoot. Standalone command quick-help is provided by each command's `-h` or
`--help` option and is backed by records in `/share/help`. The
[native command reference](../userspace/commands.md) groups commands by use.

Built-ins are `jump`, `exit`, `help`, `history`, and `reload`. Standalone
utilities with the same names are not implicitly treated as built-ins. Bare
external command names resolve under `/bin`; an absolute executable path may
also be given.

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

Command lines are limited to 511 input bytes, with at most eight arguments
and 512 bytes of parsed word storage. Script files are bounded; the exact
limits are described in [language](language.md).
