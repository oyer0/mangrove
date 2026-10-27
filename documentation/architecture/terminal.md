# Console and terminal behavior

Mangrove currently provides one framebuffer-backed system console. The kernel
maintains a text-cell ring and renders it with the embedded PSF2 font asset.
Screen dimensions derive from the UEFI framebuffer and font metrics; the
visible text grid is bounded to 256 columns and 512 rows, matching the
two 512-row cell stores. A framebuffer too small for the text margins still
uses a one-cell logical grid, and a taller framebuffer clips the visible grid
to the ring capacity. Scrolling advances the ring, copies retained framebuffer
rows, and renders newly exposed rows; a full-viewport scroll redraws the grid.
Cursor blinking and batched presentation run in normal scheduled context.

The cursor is an inverted glyph overlay in the pixel backbuffer, never part
of a stored text cell. Starting the outermost update restores its underlying
glyph and colors before changing the logical cursor or ring origin. Cursor
APIs inside that update change the desired position and visibility without
painting an intermediate overlay. Deferred scrolling can leave logical rows
ahead of the pixel layout: the cursor remains unpainted until presentation
has copied the rows and cleared the pending scroll. Presentation then draws
the final cursor once. The blink worker leaves pending output to the
presentation worker, so cursor pixels cannot become copied scroll content.

Dirty tracking records a horizontal pixel span for each changed screen row
and flushes adjacent rows together when their spans match. Glyph and cursor
updates copy their affected cells rather than the full row. Identical glyphs
with identical colors are not rasterized again. Full-row clears and scrolling
still invalidate their complete affected rows. Updating a cursor row and the
bottom status row does not copy unchanged rows between them. This matters because presentation holds the
IRQ-safe terminal lock: unnecessary large copies can delay USB completion
processing and leave a key release behind queued host typematic reports.
Full redraws and viewport scrolling still require larger copies; presentation
does not guarantee a fixed maximum interrupt-disabled duration.

The permanent GOP mapping uses the [framebuffer cache policy](memory.md) to
select WC only for a validated aperture, with strict UC fallback. The rendering
and locking model is unchanged. A framebuffer flush issues SFENCE after its WC
stores, before presentation completes or the terminal lock is released. There
is one fence per copied region, not per glyph/pixel; UC flushes need none.
The initial VRAM snapshot and direct-render fallback order preceding WC stores
against framebuffer reads with MFENCE. This also covers a failed RAM-backbuffer
allocation. Cursor overlays, pending-scroll handling and dirty spans retain
their existing ownership and invalidation rules.

The cursor starts disabled during kernel startup. Terminal writes and clears
preserve that state, so noninteractive boot output cannot reveal it. The
interactive line editor enables the cursor when its prompt is ready. Login's
cooked input helper flushes the prompt, ends any open console transaction, and
then enables the cursor immediately before its first blocking read. Fullscreen
applications manage visibility through the same terminal API and restore the
saved normal-screen cursor state when they leave the alternate screen.

The normal input path turns USB keyboard text into UTF-8 bytes in a bounded
console queue. Reads block through the scheduler until input arrives. An
interactive process may claim the versioned `mg/terminal.h` key-event input
mode while the normal screen is active; only that process may read those
events. During an alternate-screen session, only its owner may claim and read
events. The event includes USB HID key identity, modifier state, text
codepoint, and repeat state, but not a separate key-release event. An event
reader claimed on the normal screen drains bytes queued before its claim as
byte-tagged events. Alternate-screen clients retain the event-only queue
contract. When a normal-screen owner releases the mode, queued unmodified
text is returned to the cooked byte queue; control and navigation events are
not converted to text. Simple cooked readers such as login retain their byte interface.
Concurrent cooked byte reads return `MG_ERR_BUSY` while a process owns normal
screen key-event input, so they cannot consume bytes from its editing session.

On the normal screen, the terminal consumes Page Up and Page Down from the
authorized key-event reader to move the displayed viewport by one visible
page. Alt+Page Up selects the oldest retained row; Alt+Page Down returns to
live output. Ctrl-modified Page keys and other modifier combinations are
delivered to the application. When an ordinary input event is delivered while
the viewport is historical, the terminal first returns to live output so a
shell cannot edit a prompt hidden from view. Alternate-screen owners receive
all Page events unchanged.

The normal screen and alternate screen each use their existing 512-row cell
ring; scrollback does not add a second history store. Visible rows occupy part
of that capacity, leaving at most `512 - visible_rows` rows before the live
origin. The terminal tracks the live origin independently from the displayed
origin while new output continues to update the ring. It does not follow new
output while scrolled back. If output evicts the viewed oldest row, the
viewport advances only to the oldest row that remains valid. The live cursor
stays at its logical position and is hidden over historical output, then is
restored when the viewport returns live.

`ESC{cls}` is a viewport clear and preserves the logical cursor row and
column. On the live normal screen, it advances the live origin by one visible
page and clears the fresh live rows. The just-vacated rows become the newest
scrollback page; if the 512-row ring is full, only its oldest rows are evicted
to make room. The cursor is remapped to the same row and column in the new
viewport, so later output cannot overwrite retained history. While viewing
history, `cls` blanks only the displayed frame and retains its origin and
cells; navigating, including a boundary Page key, restores the retained page.
On the alternate screen, `cls` clears that screen's visible rows in place,
preserving its existing cursor behavior. Cursor movement commands continue to
address the live logical screen. The `clear` command and Ctrl+L explicitly
clear and home the live cursor after preserving the vacated page.

At a login-session boundary, after sessiond has retired the previous session's
processes, logind invokes a separate kernel operation restricted to its
authenticated session-management service identity. That operation clears
both rings and their history metadata before the next login prompt. The
ordinary clear command cannot erase or expose another session's retained
history.

A full-screen application can acquire the alternate screen, which saves and
restores the normal screen state. Its owner may also request cursor/clear
operations, styled output, batched updates, and a bounded visual overlay.
These operations do not create separate terminals or pseudo-terminals.
Ordinary system output is denied while another process owns the alternate
screen.

The versioned terminal capability query reports whether the current process's
standard output is the interactive console. Applications may use that flag
for in-place carriage-return progress; it is absent when standard output is a
file, including inherited command redirection. This capability does not imply
ANSI or VT support.

Console byte and raw-event queues, their indices and counts, raw-mode owner,
and waiter slots are protected by one IRQ-safe SMP spinlock. Queue overflow
continues to drop new input. Readers register under the lock, release it before
blocking, and recheck the durable queue after wakeup. Input producers use the
scheduler's event-wake handshake: a wake either unblocks a parked waiter or is
recorded if that waiter was preempted or still running before its block call.
No console queue lock is held while a reader sleeps or while debug output is
rendered.

Each USB keyboard source has a separate IRQ-safe lock protecting its report
history and typematic state. The xHCI worker and timer repeat path serialize
those fields through the same lock. Lock ordering is keyboard-source lock to
console-input lock to scheduler lock; terminal ownership transitions take the
terminal lock before the console-input lock. No path takes those locks in the
reverse order. LED transport synchronization occurs after releasing the
keyboard-source lock. A raw client has at most one queued repeat per keycode.
When a newly pressed key takes over repeat generation on a USB source, a
pending repeat for the previous key is discarded. Releasing the active key
also cancels its pending repeats. Initial keypress events are preserved, and
already-consumed events cannot be withdrawn.

Process console writes check alternate-screen ownership and render their full
buffer while holding one terminal lock acquisition. A concurrent screen
acquire or release is ordered before or after the complete write, never
between authorization and rendering. Kernel diagnostic writes retain their
kernel-only unchecked terminal entry point.

Shoot uses the loam line editor, standard output, and the process-owned key
event mode for interactive editing. Its event claim is released when each
line read ends, preserving queued ordinary text for the next cooked reader.
Other byte-oriented programs remain on the cooked input path unless they opt
in. Reed uses the alternate screen and the same raw key-event API. The line
editor presents each prompt and input-line redraw as one terminal update, so
moving its cursor to the input anchor is not exposed as an intermediate
framebuffer frame. Application-specific editing, shell completion, and mode
behavior are described in the [Shoot](../shell/README.md) and
[Reed](../editor/README.md) guides.

## Text and control behavior

The output decoder uses Mangrove's strict UTF-8 codec and presents malformed
prefixes as U+FFFD. An incomplete multibyte sequence is retained until later
output supplies more bytes; there is no per-write decoder reset or end-of-input
signal. The renderer uses one cell per code point and a font fallback glyph.
It does not implement grapheme clusters, combining-mark placement, East Asian
wide cells, shaping, or bidirectional layout.

The console recognizes carriage return (column zero), backspace (move left by
one cell when possible, without erasing), and line feed (column zero on the
next row). A tab advances with blank cells to the next eight-column stop.
Wrapping is deferred until the next printable character; while that wrap is
pending, the visible cursor is drawn on the last cell of the current row.
Mangrove's byte-oriented console output recognizes its native control stream:
an actual ASCII ESC byte (`0x1b`) followed by `{`, a canonical command, and
`}`. This is not ANSI/CSI or VT compatibility. All byte-stream output
controls use `ESC{...}` and pass through one parser, terminal state, renderer,
and process-ownership check. The `mg/terminal.h` output convenience functions
emit these same bytes through stdout; they do not invoke a second output
control syscall. Ordinary braces are printable text;
the source spelling `\e` is not itself a terminal command and is interpreted
only by a program that explicitly translates it to byte `0x1b`.
A C program can emit the same stream with `printf("\x1b{fg:green}...")` or
`console_write()`; Shoot's `say -e` explicitly translates `\e` in an
argument to the ESC byte.

The recognized command set is deliberately small:

| Sequence | Effect |
| --- | --- |
| `ESC{fg:COLOR}`, `ESC{bg:COLOR}` | Set foreground or background using the same 16 palette names: standard `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `l_gray`; bright `d_gray`, `b_red`, `b_green`, `b_yellow`, `b_blue`, `b_magenta`, `b_cyan`, `white`. These names map in order to the existing palette entries; RGB values are unchanged. |
| `ESC{warn}`, `ESC{err}`, `ESC{ok}`, `ESC{head}` | Apply the existing warning, error, success, and heading semantic styles. |
| `ESC{reset}` | Restore default foreground and background. |
| `ESC{at:ROW,COL}` | Move to one-based visible-screen coordinates. Zero, overflow, and out-of-range coordinates are rejected as a command and displayed literally. |
| `ESC{up:N}`, `ESC{down:N}`, `ESC{left:N}`, `ESC{right:N}` | Move by a positive number of rows or columns. Motion stops at the visible screen edge; it does not wrap. |
| `ESC{hide}`, `ESC{show}` | Hide or show the cursor. |
| `ESC{cls}`, `ESC{cll}`, `ESC{cleol}` | Clear the visible viewport without moving the logical cursor, clear the current line, or clear from the cursor to the line end. On the live normal screen, `cls` retains the vacated page in the ring; on a historical view it blanks only the presentation. |
| `ESC{alt}`, `ESC{main}` | Enter the alternate screen or return to the retained main screen. Repeating `alt` by the current owner preserves the alternate screen; repeating `main` when already on main is a no-op. |

Only these exact names and forms are supported. Operation identifiers are at
most five ASCII characters; color names and numeric arguments are exempt.
There are no aliases. RGB, bold, underline, and arbitrary device controls are
not part of this stream. A candidate sequence is bounded to 128 bytes,
including ESC and its braces. The parser retains incomplete candidates across
writes from the same process, validates the complete command before applying
it, and keeps parser and style state per process. Different processes cannot
complete each other's partial sequences. Unknown, malformed, oversized, and
process-final incomplete sequences are rendered as literal text (ESC itself
appears as the replacement glyph). A command that fails validation has no
terminal side effect. Output writes are serialized under the terminal lock;
separate process writes may appear between one another's completed writes.

Commands use the existing terminal control authorization. A process that does
not own an active alternate screen cannot write to it or change it. The ESC
grammar cannot claim keyboard input; applications use the native input API for
that operation. `alt` and `main` use the kernel's existing process-ownership
and raw-input checks. On the ordinary screen, commands can move or clear the
display, so they are not safe for untrusted text:
applications displaying file, network, or log contents must use
`console_write_literal()`/`object_write_literal()`. The styled-write helpers
also send their payload through literal output between canonical style and
reset commands. `type` and `logv` use literal writes. `say` is literal by
default; its explicit `-e`/`--escapes` option
translates selected backslash escapes before writing. When output is
redirected, emitted bytes go to the file unchanged and are not interpreted by
the terminal.

On the live normal screen, `ESC{cls}` retains the vacated visible rows in the
same ring and advances the live origin by one viewport. It preserves the
cursor's row and column by remapping them into the fresh live rows. If the
user is scrolled into history, `cls` blanks the presentation only; the next
scrollback navigation redraws retained cells. Alternate-screen `cls` clears
its visible rows in place. `ESC{at:ROW,COL}` moves without clearing, so a
caller that wants a cleared screen at the origin emits both operations. The
loam `terminal_clear_screen()` helper, used by the `clear` command and Ctrl+L,
emits `ESC{cls}` followed by `ESC{at:1,1}`. Ctrl+L then redraws the Shoot
prompt at the origin. The kernel's authorized session reset remains a
separate operation that erases both rings and their validity metadata.

The terminal ioctl-style operation slots formerly used for alternate-screen,
cursor, clear, and direct styled writes remain reserved in the version-1 ABI
and return unsupported. `terminal_alternate_*`, cursor, clear, and styled
write functions in loam are small encoders for the canonical output stream;
they require interactive stdout for screen controls, and styled writes emit
only literal text when stdout is redirected. The loam line editor therefore
requires console stdout as well as console input; with file stdout its initial
cursor synchronization returns unsupported before reading a key. A launcher
whose own stdout is redirected can explicitly select its writable console
handle through `process_spawn_argv_with_output()`.
Rectangular clearing has no
canonical stream command and is not exposed as a public operation. The
remaining `mg/terminal.h` syscall interface covers distinct facilities that
are not output-control syntax: terminal dimensions/capabilities, input events
and ownership, update transactions, line-editor cursor positioning, and the
completion overlay. These operations do not define a second byte protocol.
There is no PTY or general terminal-device abstraction. The userland line
editor uses native input events rather than escape-sequence key compatibility.

The loam line editor renders its prompt when an input line is prepared (and
again when completion explicitly clears that line). Ordinary insertion and
deletion reposition within the anchored input and repaint only the changed
suffix, clearing any old tail after a shortening edit. After text is written,
the line editor explicitly requests a visible cursor at its final logical
position. The renderer paints it after the transaction's text and scroll
updates. Cursor-only movement changes the input cursor without
rewriting prompt or text cells. History and completion replacements may
repaint the full input text while leaving an unchanged prompt alone. Each
update remains inside one terminal transaction, so temporary cursor positions
are not presented. The terminal renderer still owns cell drawing, wrapping,
and dirty-row presentation; this does not add a userspace screen buffer. Input
offsets retain the existing display model in which each UTF-8 scalar occupies
one cell.

Reed builds its document viewport from the same cell model. Soft wrapping is
Reed's default and is performed at scalar/cell boundaries without changing
logical lines or file bytes. Its optional no-wrap mode applies one shared
horizontal offset to every document line. Reed expands tabs according to its
per-user configuration; this is distinct from the kernel console's fixed
eight-column tab stops described above. Neither renderer performs grapheme
segmentation, wide-character layout, shaping, or bidirectional presentation.

Reed locates the viewport's first logical line, then advances through its
line spans while building visual rows. Visible rows reuse those spans instead
of independently locating each line from the beginning of the buffer. Each
row stops decoding at its right edge, and the status/cursor geometry reuses
the position computed for viewport adjustment. Downward movement tests the
target visual row directly; it does not count every remaining row first.
These are per-update calculations, without a persistent layout cache.
Locating a deep viewport still traverses the preceding text, and a wrapped
row far into a single long line still decodes that line's prefix. The existing
frame comparison emits only changed text/color runs, within one terminal
update transaction. Input events remain ordered and are not discarded or
combined to reduce rendering work.

## Interfaces and implementation

- Kernel renderer and text ring: `kernel/src/terminal.c`, `console.c`,
  `font.c`, and `utf8.c`.
- Kernel input queue and raw event ownership: `kernel/src/console.c` and
  `kernel/src/drivers/input/keyboard.c`.
- Native process API: `libc/include/mg/terminal.h`, implemented by the
  terminal syscall path for dimensions, input, overlays, and update
  transactions in `kernel/src/syscall.c`; its output helpers encode the
  canonical ESC stream through stdout.
- Standard streams and line editor: `libc/src/stdio_stream.c` and
  `libc/src/line_editor.c`.
