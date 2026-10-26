# Shoot command language

Shoot parses one command or assignment per input line. Spaces and tabs
separate words. Single and double quotes group text and are removed during
parsing. Outside quotes, backslash escapes the next character, including a
space, quote, dollar sign, or comment marker. A final unquoted backslash is a
syntax error. Backslashes inside single or double quotes are ordinary text.
`$name` expands a shell variable outside single quotes unless the dollar sign
is escaped; an unset variable expands to an empty string. An unquoted `//`
starts a comment; inside quotes or when escaped it is ordinary text.

Quoted and unquoted pieces can form one word. Empty quoted words are allowed.
The parser rejects unterminated quotes, a dangling escape, excess arguments,
and words that do not fit its bounded command storage. Escaping a leading `~`
also suppresses path-aware home expansion for that word. It does not perform
command substitution, field splitting, or general tilde expansion. `~` is
expanded only by path-aware features that opt into it: `jump`, script
pathname, output redirection, and the `exists` condition. A single-quoted path
disables that expansion.

## Pathname patterns

After parsing, each command argument is eligible for pathname expansion. The
command name and output-redirection target are not expanded. Unquoted `*`
matches zero or more Unicode scalar values within one pathname component;
unquoted `?` matches exactly one scalar. Bracket expressions support literal
members, scalar ranges, and leading `!` negation, for example `[abc]`, `[a-z]`,
and `[!abc]`. Matching is case-sensitive and does not use locale collation.
Malformed bracket expressions fail the command before dispatch.

Patterns may occur in any component of a relative or absolute path. Each
component is matched independently; `**` is just a same-component `*` and
does not recurse through directories. Results for each pattern are separate
arguments sorted by UTF-8 byte order (which preserves scalar order for valid
UTF-8). A pattern that finds no path remains one literal argument. Quoting or
backslash-escaping wildcard syntax makes it literal; wildcard characters
introduced by an unquoted variable expansion remain active. No field splitting
is performed, so a matched name containing spaces remains one argument.

As in familiar Unix shells, wildcard components do not match names beginning
with `.` unless the component itself begins with `.`. `*` and `.*` never
produce `.` or `..`. Expansion streams directory enumeration and grows its
result vector geometrically, retaining only final matching names. Expanded
arguments, including their pointers, terminators and executable name, must
fit the native 8 MiB packed argument budget and available memory. There is no
separate fifteen-result limit. Each complete pathname is limited to 65,535
UTF-8 bytes. FAT32 and exFAT components may use up to 765 bytes; MGFS retains
its separate 255-byte on-disk component limit. Budget, allocation, pattern,
and enumeration failures abort before a
builtin starts, a child launches, or an output target is opened. A matched
entry disappearing during lookup also fails rather than reverting to a
literal wildcard pathname.

`del *` passes the non-hidden matched operands to one `del` process. Regular
files and empty directories can be removed without recursion; `del -r *`
explicitly enables tree removal. Hidden top-level names remain excluded.
Preparation is all-or-nothing, but deletion after dispatch is a sequence of
filesystem operations: a permission, media, or nonempty-directory error can
leave earlier operands deleted and returns overall failure. Use `del ./*`
when names beginning with `-` should be passed as paths rather than command
options. No implicit batching into multiple children is performed.

## Assignments and conditions

An assignment uses `name = value`. Names begin with an ASCII letter or `_`;
subsequent characters may also be digits. Values are shell-local strings.
Shoot supports a bounded integer update form such as `count = $count + 1` or
`count = $count - 1`; operands are signed decimal integers and overflow is an
error. Variable names and values are dynamically allocated; there is no fixed
variable-count, name-length, or value-length table limit. Growth is checked and
an allocation failure leaves the prior variable value intact. Practical
capacity is bounded by Shoot's available address space and heap resources.

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
as many source lines, statements, and bytes per line as can be retained by
Shoot's available memory. These buffers grow dynamically with checked size
arithmetic. Allocation failure aborts script loading or statement insertion
without executing a partially loaded file. Block nesting remains limited to
64 levels to bound the recursive evaluator's call stack. The interactive
multiline editor still limits each entered line to 511 bytes; file-backed
scripts are not subject to that input-line limit. Script failure, including
invalid built-in arity or a nonzero child status, stops execution and makes
Shoot exit nonzero. There is no trap or pipeline status model.

## Commands and redirection

Built-ins run in Shoot. The `exit` built-in exits with status zero when no
status is given. It accepts an optional signed 32-bit decimal status; malformed
or out-of-range values are reported and leave the shell running. `logout` is
the no-status spelling of `exit`. In an interactive login, either ends the
session and returns to the cleared login screen. Other
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
