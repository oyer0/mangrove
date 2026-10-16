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

Each process has a generation-tagged handle table. Handles name kernel objects
and carry explicit read/write rights; raw kernel pointers are never a
userspace object identity. Fresh programs receive separate standard output and
keyboard-input handles. Closing the last reference releases the underlying
object according to its object-type lifetime.

External termination fails with `MG_ERR_BUSY` while the target has a live
syscall frame, is in a scheduler handoff, or is executing on a CPU. It does not
unwind that frame or release process resources on this failure. A suspended
kernel thread with an entered C frame likewise cannot be externally discarded.
Self-exit still completes through the thread's normal termination path. A
session whose backend disappears stays visible in ENDING state if a member is
busy; an authorized replacement backend can retry ending the same session ID.
There is no forced cancellation of a syscall that remains blocked indefinitely.

Executables are 64-bit little-endian x86-64 `ET_EXEC` ELF files. The kernel
validates every load segment, maps it into a fresh lower-half address space,
creates an eight-page non-executable user stack, copies bounded `argc`/`argv`
data there, and enters Ring 3. Process exit closes handles and mappings, records
the status, and wakes a waiting parent.

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
process-handle write rights, copies at most 2 MiB into 4 KiB kernel chunks before mutation,
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
file exclusively with up to 2 MiB of complete initial contents. The kernel reserves an
inactive process handle slot and allocates its file object before mutation;
it copies all input into 4 KiB chunks. A confirmed result activates a new
read/write handle for the created inode. Existing names fail without being
overwritten. Uncertain publication or cleanup returns `MG_ERR_UNCERTAIN`
without a usable handle; callers must not retry automatically. The output
handle address must be writable when the syscall begins. Mangrove currently
supports one userspace thread per process, and external termination cannot
discard an active syscall frame, so that validated mapping remains stable
through the operation. These are runtime guarantees, not crash durability.
`MG_FILE_CONTENT_MAX_BYTES` is the 2 MiB serialized-content limit shared by
snapshot, replacement, and creation. Exactly 2 MiB is accepted; larger input
returns `MG_ERR_REPLACEMENT_TOO_LARGE`, while a snapshot capacity larger than
2 MiB is an invalid argument. Shoot line/history limits are separate.
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
retain their existing entry layout and ABI. A native name field accommodates
255 UTF-8 bytes plus NUL. Checked filesystem enumeration distinguishes one
entry, EOF, and errors; runtime child mounts use the same distinction.
Legacy internal boolean wrappers remain available, but native reads use the
checked callbacks provided by FAT32, exFAT, MGFS, and initramfs.

A valid filename that cannot be represented returns `MG_ERR_NAME_TOO_LONG`
(-37), consumes exactly that entry, and leaves the caller's output unchanged.
The next read continues normally. FAT32 still uses a portable ASCII short
alias when an imported LFN exceeds this capacity. Unsupported aliases without
a valid LFN retain the driver's existing omission policy.

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

This is an interim compatibility contract. A future length-aware native
enumeration and pathname interface must expose every valid on-disk Unicode
name without an arbitrary UTF-8 byte disadvantage. It is required before the
native desktop is considered complete. Enumeration does not promise a frozen
namespace across separate calls or detection of out-of-band disk changes.

## Checked pathname lookup

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
