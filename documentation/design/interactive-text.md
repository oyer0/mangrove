# Interactive keyboard, text, and clipboard design

This is a planning document. It records accepted architectural direction and
proposals for Stage 5; it does not change the current kernel, terminal, Shoot,
Reed, or loam contracts. Implemented behavior remains described by the
[terminal](../architecture/terminal.md), [Reed](../editor/README.md),
[Shoot](../shell/interactive.md), and [IPC/service](../architecture/ipc-services.md)
documentation.

## Status and design goals

The immediate Stage 5 goal is a reliable English/US-keyboard console and
consistent interaction between Shoot and Reed. The project direction is to
keep text and input interfaces capable of growing beyond that baseline without
corrupting text or tying applications to kernel-specific shortcuts.

The following are accepted planning decisions:

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

Specific event structures, service messages, key assignments beyond the
navigation direction above, size limits, and restart policy remain proposals.
Do not treat them as approved ABI or implementation requirements until their
review is complete.

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
and currently has Normal, Insert, Command, and Search modes. It has no
selection mode. Its normal map binds `h/j/k/l`, `w`, `b`, Alt-word variants,
search controls, and Page Up/Down. Arrow/Home/End handling is currently in
insert and prompt modes, not normal mode. See the [current Reed guide](../editor/README.md).

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

Reed's implemented search repeat is `n` forward and Alt+`n` backward. Do not
infer `n`/`N` search behavior from older documentation if the keymap is changed
later; any remap must be reviewed separately.

## Stage 5 keyboard and editing direction

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

Application keymaps should translate key events in userspace. The transition
must preserve existing working ordinary-input actions and avoid exposing
escape-prefixed editor packets to direct readers such as login. Ctrl behavior
must be defined at the terminal/process layer before applications claim Ctrl
combinations. Alt is the primary application-command modifier, but its exact
layout and behavior on non-US keyboards need a later input-layout design.

### Reed navigation

The intended Normal and Selection navigation is:

| Key | Action |
| --- | --- |
| Arrows / `h j k l` | Character and line movement. Vertical movement retains a preferred display column. |
| `b` / `n` | Previous / next word start. |
| `B` / `N` | Previous / next word end. |
| Home / End | Start / end of logical line. |
| Ctrl+Home / Ctrl+End | Start / end of document, subject to terminal-control keymap review. |
| Page Up / Page Down | Move by one text viewport while retaining preferred display column. |

The agreed word-motion rule is that a command starting strictly inside a word
must move to a different word in the requested direction, not merely to the
other boundary of the containing word. Stage 5 is English/US-first, so its
initial classifier can use ASCII letters, digits, and underscore, with each
non-ASCII scalar conservatively treated as a word character. That preserves
the current broad behavior without claiming Unicode word segmentation. The
later shared text layer should replace that classifier with a pinned Unicode
word-boundary implementation. The motion semantics are:

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
- `b/n` target word starts; `B/N` target the exclusive byte boundary after a
  word. From inside a word, that entire word is excluded as a target. From
  separators, choose the nearest boundary strictly in the requested direction.
  There is no wrap at document edges; an unavailable target leaves the caret
  unchanged.
- Empty lines and final-newline state affect line navigation, not word
  classification.

The exact Unicode word-boundary version and whether word motions advance over
emoji as separate word-like units remain design questions for the shared text
specification. Stage 5's scalar-based selection and navigation must not be
described as grapheme-aware.

### Selection and common operations

Proposed modes are Normal, Insert, and Selection, alongside Reed's existing
command/search prompts. Candidate entry keys are `v` for character selection
and `V` for line selection; these assignments are not approved. Character
selection fixes an anchor and moves an active caret. Line selection covers
complete logical lines and includes a following newline when present, but does
not synthesize a final newline. Escape cancels selection. Movement keys extend
selection in Selection mode. Typing or entering Insert mode over a selection
would replace it; Backspace/Delete would delete it. Exact mode transitions
remain subject to review.

Candidate command chords use Alt to follow Mangrove's modifier direction:
Alt+C/X/V for copy/cut/paste, Alt+S for the existing save, Alt+Shift+S for
Save As, Alt+Q for the existing quit flow, and Alt+F for search. Existing `u`
and Shift+U remain undo/redo, with Alt+Z/Y possible aliases. These candidates
are not approved bindings. The existing search repeat Alt+`n` conflicts with
the intended `N` word motion and must be remapped if that navigation map is
accepted. Document-start/end also need a reviewed binding because the earlier
Ctrl+Home/End proposal competes with the planned Ctrl terminal/control role.

Copy leaves the document unchanged. Cut publishes the selected content before
deleting it; a failed copy leaves the document untouched. Paste obtains and
validates the complete representation before changing the buffer. Replacement,
cut, and paste should each form one undo transaction. Undo/redo restore both
text and dirty-state semantics. Save conflicts and quit confirmation continue
to use Reed's existing conditional safe-save contract.

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

The initial client-facing operation may support only `text/plain; charset=utf-8`.
That first format does not constrain the item model to text. There is no
permanent universal 2 MiB clipboard maximum. Initial per-representation and
aggregate resource limits must be explicit, bounded, and revisable. Large data
may later need a streaming or object-transfer mechanism rather than copying it
through small IPC messages.

### Interfaces to design

The public loam interface should describe clipboard items, MIME representations,
format discovery, and a transfer handle or generation. It should offer the
common text convenience operations without making text the only protocol form:

- query available representations and their bounded metadata;
- publish a complete item, with one or more offered representations;
- request a preferred representation and read it through a bounded transfer;
- clear the current session item;
- abort a failed publication or transfer without changing the visible item.

The service derives caller/session identity from authenticated IPC requester
metadata. Payload fields cannot claim another user or session. No new kernel
clipboard syscall is currently justified; use the service/IPC boundary.
Current IPC carries at most 1008 bytes per message, so chunking is a likely
initial transport detail. The transaction protocol, generation behavior,
quotas, public C types, service name, logout notification, and service-restart
semantics are intentionally not frozen here.

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

- Whether search repeat moves from `n`/Alt+`n` to another pair so `n/N` can
  serve word motion; candidate bindings must not be treated as approved.
- Which Reed actions use Alt and which existing plain-key bindings remain as
  aliases. Alt is the primary command modifier, but exact assignments need a
  conflict review against current modes and non-US layout behavior.
- Whether line selection includes a following newline exactly as proposed and
  how empty final lines are represented.
- Exact Ctrl behavior for terminal cancellation, foreground process control,
  and application events; Shoot must keep control semantics distinct from
  clipboard actions.
- The shared Unicode version, word-boundary tailoring, ambiguous/wide/emoji
  width policy, font-fallback behavior, and staged script support claims.
- Initial clipboard per-item and aggregate limits, service restart/loss policy,
  session lifecycle integration, and the public MIME/transfer API.
