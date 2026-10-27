# Processes, threads, handles, and syscalls

Mangrove separates execution scheduling from process ownership. A kernel
thread is the scheduler object; a process owns a userspace address space,
credentials, current directory, handles, executable state, and its main
thread. A newly spawned program begins with one main userspace thread.

## Process lifetime and inheritance

Processes have monotonic boot-local PIDs and active or terminated state. A
normal child inherits its parent's effective credentials, session membership,
current directory, console input, and current output object. System services
are instead spawned from the kernel service definition with a bounded service
privilege set. The parent receives a generation-tagged process handle and may
wait, poll, or terminate that child through the handle.

Boot constructs PID 1 before loading `/core/sprout`. If that construction
fails, the fatal boot diagnostic identifies the failed process-object,
handle-table, address-space, working-directory, or registry stage; a VFS
working-directory lookup failure also reports its VFS status. ELF loading is
reported separately and occurs only after a valid process/address-space pair
exists. This diagnostic does not retry or mask construction failures.

Each process has a generation-tagged handle table. Handles name kernel objects
and carry explicit read/write rights; raw kernel pointers are never a
userspace object identity. Fresh programs receive separate standard output and
keyboard-input handles. Closing the last reference releases the underlying
object according to its object-type lifetime. The table starts with 16 slots
and grows geometrically when full, allocating and copying outside the
interrupt-disabled slot lock. Allocation failure leaves existing handles
usable. The 32-bit handle ABI encodes a one-based slot index in 16 bits, so
65,535 slots per process is the format ceiling; actual capacity depends on
kernel memory and other live objects. Handle generation tags also remain
16-bit for ABI compatibility.

External termination fails with `MG_ERR_BUSY` while the target has a live
syscall frame, is in a scheduler handoff, or is executing on a CPU. It does not
unwind that frame or release process resources on this failure. A suspended
kernel thread with an entered C frame likewise cannot be externally discarded.
Self-exit still completes through the thread's normal termination path. A
session whose backend disappears stays visible in ENDING state if a member is
busy; an authorized replacement backend can retry ending the same session ID.
There is no forced cancellation of a syscall that remains blocked indefinitely.

## Foreground interruption

Ctrl+C is a cooperative foreground-process interruption, not a POSIX signal.
At a Shoot prompt, the line editor receives Ctrl+C as a key event and discards
the unfinished command line. While Shoot waits for a foreground child, the
kernel follows that session's authenticated parent/waiting-child chain and
sets a one-shot interrupt request on the deepest active child, then wakes its
main thread. Console reads, IPC waits, networking waits, and millisecond sleeps
observe the request and return `MG_ERR_INTERRUPTED`; interrupted network
operations retire any submitted request before returning. A program doing
nonblocking or CPU-only work observes the request only when it next enters an
interruptible operation. The request never terminates Shoot or another
session, and no live syscall frame is forcibly discarded. Applications must
handle interruption at their own cleanup boundary; Reed uses its ordinary
save/discard/cancel prompt before leaving a modified buffer.

Publishing `PROCESS_STATE_TERMINATED` starts exit cleanup; it does not authorize
collection. Cleanup can still own a running, ready, or blocked kernel frame.
Wait, poll, session retirement, and shutdown require
`scheduler_thread_exit_complete()`: terminal thread state, no running CPU,
no pending stack handoff, and no sleeping or preemption continuation.
`process_wait()` retains the process owner reference while waiting for this
condition; shutdown can interrupt that wait without collecting the child.
`process_poll()` returns `MG_ERR_WOULD_BLOCK` until exit is complete. A final
process destructor must successfully destroy its main thread before releasing
the address space; refusal is an invariant failure, not permission to continue
teardown.

## Orderly shutdown and reboot

The privileged `/bin/shutdown` and `/bin/reboot` commands request a kernel
power transition. After validating the ACPI operation, the kernel closes
process creation and requests orderly exit from active applications. Blocking
console reads, IPC waits, and child-process waits return
`MG_ERR_SHUTDOWN`; applications can run their normal cleanup path. Shoot uses
that result to save pending shell history and exit. Shutdown waits up to five
seconds for cooperative exit, then attempts safe process termination for up to
another five seconds. It also waits for already-terminated processes to finish
their final syscall and scheduler handoff. A process that cannot safely exit
within those bounds cancels the transition with a timeout; the kernel does not
discard a live syscall frame.

After applications exit, leaf services receive the same request and bounded
cleanup window. Each phase allows five seconds for cooperative exit and up to
five seconds for safe forced termination; if a live syscall or scheduler
handoff remains, shutdown reports timeout instead of destroying its state.
Sprout and the session-control services remain alive as the shutdown control
plane. Sprout suppresses service restarts during this period;
if cleanup or filesystem synchronization cancels the transition, it resumes
supervision. Logind waits without starting another login while a transition is
pending.

Before the ACPI power-off or reset operation, the kernel synchronizes every
mounted filesystem and flushes each backing block device. VFS lock acquisition
uses a shared ten-second deadline; filesystem and device operations retain
their own bounded timeout and error behavior. A synchronization error cancels
power-off/reboot and reopens process creation. Services stopped
before a failed transition can then be restarted by Sprout. This is an orderly
runtime shutdown contract; it does not make every filesystem mutation
crash-atomic or guarantee recovery from power loss during a failing device
flush. Panic and abrupt hardware reset do not run this lifecycle. Reed's
unsaved editor buffers are not saved automatically; only an application's
explicit save operation establishes its existing safe-save guarantees.

The commands print `Shutting down...` or `Restarting...` before requesting the
transition. On the normal path, the kernel emits no subsystem-stop, sync,
unmount, or ACPI progress messages. A forced-termination warning and any
process-exit, filesystem-sync, or power-transition error remain visible; the
commands also report a returned failure. The kernel does not delay the final
power operation to preserve screen output.

Executables are 64-bit little-endian x86-64 `ET_EXEC` ELF files. The kernel
validates every load segment, maps it into a fresh lower-half address space,
creates an eight-page non-executable user stack, installs a separate owned
argument mapping, and enters Ring 3. Process exit closes handles and mappings, records
the status, and wakes a waiting parent.

Native argv requests use a per-spawn **8 MiB packed argument budget**, defined
in `include/mg/process_limits.h`. It includes all 64-bit argument pointers, the
NULL sentinel, and every string's terminating NUL. Argument count is derived
from that budget (at most 932,066 entries with minimal strings), rather than a
separate small vector limit. This is a kernel admission policy that bounds
per-call staging and child backing; it does not add a fixed-size syscall record.
Actual capacity depends on available kernel heap,
physical frames, page tables, and process/handle resources. Zero `argc`, a NULL
vector, and NULL entries are invalid; empty strings after the executable are
valid. The executable must be nonempty and obey the ordinary path limit.

Syscalls 64 and 96 retain their original vector/count/output-handle calling
conventions. The kernel validates the count and address arithmetic before
allocation, snapshots each pointer once, then captures string bytes into
4 KiB heap chunks after checking each user page. It scans captured user bytes
for terminators, avoiding a second read of mutable user strings. Invalid input
returns `MG_ERR_BAD_ARGUMENT`, an over-budget vector returns
`MG_ERR_BUFFER_TOO_SMALL`, and staging allocation failure returns
`MG_ERR_NO_MEMORY`. No partial argument snapshot reaches child creation.

The child receives a writable, non-executable argument mapping in a reserved
8 MiB region below its stack, separated from it by a guard page. Only the
needed pages are allocated, zeroed and populated; ELF segments cannot overlap
this region or the stack. `argc` and `argv` still enter in RDI and RSI, with
`argv[argc] == NULL` and a 16-byte-aligned RSP. The execution stack remains eight
pages; large arguments do not consume it. Even a small request uses one
separate 4 KiB argument page. Mapping failure rolls back all
argument pages before aborting the suspended child. A successful child's
address space owns these frames until ordinary process destruction. No kernel
staging pointer escapes to userspace, and the parent snapshot is released
when synchronous spawn returns. The returned process-handle value is published only
after successful scheduling admission.

The kernel copies string data once into staging and once into the child's
frames, without assembling another full contiguous string buffer. At the
8 MiB admission boundary, the host fixture measures 8,421,392 bytes of kernel
staging (including chunk headers and rounding), plus at most 8 MiB of child
argument pages and page-table overhead. These are bounded test measurements,
not a guarantee that a pressured system can admit the maximum request.

The legacy command-line spawn syscalls retain their 256-byte input bound and
historical quote parser for binary compatibility. They reject unterminated
quotes and do not silently stop at sixteen arguments. No in-tree userspace
application uses this form. Session shell creation and Sprout-supervised
service launches, as well as initial Sprout argument setup, pass kernel-owned
argv vectors into the same argument capture and installation path used by the
public argv interfaces, so those paths do not inherit the legacy line bound or
reparse a command string. New callers should use argv so
argument boundaries, long strings, and UTF-8 bytes are explicit. Existing
binaries using either syscall remain compatible with the historical bound.

`process_spawn_argv()` copies a bounded argument vector and inherits the
caller's current standard output. `process_spawn_argv_with_output()` accepts a
writable file or console handle for the child output without changing the parent's
output slot. The kernel validates the handle type and write right and retains the
child reference during creation. A caller logging to a file can explicitly select
its writable stdin console handle for an interactive child; ordinary spawn still
inherits its stdout destination. This direct form supports nested command
redirection while preserving argument boundaries; the parent may close its handle
after a successful spawn.
It uses additive syscall 96; the existing argv syscall remains unchanged.

## Working-directory identity and path resolution

A process retains a reference-counted directory location and its mount
incarnation, rather than using a pathname as authority. Child inheritance
retains the same location. Exit and explicit `chdir` release the old binding;
a clean unmount is BUSY while a CWD, file or directory handle retains its mount.
An inherited removed CWD stays removed: relative lookup, creation, rename,
executable loading and `getcwd` return DEVICE_GONE. Absolute paths remain
available for recovery, and an explicit successful `chdir` establishes a new
binding. Mounting replacement media at the same displayed path never revives
an old CWD or handle.

Resolution walks retained directory identities under namespace admission,
including actual `..` ancestry and mounted-root crossings. It checks traversal
permissions using the target process credentials, including when loading a
child executable. Namespace syscalls retain resolved parents across permission
confirmation and revalidate identity before mutation; they do not check an
anchor and then repeat an unrestricted global pathname lookup.

Supported directory renames update the shared location graph after confirmed
filesystem publication and pin completion. Existing CWDs follow the moved
directory; `getcwd` renders its current ancestry. Removing a directory retained
as any process CWD, or one containing a mount, returns BUSY. An ancestry cycle
is rejected before mutation. A `getcwd` buffer that is too small is unchanged
and receives the required size, including the terminating NUL. These rules do
not introduce new Unicode pathname capacity or persistent directory handles.

Command utilities preserve relative pathname authority and pass components to
the kernel unchanged; they do not turn `getcwd` into an absolute lookup or
lexically erase `missing/..`. Explicit executable paths containing `/` remain
absolute or relative as supplied; bare command names retain `/bin` lookup.
Home expansion remains an explicit absolute user-home selection. Reed safe
creation accepts relative paths through the same native anchored interface.

## Native syscall ABI

Userspace invokes the x86-64 `SYSCALL` instruction through `mg_syscall`:

```text
mg_syscall(number, arg0, arg1, arg2)
RAX = number
RDI = arg0
RSI = arg1
RDX = arg2
RAX = result
```

The complete numeric ABI is the `enum syscall_number` in
`kernel/include/syscall.h`; number zero is reserved. Public libc wrappers and
their request structures are the userspace-facing interface. Results use
Mangrove result codes or operation-specific nonnegative values.

The native `mg_random_status()` and `mg_random_bytes()` APIs use syscalls 100
and 101. The byte-fill syscall validates the complete writable destination
before writing and propagates `MG_ERR_ENTROPY_UNAVAILABLE` if the kernel CSPRNG
cannot initialize or reseed. See [cryptographic random bytes](randomness.md)
for source assumptions, output error semantics, and verification limits.

`object_write()` uses syscall 5 and `object_write_literal()` uses additive
syscall 103. Writes to non-console objects, including redirected standard
output files, preserve the supplied bytes in either case. Console writes
through syscall 5 recognize Mangrove's bounded ESC `{...}` terminal commands;
syscall 103 bypasses that parser and is the required path for displaying
untrusted file, network, or log bytes. See the
[terminal control contract](terminal.md) for the command grammar and process
ownership rules.

On entry the assembly path saves the user return state and general registers,
switches to the current thread's kernel stack, and calls `syscall_dispatch`.
The kernel masks interrupt, trap, and direction flags during entry and returns
with `SYSRET`. A syscall may block because every schedulable thread has its own
kernel stack and saved syscall frame.

Pointer arguments are not trusted. Dispatch validates mapped user ranges and
copies bounded strings or request records before using them. Kernel objects
are resolved through the caller's handle table with required type and rights.
Identity, service, session, IPC, network, volume, and storage operations apply
their additional authenticated policy in the kernel rather than accepting
client-supplied authority.

Each process currently has one userspace thread. Its address space cannot be
changed by that process while its syscall is executing, and external process
termination/reaping waits for the active syscall/thread lifetime. Thus a
successful mapping check remains stable for the current syscall access model;
user pages are not generally pinned for every syscall. Any future support for
multiple userspace threads in one address space must add a mapping-lifetime
contract before allowing concurrent unmap or protection changes.

## Contract boundaries

The syscall number table and the layouts in public `libc/include/mg/` headers
must change in step with kernel dispatch and libc wrappers. Internal process,
thread, page-table, and object layouts are implementation details rather than
userspace ABI.

`file_append_lines()` (syscall 87) merges bounded newline-terminated records
with the latest contents of a readable/writable file and retains the requested
number of lines. Optional result metadata identifies the saved incoming range.
`file_snapshot()` (syscall 88) returns bounded contents and identifier/revision
metadata without changing the handle offset. A complete snapshot on a
supported filesystem also binds that record identity and revision to the open handle's next conditional
save expectation. Failed snapshots preserve the prior expectation. Snapshot and
replacement on one handle share its offset mutex and the superblock operation
lock. MGFS implements each complete operation under that superblock lock, using
the persistent Record size.
Unsupported filesystems return an error. The reset flag explicitly replaces
previous contents, including replacing a file with no entries.

`file_replace()` (syscall 89) conditionally replaces an existing MGFS 1.2,
FAT32, or exFAT file through its writable open handle. The kernel checks
process-handle write rights, copies the complete input into 4 KiB kernel chunks before mutation,
and passes those chunks to VFS for generation and current-permission checks.
It preserves the handle offset. Stale generation, oversized input, filesystem
no-space, device removal, and uncertain mutation have distinct native errors.
Successful replacement advances the handle's saved generation, allowing
another save through the same handle. A stale-generation failure does not
refresh the saved generation; a later complete snapshot can bind a new one.
Path metadata is copied under the owning superblock operation lock so a size
update and `path_info()` cannot observe opposite sides of one mutation.

FAT32 and exFAT generations are mount-local and do not detect external media
changes or survive remount; MGFS generations are stored in its file records.
Filesystem-specific layout restrictions can return an unsupported error. exFAT
content/length and namespace mutations requiring timestamps return
`MG_ERR_TIME_UNAVAILABLE` before media changes if validated UTC is unavailable
or outside 1980–2107. Ordinary writes preserve that error through object/syscall
translation; it is distinct from unsupported operations and uncertain mutation.
See [exFAT metadata](../filesystem/exfat/README.md#timestamps-and-allocation-percentage).


`file_create_contents()` (syscall 90) creates an MGFS 1.2, FAT32, or exFAT
file exclusively with complete initial contents. The kernel reserves an
inactive process handle slot and allocates its file object before mutation;
it copies all input into 4 KiB chunks. A confirmed result activates a new
read/write handle for the created inode. Existing names fail without being
overwritten. Uncertain publication or cleanup returns `MG_ERR_UNCERTAIN`
without a usable handle; callers must not retry automatically. The output
handle address must be writable when the syscall begins. Mangrove currently
supports one userspace thread per process, and external termination cannot
discard an active syscall frame, so that validated mapping remains stable
through the operation. These are runtime guarantees, not crash durability.
Safe-file contents have no fixed 2 MiB admission ceiling. Lengths and snapshot
capacities must fit `MG_FILE_CONTENT_ADDRESS_MAX`, the signed address-arithmetic
range. The old `MG_FILE_CONTENT_MAX_BYTES` and `MG_FILE_REPLACE_MAX_BYTES` names
remain deprecated source-compatibility constants for old bounded callers; they
are not transaction limits. FAT32 retains its on-disk 32-bit byte-size limit;
MGFS and exFAT admit lengths against mounted geometry. Insufficient staging
memory, free storage, or ownership-proof resources fails explicitly. All input
staging is acquired before any persistent mutation, preserving immutable input
even if the original userspace bytes change. Namespace/recovery bounds remain
filesystem-specific; see [storage lifecycle](../storage/lifecycle.md).
Shoot line/history limits are separate.
Snapshot copies through a 4 KiB kernel buffer under the operation lock. A
failed copyout may leave partial bytes in the caller's data buffer, but does
not update the result state or the handle's conditional-save expectation.

Serialization does not provide crash atomicity. The line-mutation API reports
`MG_ERR_UNCERTAIN` when a mutating callback fails, since data or the Record may
already have changed. Callers must not blindly repeat that append. Errors before
mutation remain ordinary errors. Snapshots validate their capacity before copying
and expose no kernel pointers. Appends retain at most 4096 records and validate
incoming records to at most 511 text bytes plus newline; snapshot buffers are
capped at 2 MiB independently of those line limits.

## Authoritative code

- `kernel/include/syscall.h` and `kernel/src/syscall.c`
- `kernel/src/syscall_entry.s` and `libc/src/mangrove_syscall.s`
- `kernel/include/process.h` and `kernel/src/process.c`
- `kernel/include/object.h` and `kernel/src/object.c`
- `kernel/src/elf_loader.c`, `kernel/src/scheduler.c`, and
  `libc/include/mg/`

## Checked directory enumeration

`directory_read()` (syscall 15) and `directory_read_batch()` (syscall 31)
retain their existing 256-byte entry layout and ABI for existing binaries.
Additive `directory_read_ex()` and `directory_read_batch_ex()` calls (syscalls
98 and 99) use `mg_directory_entry_ex_t`, whose name field accommodates the
765 UTF-8 bytes needed to represent any valid 255-code-unit FAT32 or exFAT
name. MGFS continues to enforce its own 255-byte on-disk limit. Checked
filesystem enumeration distinguishes one entry, EOF, and errors; runtime child
mounts use the same distinction. Legacy internal boolean wrappers remain
available, but native reads use the checked callbacks provided by FAT32,
exFAT, MGFS, and initramfs.

A valid filename that cannot be represented by a selected ABI returns
`MG_ERR_NAME_TOO_LONG` (-37), consumes exactly that entry, and leaves the
caller's output unchanged. The next read continues normally. The legacy calls
can still return this result for a name longer than 255 UTF-8 bytes; callers
that must expose all supported FAT32/exFAT names use the `_ex` calls. FAT32
uses a portable ASCII short alias only when an imported LFN is not representable
by the extended interface. Unsupported aliases without a valid LFN retain the
driver's existing omission policy.

A batch returns completed entries before a following error. The directory
object stores that error for the next single or batch read, which returns it
without reading another entry. Errors other than name capacity do not consume
an unreported entry. Both APIs preserve output on an error; libc also preserves
`out_count`. EOF remains `MG_ERR_END_OF_FILE`, while unsupported operations,
I/O, cancellation, device loss, and uncertainty retain separate results.
Corruption maps to `MG_ERR_IO` because the native ABI has no distinct corruption
code. Listing prints a capacity warning and continues; completion skips only
the consumed unrepresentable entry and stops on other errors.

One directory-object mutex serializes the complete batch, cursor, and deferred
error. Filesystem reads hold the mount operation lock; callbacks never run
under the VFS metadata spinlock. Syscalls retain the directory object across
concurrent handle closure. The object is 112 requested bytes on x86-64 and
needs no additional heap allocation per read for deferred errors. Indexed
callbacks still have a 32-bit position; crossing that boundary returns
`MG_ERR_BUFFER_TOO_SMALL` rather than wrapping.

Enumeration does not promise a frozen namespace across separate calls or
detection of out-of-band disk changes. The old ABI remains available for
compatibility; new applications should use the extended calls.

## Checked pathname lookup

Native pathname arguments are NUL-terminated UTF-8 byte strings. The syscall
boundary reads at most `MG_PATH_MAX_BYTES + 1` bytes, copies the exact string
into kernel-owned storage, and rejects an empty, inaccessible, unterminated,
or overlong input; it never truncates a path. `MG_PATH_MAX_BYTES` is 65,535
bytes before NUL and counts the complete pathname, including separators. This
is a bounded ABI/resource limit, not a filesystem-format limit, scalar count,
or display-cell limit. Path copying is dynamically allocated, and VFS parsing
uses component-sized storage rather than a path-sized stack buffer. Components
may use up to 765 UTF-8 bytes, the maximum encoding of 255 UTF-16 code units;
MGFS retains its separate 255-byte component format limit. The VFS validates
strict UTF-8 before component lookup; malformed sequences return
`MG_ERR_BAD_ARGUMENT` rather than being reported as a missing name.

The public pathname ABI does not carry a separate input length. Kernel VFS
`*_n` helpers accept an exact byte length for already-owned data, reject
embedded NUL bytes, and do not read past that length. C-string wrappers first
find a terminator within the same 65,535-byte bound. The first NUL therefore
ends a public C pathname; bytes after it are not part of that argument.

An empty pathname is invalid. `/` names the process root; repeated separators
are treated as one. Root-only spellings are valid for lookup but have no final component for
create, remove, or move. `.` names the current directory and `..` follows the
retained directory ancestry, with the root parent clamped to root. Pathname
operations are relative to the process CWD; directory handles do not provide a
separate relative-path lookup interface. Relative
operations start from the process's identity-bound CWD. A trailing slash
requires the final existing object to be a directory. Directory creation may
use a trailing slash; regular-file creation may not. Removal with a trailing
slash cannot remove a regular file. Move requires a directory source when its
source ends in `/`; its no-replacement destination is still a final target
name, so a trailing-slash destination must already name a directory and then
fails with the existing destination-collision result. Absolute operations
remain rooted independently of a removed CWD; relative operations from that
CWD return `DEVICE_GONE`.

The retained CWD is not limited by the single-call pathname budget: repeated
relative `chdir` operations can create a longer ancestry. `getcwd` reports its
required byte capacity and can return such a path, but that full spelling may
not fit in one later pathname syscall. Relative operations remain anchored to
the retained directory identity.

Persistent filesystem lookup returns a status separately from its node result.
Only a completed search may report NOT_FOUND. Metadata I/O, allocation failure,
malformed layout, cancellation, unresolved mutation and device loss remain
errors through global and identity-anchored resolution and native syscall
translation. The native ABI is unchanged.

VFS mutation authorization, CWD/mounted-child BUSY checks, rename identity
checks and mountpoint admission must stop on lookup errors. Account and network
configuration creation likewise require confirmed absence before creating.
A pre-mutation lookup error performs no media write and acquires no recovery
pin; it does not by itself establish persistent uncertainty. MGFS, FAT32,
exFAT and initramfs implement checked lookup. Legacy pointer-only kernel
wrappers remain for callers that explicitly accept loss of error status;
namespace mutation and public pathname operations use the checked contract.

## Checked reads and recovery inspection

Filesystem checked reads return both status and a confirmed byte count. VFS
advances the offset by exactly the confirmed prefix. Native `read` preserves
the existing ABI: a positive count reports that prefix; zero is genuine EOF
(or a zero-length request); an error with no confirmed bytes is negative.
Online I/O failure and DEVICE_GONE are distinct from EOF. A retry at the
advanced offset can expose a persistent error after a partial read. Executable
and configuration loaders require a successful complete checked read and do
not parse an error prefix as complete input. Native process launch preserves
loader IO, DEVICE_GONE and UNCERTAIN errors; malformed ELF remains INVALID_EXEC. Legacy count-only kernel wrappers
remain for callers that explicitly accept that loss of status.

`recovery_owners_snapshot()` (syscall 91) is read-only and requires the live
MANAGE_STORAGE privilege. Ordinary user credentials cannot inspect owners.
The authorized storage service can inspect them without acquiring mutation
rights or a confirmation bypass. A complete snapshot contains at most 32
records of 32 bytes each: owner ID, selected registration ID, physical-parent
ID, ACTIVE/UNRESOLVED state and online/offline-retained flags. Too-small capacity
returns the required count without changing entries; overlapping output ranges
are rejected. No kernel pointers,
transaction images, release controls or repair operations are exposed.
Registry membership is coherent; liveness is an observation, not a freeze of
hardware or authority to perform a later management operation.
