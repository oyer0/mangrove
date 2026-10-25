# Read-only text pager

`userspace/common/text_pager.c` provides a shared full-screen pager for native
read-only text applications. `/bin/view FILE` reads an ordinary text file,
while `/bin/info PROGRAM` reads the optional manual at
`/share/info/PROGRAM`; a missing manual is reported without creating files.
Quick-help records in `/share/help` remain separate. The pager uses the native
terminal-control interface to claim the alternate screen and raw key-event
input, emit changed text/color runs inside terminal update transactions,
and restore input ownership and the ordinary
screen before returning.

The existing current and previous cell frames identify unchanged cells and
rows. Changed runs include explicit styled spaces when old text must be
erased; the pager does not clear and rewrite an entire row first. An unchanged
viewport emits no terminal output. Navigation and search events remain ordered
and are processed individually; this renderer does not discard queued input.

The caller supplies the title, path label, and loaded bytes. The pager never
opens a file for writing. `text_pager_read_file()` accepts regular files up to
4 MiB, verifies that the initial size can be read completely, and rejects a
file that grows during the read. A source that changes while being read is not
a stable snapshot; short reads and read errors are returned to the caller.
Applications should report errors after the pager has restored the console.

Documents are laid out as visual rows. LF and CRLF end logical lines; soft
wrapping never changes the supplied bytes. Tabs expand to four spaces by
default. Layout initially treats each valid UTF-8 scalar as one cell. Invalid
UTF-8 bytes and unsupported control characters are displayed as U+FFFD; the
source text remains unchanged. Grapheme clusters, wide-character cell widths,
shaping, and bidirectional layout are unsupported.

The bounded layout accepts terminal sizes up to 256 columns by 128 rows, with
at least two rows required for document content and status. It supports at
most 262,144 visual rows. These limits bound the document index and render
frames; larger documents or layouts fail visibly rather than being truncated.

The shared controls are `j`/Down and `k`/Up for visual rows, Page Up/Down and
Space for page movement, `g`/Home and `G`/End for document boundaries, and
`q` to exit. `?` opens incremental literal search. Escape cancels a query and
restores its starting position; after a match is accepted, Escape exits search
mode at the current reading position, and another Escape closes the pager.
`n` and `b` move between matches without wrapping. Matches are highlighted
while a query is edited and after it is accepted.

The pager is an in-process component, not a shell pipeline or terminal
emulator. Applications choose which files to load and how to describe them.
