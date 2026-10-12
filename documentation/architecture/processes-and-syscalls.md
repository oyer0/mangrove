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
Filesystem-specific layout restrictions can return an unsupported error.

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
