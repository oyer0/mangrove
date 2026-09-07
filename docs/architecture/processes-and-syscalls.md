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

## Authoritative code

- `kernel/include/syscall.h` and `kernel/src/syscall.c`
- `kernel/src/syscall_entry.s` and `libc/src/mangrove_syscall.s`
- `kernel/include/process.h` and `kernel/src/process.c`
- `kernel/include/object.h` and `kernel/src/object.c`
- `kernel/src/elf_loader.c`, `kernel/src/scheduler.c`, and
  `libc/include/mg/`
