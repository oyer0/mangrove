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
Ordinary output is literal text: ESC bytes are rendered as U+FFFD and never
start a control sequence. ANSI/CSI-looking text, MGTC-looking bytes, and
markup-looking text therefore cannot clear or restyle the console. There is
no PTY or general terminal-device abstraction. The userland line editor
supports its documented key actions through native input events rather than
escape-sequence compatibility.

### Native terminal-control stream

The structured `mg/terminal.h` operations remain the direct native interface
for screen ownership, cursor movement and visibility, clearing, styling,
updates, dimensions, and input. They share the kernel terminal renderer and
process ownership checks. Applications can also open a dedicated, write-only
MGTC control-stream handle. Opening it requires stdout to be the process's
console handle and the process to be allowed to control the current screen;
a redirected stdout cannot open a stream that manipulates the console. The
handle is process-owned, is not inherited as stdout, and is closed with the
ordinary handle API.

MGTC version 1 is a concatenation of records:

```text
"MGTC" | version:u8 | type:u8 | payload_length:u32-le | payload
```

The header is 10 bytes. Defined types are UTF-8 text (1), foreground color
(2), background color (3), semantic style (4), style reset (5), cursor
position (6), cursor visibility (7), and clear (8). Color and style payloads
are one-byte values from `terminal_colors.h`; reset has no payload. Cursor
position is two little-endian u32 values, row then column, both zero-based as
in the structured API. Visibility is one byte (0 hidden, 1 shown). Clear is
one byte: line (1), cursor to line end (2), or screen (3). Text is opaque to
the record parser and follows the console's UTF-8 and C0 text behavior;
malformed UTF-8 is rendered with replacement characters.

Each stream write is limited to 65,536 bytes and must contain only complete,
well-formed records. The kernel copies and validates the complete batch,
checks cursor bounds and terminal ownership, then applies it under one
terminal-lock acquisition and schedules one coherent presentation. Invalid
version/type/length/payload, out-of-range coordinates, allocation failure, or
authorization failure leaves terminal contents and stream style unchanged.
Stream style starts at the default role and is local to the handle; `fg` and
`bg` change one component, a semantic role sets both components, and reset
restores default. Closing the handle discards that style. Separate writes
from any process may interleave; a write is the atomicity boundary. A userland
template larger than one batch is encoded fully before output and split only
between records, but a later I/O error can follow already-applied earlier
batches.

The shared loam encoder in `mg/terminal_markup.h` accepts explicit markup
templates using this canonical notation:

| Directive | Effect |
| --- | --- |
| `{fg:NAME}`, `{bg:NAME}` | Set foreground or background to one of the 16 palette names (`black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `light-gray`, `dark-gray`, `bright-red`, `bright-green`, `bright-yellow`, `bright-blue`, `bright-magenta`, `bright-cyan`, `white`). |
| `{style:ROLE}` | Apply `default`, `directory`, `error`, `warning`, `success`, or `heading`. |
| `{reset}` | Restore the stream's default colors. |
| `{at:ROW,COLUMN}` | Move to one-based coordinates; an out-of-range position fails its MGTC write batch before any records in that batch are applied. |
| `{cursor:show}`, `{cursor:hide}` | Set cursor visibility. |
| `{clear:line}`, `{clear:to-end}`, `{clear:screen}` | Clear the current line, from cursor to line end, or the screen. |
| `{{`, `}}` | Emit a literal opening or closing brace. |

Only these spellings are recognized. There are no RGB, bold, underline, or
alias forms. A directive is limited to 128 source bytes; a longer candidate
is emitted as literal text. Unknown, malformed, or incomplete directives
remain literal. The parser validates a full directive before encoding it.
Markup is never inferred from ordinary output: use
`terminal_markup_write_template()` only for an explicit template. Display
untrusted file, network, or log text with
`console_write()` or a text record, never by inserting it into a template.
When stdout is redirected or the control stream is unavailable, the encoder
emits visible text only and omits recognized terminal operations. Ordinary
stdout stays byte-oriented text, including literal braces and ESC sequences.
`say` uses the encoder only when explicitly invoked with `--markup`.

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
  terminal-control syscall path in `kernel/src/syscall.c`.
- Standard streams and line editor: `libc/src/stdio_stream.c` and
  `libc/src/line_editor.c`.
