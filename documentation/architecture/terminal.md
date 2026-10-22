# Console and terminal behavior

Mangrove currently provides one framebuffer-backed system console. The kernel
maintains a text-cell ring and renders it with the embedded PSF2 font asset.
Screen dimensions derive from the UEFI framebuffer and font metrics; the
visible text grid is bounded to 256 columns and retains 512 rows. Scrolling
advances the ring and redraws the visible framebuffer. Cursor blinking and
batched presentation run in normal scheduled context.

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
keyboard-source lock.

Process console writes check alternate-screen ownership and render their full
buffer while holding one terminal lock acquisition. A concurrent screen
acquire or release is ordered before or after the complete write, never
between authorization and rendering. Kernel diagnostic writes retain their
kernel-only unchecked terminal entry point.

Shoot and the loam line editor use the byte-oriented standard streams and
native cursor/input helpers. Reed uses the alternate screen and raw key-event
API. Application-specific editing, shell completion, and mode behavior are
described in the [Shoot](../shell/README.md) and [Reed](../editor/README.md)
guides.

## Text and control behavior

The output decoder uses Mangrove's strict UTF-8 codec and presents malformed
prefixes as U+FFFD. An incomplete multibyte sequence is retained until later
output supplies more bytes; there is no per-write decoder reset or end-of-input
signal. The renderer uses one cell per code point and a font fallback glyph.
It does not implement grapheme clusters, combining-mark placement, East Asian
wide cells, shaping, or bidirectional layout.

The console recognizes carriage return, backspace, and line feed. Its limited
escape parser handles `ESC [ 2 J` (clear), `ESC [ 0 m` (normal video), and
`ESC [ 7 m` (inverse video). Other escape/CSI sequences are consumed or
ignored; this is not a VT, ANSI, or xterm-compatible terminal emulator. There
is no PTY or general terminal-device abstraction. The userland line editor
supports its documented key actions through native input events rather than
relying on broad escape-sequence compatibility.

## Interfaces and implementation

- Kernel renderer and text ring: `kernel/src/terminal.c`, `console.c`,
  `font.c`, and `utf8.c`.
- Kernel input queue and raw event ownership: `kernel/src/console.c` and
  `kernel/src/drivers/input/keyboard.c`.
- Native process API: `libc/include/mg/terminal.h`, implemented by the
  terminal-control syscall path in `kernel/src/syscall.c`.
- Standard streams and line editor: `libc/src/stdio_stream.c` and
  `libc/src/line_editor.c`.
