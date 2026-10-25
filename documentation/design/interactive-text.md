# Interactive keyboard, text, and clipboard design

This document records the accepted interactive-text architecture, current
Stage 5 behavior, and deferred proposals. The implementation contract for each
component remains in its authoritative guide: [terminal](../architecture/terminal.md),
[Reed](../editor/README.md), [Shoot](../shell/interactive.md), and
[IPC/service](../architecture/ipc-services.md). Clipboard and later
internationalization sections describe future work unless explicitly marked as
implemented below.

## Status and design goals

The immediate Stage 5 goal is a reliable English/US-keyboard console and
consistent interaction between Shoot and Reed. The project direction is to
keep text and input interfaces capable of growing beyond that baseline without
corrupting text or tying applications to kernel-specific shortcuts.

The following are accepted architectural decisions; their implementation
status is stated in the relevant sections below:

- Application-facing keyboard events and modifier state belong at a documented
  interface. Individual applications own their keymaps.
- Alt is Mangrove's primary application-command modifier where a command chord
  is appropriate; Reed's plain modal keys remain valid commands. Ctrl remains
  available for terminal and process-control functions; it is not a universal
  application shortcut namespace.
- Reed's intended navigation includes arrows, `h j k l`, and `b n B N` with
  the word-motion rule below. Selection includes character and line modes.
- Copy, cut, paste, undo, redo, save, and search should be recognizable
  operations, while exact bindings remain contextual and subject to the
  existing compatibility review.
- Clipboard ownership is a system-wide userspace service. Its architecture
  supports MIME-aware items with multiple representations, negotiation,
  atomic transfers, and session isolation. Source-process exit does not destroy
  published clipboard content. The initial implementation may transfer only
  UTF-8 plain text.
- Internationalization is a permanent architecture goal. Valid text should be
  preserved even when the current console cannot fully enter or render it.

Further event forms, keyboard layouts, richer clipboard representations and
resource fairness remain design work. The current terminal event layout is a
versioned ABI, documented in the terminal header and architecture guide.

## Current implementation

The active keyboard transport is USB HID. The kernel applies a US-style ASCII
map, modifiers, and repeat. Cooked console readers receive UTF-8 bytes and
retain legacy semantic editor-action packets for compatibility. The opt-in
terminal event mode carries HID key identity, modifiers, text code point, and
repeat state, but no separate release event. A process may claim events on the
normal screen; on the alternate screen, only its owner may claim them. On
the normal screen, previously queued cooked bytes are delivered first as
tagged events; alternate-screen raw clients keep an event-only queue. On
normal-screen release, queued unmodified text is returned to the cooked queue;
cooked byte readers cannot consume input while an event owner is active. The
compatibility mapping is in
[`keyboard.c`](../../kernel/src/drivers/input/keyboard.c); the public event
layout is in [`terminal.h`](../../libc/include/mg/terminal.h).

Shoot uses loam's event-aware line editor for its nonmodal keymap, history,
deletion, and completion. It has no command-line selection. Its exact bindings
and scalar-based word rules are documented in the
[interactive Shoot guide](../shell/interactive.md). Reed consumes raw events
and has Normal, Insert, Selection, Command, and Search modes. The implemented
keymap, word/paragraph boundaries, selection transitions, command interface,
and incremental search behavior are specified in the
[Reed guide](../editor/README.md). Reed copy/cut/paste uses the session
clipboard. Its implemented protocol and resource contract is in
[Session clipboard](../architecture/clipboard.md).

Shoot handles Ctrl+C cancellation, Ctrl+D EOF/deletion, and Ctrl+L clear only
while it owns its event-mode line read. Login input uses a direct cooked byte
reader. Other raw keyboard use found in current applications is Tmon's
`q`/Escape exit. Kernel keymaps should not acquire more application commands;
doing so would make one input policy affect the shell, login, and future
applications alike.

The shared UTF-8 code validates and decodes Unicode scalars. Reed keeps byte
offsets at scalar boundaries. Shoot's word operations use scalar boundaries,
group ASCII letters/digits/underscore and valid non-ASCII scalars, and treat
ASCII punctuation and whitespace as separators. This is intentionally not
Unicode word segmentation. The terminal currently renders one cell per scalar
and has no general grapheme, combining-mark, wide-cell, shaping, or
bidirectional layout. The [runtime contract](../architecture/c-runtime.md)
and [terminal contract](../architecture/terminal.md) describe these limits.

## Stage 5 keyboard and editing contract

### Event and modifier boundary

The application-facing contract should distinguish physical key identity and
modifiers from committed text. Key events should preserve press/repeat/release
state and ordering; a text commit may contain UTF-8 rather than assuming one
key produces one character. Composition/preedit events can be added by the
future input-method layer. An opt-in event path should be available to the
foreground interactive owner on both the normal screen and alternate screen;
current raw input's alternate-screen restriction is not sufficient for
Shoot-specific commands. Simple byte-oriented programs should retain a cooked
input path. Whether the kernel transports these events directly or a userspace
input service performs layout translation remains undecided.

Shoot and Reed translate the current key events in userspace. Shoot uses Ctrl+C,
Ctrl+D, and Ctrl+L for line cancellation, EOF, and terminal clear. Reed uses
Ctrl+Home/End for document boundaries. These bindings are application-local;
direct cooked readers such as login do not receive editor action packets. Alt
is the primary application-command modifier where Reed uses a chord. The
current USB path remains US-style and does not yet provide key-release events
or selectable layouts.

### Reed navigation

Normal and Selection modes use this navigation map:

| Key | Action |
| --- | --- |
| Arrows / `h j k l` | Character and line movement. Vertical movement retains a preferred display column. |
| `b` / `n` | Previous word end / current or next word end. |
| `B` / `N` | Current or previous word start / next word start. |
| `a` / `e`, Home / End | Start / end boundary of logical line. |
| `g` / `G`, Ctrl+Home / Ctrl+End | Start / end boundary of document. |
| `[` / `]` | Previous / next paragraph. |
| Page Up / Page Down | Move by one text viewport while retaining preferred display column. |
| `o` / `O` | Open a new logical line below / above the current line and enter Insert mode there. |

Stage 5 is English/US-first and scalar-safe, not grapheme-aware. Reed treats
ASCII letters, digits, underscore, and each valid non-ASCII scalar as word
characters. ASCII punctuation, spaces, tabs, carriage returns, and line breaks
are separators. The motions follow these exact boundary rules:

- In Stage 5, ASCII letters, digits, underscore, and non-ASCII scalars form
  word runs. ASCII punctuation, spaces, tabs, and line breaks are separators;
  punctuation does not join neighboring runs.
- Word boundaries are never allowed inside a UTF-8 scalar. The later shared
  grapheme model will make them grapheme boundaries.
- Under the later Unicode model, words consist of letter, number, mark, and
  connector-punctuation sequences; Unicode-defined internal punctuation may
  connect letters or numbers.
- Whitespace and line breaks separate words. Under the Unicode model,
  punctuation-only and symbol-only spans are skipped; grapheme navigation
  still reaches every symbol and emoji.
- `b` and `n` land on the first byte of the final scalar of the target word;
  their cursor destinations are inclusive. `n` from before the final scalar
  reaches it. From that scalar, the exclusive word end, or a separator, `n`
  advances to the next word's final scalar. If there is no next word, it stays
  put. `b` skips the containing word, including one whose exclusive end is at
  the cursor, and reaches the preceding word's final scalar; if no preceding
  word exists, it stays put.
- `N` skips a containing word and moves to the next word start; from a
  separator it finds the next word. It stays put when there is no next word.
  `B` from inside a word reaches its start. If already at a word start, another
  `B` moves to the previous word start. From a separator it reaches the
  preceding word's start; it stays put when there is no previous word.
- Word motions do not wrap at document edges; unavailable targets leave the
  cursor unchanged. Empty lines and final-newline state affect line navigation,
  not word classification.
- Concrete cases: `wo|rd next` + `n` → `wor|d next`,
  `word wo|rd` + `B` → `word |word`, `hello wo|rld` + `b` →
  `hell|o world`, and `wo|rd next` + `N` → `word |next`. From `one| two`
  (cursor after `one`), `n` targets the final scalar of `two`; repeated `B`
  from inside `three` reaches `three`, `two`, then `one` starts in order.
- A paragraph is a run of nonblank logical lines separated by one or more
  blank lines. Empty lines and lines containing only ASCII space, tab, or CR
  are blank. `[` moves from inside a paragraph to its first line; from that
  line's start or from a separator, it moves to the previous paragraph's first
  line. `]` moves to the next paragraph's first line, or from a separator to
  the next nonblank paragraph. Missing previous/next paragraphs leave the
  cursor unchanged.
The exact Unicode word-boundary version and whether word motions advance over
emoji as separate word-like units remain design questions for the shared text
specification. Stage 5's scalar-based selection and navigation must not be
described as grapheme-aware.

### Selection and common operations

Normal `s` begins character selection, `S` begins line selection, and Alt+`S`
selects the whole document. Navigation extends from the original anchor.
Pressing the active selection key or Escape returns to Normal mode while
keeping the endpoint; pressing the other selection key changes granularity
without moving either endpoint. Line selection covers complete logical lines
and the appropriate separator; deleting through the final line consumes its
preceding newline when present and never synthesizes a final newline. `d`
deletes a selection as one undo transaction. Insert mode cannot be entered
while a selection is active. The highlight is rendered by Reed's existing
framebuffer transaction.

In Normal mode, `d` deletes the scalar at the cursor, Backspace deletes before
the cursor, and Delete deletes at the cursor. `u` and Shift+`U` undo and redo.
These operations do not copy implicitly. `c`, `x`, and `v` use the system
clipboard; a motion operator's edit range is half-open. For inclusive `b`/`n`
destinations, extend the high endpoint through the destination scalar using the next UTF-8 scalar
boundary; when moving backward with `b`, the destination already forms the
inclusive low endpoint. Do not pass the final-scalar cursor position directly
as an exclusive edit end. `B`/`N` retain their word-start destination
semantics. `cc`/`xx` cover the complete logical line using the
line-selection newline rule. In Selection mode, `c`/`x` apply to the selected
range; `v` pastes at the cursor in Normal mode or atomically replaces the
selection. A failed clipboard operation leaves the document unchanged. These
bindings use clipd; Reed has no private editor register.

`/` remains the command prompt. `/s`, `/q`, and `/sq` retain Save, protected
Quit, and Save-and-Quit semantics; `/saveas PATH`, `/open PATH`, and
`/find TEXT` remain available. Command names are case-insensitive tokens
separated from arguments by ASCII spaces or tabs. Tab completes an unambiguous
command name, not a path. Alt+`S` selects all; Save and Quit are commands, not
Alt shortcuts. Reed's existing conditional safe-save contract is unchanged.

`?` opens incremental literal search. Matches are highlighted as the query is
entered while the cursor remains at the original anchor. Enter chooses the
nearest match by document byte position, choosing the later match on a tie,
and places the cursor after that match. No match returns to Normal mode at the
anchor. Search mode retains highlights; `n` moves forward and `b` backward
from the current cursor, without wraparound. Uppercase `N` and `B` retain their
word-motion meanings and do not change the active search match. Escape or `?`
exits at the current cursor and clears highlights. Editing commands are
unavailable in Search mode.

Shoot retains its line editing, history, completion, and process-control
semantics. It may reuse shared text boundaries and clipboard client functions,
but not Reed's modal keymap. Ctrl+C should remain available for line
cancellation or foreground process control, according to terminal state; it
must not silently become copy. Alt+C/X/V are candidate clipboard operations
for a selected command-line range, but are not approved bindings. Tab
completion and Up/Down history retain priority.

## Clipboard architecture

### Accepted direction

Clipboard contents belong to a supervised userspace service, not kernel global
storage and not the copying process. The service owns an item containing one
or more MIME-typed representations. Clients can discover available formats
and negotiate a representation. Publication and transfer are atomic: readers
see the old complete item or the new complete item, never a partially updated
one. Clipboard content is scoped to the authenticated session, shared by
authorized applications in that session, and survives the source process
exiting.

The implemented representation is `text/plain;charset=utf-8`.
That first format does not constrain the item model to text. There is no
permanent universal 2 MiB clipboard maximum. Initial per-representation and
aggregate resource limits must be explicit, bounded, and revisable. Large data
may later need a streaming or object-transfer mechanism rather than copying it
through small IPC messages.

### Current interface and extensions

The public loam API, chunked wire protocol, item generations, transfer expiry,
resource budget and session lifecycle are implemented in
[Session clipboard](../architecture/clipboard.md). Clients discover formats and
request an exact MIME representation; convenience text operations use the
same transfer boundary. Version 1 publishes one complete UTF-8 representation.
The accepted next extension is atomic publication of multiple representations
of one item. Its offering operations and admission policy still need design;
there is no requirement to replace the service or session ownership model.
No kernel clipboard syscall is needed for the current implementation.

Future representations may include rich text, images, file references, and
larger streaming content. Each requires explicit authorization, ownership,
size/resource policy, and error semantics. A clipboard service restart may
lose volatile contents unless a later decision requires recovery; surviving
the source application's exit is the Stage 5 requirement.

## Internationalization milestones

Internationalization is broader than accepting UTF-8 bytes. Keep these layers
separate and expose support honestly:

| Layer | Planned milestone | Support boundary |
| --- | --- | --- |
| UTF-8 preservation and validation | Stage 5 | Do not corrupt valid stored or transferred text. Existing Reed and filesystem limits remain format/API-specific. This does not promise a keyboard layout for every script. |
| Keyboard events and layouts | Stage 5 event boundary; Stage 7 hardware qualification | Stage 5 stabilizes English/US use. Later layout selection must map physical events to text without embedding a US layout in applications. Hardware results inform supported-device claims. |
| Grapheme editing and Unicode word boundaries | Stage 6 shared text milestone, after Stage 5's input contract | Cursor movement, deletion, selection, and word motions operate on shared boundaries; the Unicode data version and tailoring are explicit. Stage 5 remains scalar-safe and English/US-focused. |
| Display-cell width and monospace layout | Stage 6 shared terminal/text milestone before wider-script terminal claims | Combining and wide clusters need coherent cell, cursor, selection, wrapping, and fallback behavior. Current one-cell-per-scalar output remains the implementation until changed and tested. |
| Font fallback and glyph coverage | Stage 7 hardware survey, then Stage 11 desktop integration | Preserve text when a glyph cannot be rendered; distinguish missing glyph from invalid text. A fixed monospace console is not equivalent to full shaping. |
| Complex shaping and bidirectional ordering | Stage 11 desktop/text-stack design | Arabic, Indic scripts, and mixed-direction text need shaping and bidi-aware layout; do not claim support before integrated cursor and selection behavior exists. |
| Composition and input methods | Stage 7 input/hardware survey; Stage 11 desktop integration | Composition, preedit, candidate selection, and committed text are separate from key events. No complete IME is a Stage 5 prerequisite. |
| Multilingual and accessibility qualification | Stage 6 shared text tests, expanded on hardware in Stage 7 and integrated in Stage 11 | Include Arabic, Chinese, Japanese, Korean, Burmese, Thai, and Indic scripts, with explicit per-layer results rather than one binary “Unicode supported” claim. |

The milestone order is a dependency plan, not a promise that every listed
language feature is required before a usable Stage 11 desktop. Desktop
completion should state the supported text/input profile and preserve
unsupported content without corruption.

## Decisions still requiring review

- Foreground process-control behavior if terminal job control is introduced;
  current Shoot Ctrl+C/D/L behavior stays application-local and does not imply
  a system-wide Ctrl shortcut policy.
- The shared Unicode version, word-boundary tailoring, ambiguous/wide/emoji
  width policy, font-fallback behavior, and staged script support claims.
- Richer clipboard representation admission, larger-transfer alternatives,
  resource fairness, and desktop access policy. The initial UTF-8 API and
  incarnation resource budgets are implemented.
