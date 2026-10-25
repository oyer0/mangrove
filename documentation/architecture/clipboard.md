# Session clipboard

`/core/clipd` owns the volatile clipboard and is supervised by Sprout. The
`clipboard` endpoint is the seventh of eight registry slots. It has no service
privileges; its kernel-assigned service identity permits only read-only
`session_query`, not session enumeration, creation, launch, or termination.

## Identity and lifetime

The daemon uses the IPC receiver's kernel-authenticated PID, UID and session
ID. Requests contain no session selector. System services, processes without
an active human session, UID/session mismatches, and cross-process transfer
tokens are rejected. Different sessions have different items, even when the
same user owns both sessions. Each request rechecks authoritative session
liveness; periodic collection drops ended sessions and their transfers.

Publication transfers ownership to clipd. Exiting the source program does not
remove committed data. Logout removes that session's clipboard; the periodic
check runs every 250 monotonic milliseconds when the service is scheduled.
No data is saved to disk. A service stop, crash or restart loses volatile
contents. Sprout restarts clipd on failure, using the normal service backoff.
Existing endpoint handles stay dead and are never silently rebound.

Incomplete uploads and pinned reads have a 30-second inactivity lease, renewed
by each accepted transfer request. Explicit close releases them immediately;
a crashed client leaves bounded resources until expiry. There is no process
exit notification subscription or instantaneous orphan detection. A successful
publication is independent of that lease. Closing a transfer cannot retract a
committed item.

## Public API and representation model

`<mg/clipboard.h>` provides format discovery (`clipboard_query_format`, with
an indexed representation and total format count), negotiated reads by MIME
identifier, publication, clear, and explicit transfer close. `clipboard_query`
is the first-representation convenience query. The initial representation is
exactly `text/plain;charset=utf-8`. Unknown formats return `MG_ERR_UNSUPPORTED`.
An absent item returns `MG_ERR_NOT_FOUND`; zero-length text is a valid item.

`clipboard_set_text` publishes an explicit byte range. `clipboard_get_text`
allocates bytes plus a convenience trailing NUL and leaves its output parameters
unchanged on failure. Its caller supplies its own size limit and frees the
result. Embedded NUL, whitespace, newlines, and all valid Unicode scalars are
preserved. Malformed or incomplete UTF-8 is rejected; no normalization occurs.
The transfer APIs support large text without requiring a contiguous client
copy. C clients must close every successful begin, including on failure.
Transfer objects are single-threaded and own an endpoint handle.

The versioned protocol describes one item, representation identifiers, format
count/index, generation, transfer token, byte size and offsets. Version 1
publishes one UTF-8 representation. Future operations can stage additional
representations before the same atomic item publication; images, rich text and
file references are not implemented. There is no kernel clipboard syscall,
private editor clipboard, provider callback, shared-memory transfer, or history.

## Transport and atomicity

The existing 1,008-byte IPC payload carries a 104-byte control header and at
most 896 data bytes. Begin/query/commit/cancel and acknowledgments carry only
the header; data bytes appear only in write requests and read replies.
Data uses sequential offset-checked chunks. No bulk array is retained in a
kernel IPC record. Every operation is bounded; a client yields the endpoint
between chunks, so another client can query, read or publish during a large
transfer. There is at most one upload and one read per PID, and 32 transfers
across the daemon.

Uploads have private storage and incremental strict UTF-8 validation, including
scalars split across requests. Commit requires the exact declared byte count
and a complete scalar. It changes the session's committed reference in one
service-loop operation. Failed allocation, malformed data, incomplete commit,
cancellation and expiry leave the previous item unchanged. Concurrent writers
use last-successful-commit ordering. Reads pin an immutable item and continue
reading that snapshot across replacement or clear. They cannot cross sessions.
Tokens do not wrap into reuse within an endpoint incarnation.

A lost commit reply is ambiguous: publication may already have completed.
Clients report the transport error, never automatically replay the commit, and
close their transfer. Synchronous IPC has no independent client deadline: a
live daemon that stops making progress inherits the existing IPC blocking
contract. Transfer expiry is service policy, not a kernel watchdog. A failed IPC reply does not roll back an already
committed item. Applications must not perform destructive edits unless they
have received successful publication confirmation.

## Memory and recovery

Bulk storage consists of explicitly mapped 64 KiB blocks, including linkage.
It is not allocated through loam's malloc, whose mappings currently remain
until process exit. Committed items, read pins, incomplete uploads and retired
blocks all count against a daemon budget of half the physical free RAM sampled
at startup. A single item is limited to half that budget, leaving nominal room
for replacement. These resource limits are incarnation policy, not permanent
format limits or a universal 2 MiB maximum. Actual PMM, page-table, kernel
metadata, address-space or mapping exhaustion may reject an allocation earlier.
No data is silently truncated and existing contents are not evicted to admit a
new upload. One busy session can exhaust aggregate capacity; fairness quotas
are not implemented.

Each upload block is allocated only when needed. Reads and commit do not copy
the entire item. Superseded or cancelled storage enters an allocation-free
retirement list. Each service-loop iteration unmaps at most eight retired
blocks; timed receives keep reclamation moving when clients are idle. An
unexpected unmap failure ends the daemon so process teardown retains the
kernel's mapping ownership contract. Kernel page-table hierarchy frames may
remain until process exit. The fixed metadata tables and IPC queue can also
return `MG_ERR_BUSY` or transport queue errors.

The host fixture executes the production client API and daemon policy with
controlled IPC, session, mapping and failure adapters. Its 8 MiB round trip uses
18,730 requests and 8,454,144 mapped bulk bytes, with no second full service
copy. Compact control messages transfer 20,673,056 payload bytes for that
round trip, rather than 37,460,000 bytes with padded acknowledgments. Host CPU
timings do not establish native IPC throughput or low-memory hardware behavior. See [test procedures](../development/tests.md).

## Reed and qualification

Reed uses this public API for `c`/`x` motion operators, `cc`/`xx`, selection
copy/cut and `v` paste/replacement. It prepares its capacity and undo delta
before a cut publication, and mutates only after a confirmed commit. Paste
loads a complete pinned snapshot before one prepared range edit. Failures leave
text, selection, dirty state and undo/redo intact. See the [Reed guide](../editor/README.md).
Its rejection of NUL-bearing file text and in-memory editing requirements are
application limits, not clipboard limits.

`make test-guest-clipboard` builds source-matched binaries and runs one disposable
two-vCPU Q35/OVMF guest. Linux defaults to KVM; `GUEST_CLIPBOARD_ACCEL=tcg` is
explicit. It stages `/bin/cliptest` only on its private image. The native suite
verifies an 8 MiB UTF-8 item after its publisher exits, immutable reads during
concurrent replacement, cancellation, empty/absent text, and unsupported MIME.
USB events then drive copy/cut/undo in one Reed process and paste/undo/redo/save/
reopen in another. Framebuffer glyph observations, exact stopped-image file
bytes, child status checks, clean shutdown and an independent MGFS check are
separate evidence. Physical keyboards, physical hardware, native multi-session
logout, daemon restart and actual native PMM exhaustion are not qualified by
this workflow; controlled host tests cover their policy/failure boundaries.

In one bounded 512 MiB two-vCPU KVM run, the 8 MiB publisher's process start,
upload and exit took 1,080 ms; the subsequent read and byte verification in a
separate process took 392 ms. Timing uses the guest monotonic millisecond
clock. These are individual observations, not worst-case bounds or a physical
hardware qualification. Kernel, daemon, client test and Reed binary hashes are
recorded by the runner.
