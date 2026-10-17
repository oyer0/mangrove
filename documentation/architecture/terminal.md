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
