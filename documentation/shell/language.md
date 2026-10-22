# Shoot command language

Shoot parses one command or assignment per input line. Spaces and tabs
separate words. Single and double quotes group text and are removed during
parsing. `$name` expands a shell variable outside single quotes; an unset
variable expands to an empty string. There is no backslash escape syntax.
An unquoted `//` starts a comment; inside quotes it is ordinary text.

Quoted and unquoted pieces can form one word. Empty quoted words are allowed.
The parser rejects unterminated quotes, excess arguments, and words that do
not fit its bounded command storage. It does not perform pathname globbing,
command substitution, field splitting, or general tilde expansion. `~` is
expanded only by path-aware features that opt into it: `cd`, script pathname,
output redirection, and the `exists` condition. A single-quoted path disables
that expansion.

## Assignments and conditions

An assignment uses `name = value`. Names begin with an ASCII letter or `_`;
subsequent characters may also be digits. Values are shell-local strings.
Shoot supports a bounded integer update form such as `count = $count + 1` or
`count = $count - 1`; operands are signed decimal integers and overflow is an
error. There are at most 64 variables, with names and values bounded to 31 and
255 bytes respectively.

Script blocks use `if`, optional `else`, `while`, and `end`. Conditions are one
of:

```text
if exists PATH
if LEFT == RIGHT
if INTEGER < INTEGER
if INTEGER <= INTEGER
if INTEGER > INTEGER
if INTEGER >= INTEGER
```

Equality compares expanded strings. Relational operators compare signed
decimal integers. `exists` tests whether pathname lookup succeeds. Boolean
operators and compound conditions are not supported. Blocks may be nested;
there is no `break` or `continue`, so a `while` condition must eventually
become false for a script to finish.

An interactive `if` or `while` begins a multiline block; Shoot reads through
the matching `end` before execution. A `.shoot` script is read completely,
checked for block structure, and then executed. Command syntax and conditions
are checked as their statements are reached, so an earlier command may have
completed before a later malformed statement stops the script. Scripts accept
at most 4096 source lines, 512 statements, 511 bytes per line, and 64 nested
blocks. Script failure, including invalid built-in arity or a nonzero child
status, stops execution and makes Shoot exit nonzero. There is no trap or
pipeline status model.

## Commands and redirection

Built-ins run in Shoot. The `exit` built-in exits with status zero when no
status is given. It accepts an optional signed 32-bit decimal status; malformed
or out-of-range values are reported and leave the shell running. Other
commands are resolved from `/bin` or by an absolute path, then started as
native child processes. Shoot waits for the child and treats a nonzero exit as
command failure. Parsed arguments are passed as separate entries through the
native process argument interface, preserving empty arguments and quote
characters that were ordinary text inside another quote style. They are not
inherited as an environment.

One output redirection may appear in a command:

```text
command arguments > PATH
command arguments >> PATH
```

`>` creates a missing file or truncates an existing regular file before
launch. `>>` creates a missing file or seeks an existing file to its end.
Only standard output is redirected. For an external command, Shoot passes the
target handle directly to child creation; the parent shell's output is not
changed. This preserves nested redirection when Shoot itself is running with
redirected output. The child holds its own reference, so Shoot can close its
handle after creation. A failed child creation closes the target handle and
does not run the command. Shoot does not provide input redirection, separate
standard error, pipes, or chained commands. A redirection target is opened by
the ordinary native filesystem interface; its failure prevents the command
from running.

## Implementation boundary

The parser and script interpreter are in `userspace/shoot/language.c` and
`script.c`; command launch and redirection are in `shell.c`. The process ABI
and argument limits are documented in
[processes and syscalls](../architecture/processes-and-syscalls.md).
