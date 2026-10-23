# Console and terminal behavior

Mangrove currently provides one framebuffer-backed system console. The kernel
maintains a text-cell ring and renders it with the embedded PSF2 font asset.
Screen dimensions derive from the UEFI framebuffer and font metrics; the
visible text grid is bounded to 256 columns and 512 rows, matching the
two 512-row cell stores. A framebuffer too small for the text margins still
uses a one-cell logical grid, and a taller framebuffer clips the visible grid
to the ring capacity. Scrolling advances the ring and redraws the visible
framebuffer. Cursor blinking and batched presentation run in normal scheduled
context.

Dirty tracking records individual screen rows and flushes contiguous changed
runs to video memory. Updating a cursor row and the bottom status row does not
copy unchanged rows between them. This matters because presentation holds the
IRQ-safe terminal lock: unnecessary large copies can delay USB completion
processing and leave a key release behind queued host typematic reports.
Full redraws and viewport scrolling still require larger copies; presentation
does not guarantee a fixed maximum interrupt-disabled duration.

The normal input path turns USB keyboard text into UTF-8 bytes in a bounded
console queue. Reads block through the scheduler until input arrives. A
full-screen application can acquire the alternate screen, which saves and
restores the normal screen state. Its owner may request raw key events,
cursor/clear operations, styled output, batched updates, and a bounded visual
overlay through the versioned `mg/terminal.h` native API. These operations do
not create separate terminals or pseudo-terminals. Only the process owning
the alternate screen may control its full-screen state or read raw key events;
ordinary system output is denied while another process owns that screen.

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

Shoot and the loam line editor use the byte-oriented standard streams and
native cursor/input helpers. Reed uses the alternate screen and raw key-event
API. The line editor presents each prompt and input-line redraw as one terminal
update, so moving its cursor to the input anchor is not exposed as an
intermediate framebuffer frame. Application-specific editing, shell
completion, and mode behavior are described in the [Shoot](../shell/README.md)
and [Reed](../editor/README.md) guides.

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
Wrapping is deferred
until the next printable character; while that wrap is pending, the visible
cursor is drawn on the last cell of the current row. The limited escape parser
handles `ESC [ 2 J` (clear the text ring and home the cursor), `ESC [ 0 m`
(normal video), and `ESC [ 7 m` (inverse video). Each CSI sequence starts with
no parameters; unsupported CSI input is consumed through its final byte and
does not leak its remaining bytes into displayed text. Other escape/CSI
sequences have no terminal effect; this is not a VT, ANSI, or xterm-compatible
terminal emulator. There is no PTY or general terminal-device abstraction.
The userland line editor supports its documented key actions through native
input events rather than relying on broad escape-sequence compatibility.

The loam line editor renders its prompt when an input line is prepared (and
again when completion explicitly clears that line). Ordinary insertion and
deletion reposition within the anchored input and repaint only the changed
suffix, clearing any old tail after a shortening edit. Cursor-only movement
changes the input cursor without rewriting prompt or text cells. History and
completion replacements may repaint the full input text while leaving an
unchanged prompt alone. Each update remains inside one terminal transaction,
so temporary cursor positions are not presented. The terminal renderer still
owns cell drawing, wrapping, and dirty-row presentation; this does not add a
userspace screen buffer. Input offsets retain the existing display model in
which each UTF-8 scalar occupies one cell.

## Interfaces and implementation

- Kernel renderer and text ring: `kernel/src/terminal.c`, `console.c`,
  `font.c`, and `utf8.c`.
- Kernel input queue and raw event ownership: `kernel/src/console.c` and
  `kernel/src/drivers/input/keyboard.c`.
- Native process API: `libc/include/mg/terminal.h`, implemented by the
  terminal-control syscall path in `kernel/src/syscall.c`.
- Standard streams and line editor: `libc/src/stdio_stream.c` and
  `libc/src/line_editor.c`.
