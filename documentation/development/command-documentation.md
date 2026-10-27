# Command documentation style

First-party command help and manuals are user-facing references. Keep their
content aligned with the installed implementation and `/share/help` records.

## Quick help

Every standalone command that accepts quick help should use the shared renderer
and its installed help record. The renderer prints the command name and a
one-sentence purpose, followed by `Usage:` and nonempty sections only. Use
`Options:` for flags accepted when launching the command, `Keybindings:` for
controls handled while an interactive application is open, `Commands:` for
application-internal commands, and `Notes:` for important constraints. Keep
entries concise, use consistent capitalization and punctuation, and explain
what an option does. Do not put examples, interactive controls, or shell
language commands under `Options:`. Do not add empty sections or meaningless
options for uniformity. `-h` and `--help` should have the same behavior where
both forms are supported.

## Full manuals

Install a plain-text manual at `/share/info/PROGRAM` for each first-party
command with an installed quick-help record. Use uppercase section headings
with a blank line before the section body, in this order when applicable:

1. `NAME` — command and purpose.
2. `SYNOPSIS` — supported invocation forms.
3. `DESCRIPTION` — user-visible behavior and concepts.
4. `OPTIONS` — every supported command-line option.
5. `ESCAPES` or another short command-specific section when a distinct
   reference is needed.
6. `KEYBINDINGS` — interactive keys, each binding on its own line.
7. `CONFIGURATION` — path, canonical `key=value` examples, values, defaults,
   reload timing, and error behavior for configurable programs.
8. `COMMANDS` — internal commands when relevant.
9. `EXAMPLES` — realistic invocations only when useful.
10. `NOTES` — important consequences and limitations.
11. `SEE ALSO` — related Mangrove commands or current documentation.

Omit inapplicable sections. Keep manuals as reference pages rather than
tutorials or implementation notes. Preserve useful command-specific details,
but do not repeat a long description from another page when a link is clearer.
Document only behavior verified in source and tests. Quick help stays shorter
than the manual and focuses on invocation and options.

Configuration examples use `key=value` with no spaces around `=`. Parsers may
accept whitespace where documented, but examples and generated configuration
use one canonical spelling. When settings are loaded as a file, validation is
all-or-nothing: identify errors by path and line when available, keep defaults
on invalid input, and do not rewrite user files automatically.
