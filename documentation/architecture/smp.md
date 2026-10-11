# SMP architecture

Pith uses ACPI CPU topology and xAPIC startup to run kernel and userspace
threads on the online x86-64 CPUs. The implementation is bounded and uses one
global scheduler queue set; it does not yet provide topology-specific policy.

## CPU discovery

The ACPI MADT parser records usable Processor Local APIC entries and filters
entries that are neither enabled nor online-capable. Each record retains its
ACPI processor ID and 8-bit xAPIC ID. Pith assigns stable CPU indices without
using APIC IDs as array indices; the BSP is index zero and the current bound is
`CPU_MAX_COUNT` (256 records). The LAPIC ID is matched back to the MADT record
to identify the BSP.

Processor Local x2APIC entries are recognized and counted, but x2APIC mode is
not supported. Usable x2APIC processors are reported as unsupported rather
than being treated as ordinary xAPIC CPUs.

## AP startup

The PMM chooses a page from firmware-reported usable conventional memory below
the VGA aperture and Pith reserves it before use. The page is temporarily
identity-mapped for the startup trampoline and is addressed by its SIPI
vector. Pith starts APs one at a time with a serialized low-memory mailbox,
INIT, and one or two SIPIs. Each AP receives an independent startup/idle stack,
loads its per-CPU descriptor state and shared IDT, initializes its local LAPIC
timer, and publishes an online acknowledgement. Delivery and online waits are
bounded; a failed AP is reported and does not prevent the rest of the system
from continuing with the CPUs that came online.

With one discovered CPU, no AP startup is attempted. The BSP still initializes
its local scheduler timer.

## Per-CPU state

`cpu_local_t` is addressed through GS in kernel context. User-to-kernel
transitions use `swapgs`; kernel-origin interrupts do not swap GS
unconditionally. Every online CPU has its own GDT, TSS, kernel `rsp0`,
emergency IST stack, current thread, and current page-table root. The IDT is a
shared immutable table, and syscall MSRs are initialized for each CPU.

## Scheduling

The scheduler retains one global set of priority-ordered ready queues and one
global scheduler spinlock. Current and idle threads, preemption state, and
context-switch state are CPU-local. Each CPU owns an idle thread that is not
placed in the global ready queues. A thread's `running_cpu` field serializes
running ownership so a runnable thread cannot be selected simultaneously by
two CPUs.

Kernel threads and process-backed userspace threads can run on any online CPU;
the initial bootstrap context remains a BSP-only special case. Threads may
migrate while not running. There are no CPU-affinity controls or per-CPU run
queues.

Each CPU has a local LAPIC scheduler timer. The BSP's PIT path remains the
global scheduler timekeeping source and wakes sleeping threads; local LAPIC
timer interrupts provide per-CPU preemption without multiplying wall-clock
time by the CPU count.

## Interrupt policy

External device interrupts remain routed to the BSP, including the current
PIT/PS2 and device paths. Local LAPIC timers are handled independently on each
CPU. xAPIC fixed IPIs are used for AP startup and TLB shootdowns. The TLB
shootdown handler has a dedicated interrupt vector and reloads CR3 on CPUs
that are using the affected address space.

## Memory synchronization

Each process address-space metadata record contains an active-CPU mask indexed
by stable CPU index. Switching address spaces marks the incoming CPU active and
clears its previous address-space bit while the hardware CR3 update and
bookkeeping are serialized.

User page-table changes capture the active target CPUs, publish a bounded
shootdown request, send fixed IPIs, and wait for acknowledgements. Required
shootdown failure is fatal rather than silently continuing with potentially
stale translations. Shared kernel mappings are propagated to process roots and
invalidated on other online CPUs.

Destruction sets `destroy_pending`, rejects new switches into the address
space, waits for the active-CPU mask to empty through scheduler-backed bounded
sleep/retry, then detaches and frees owned page-table frames. Process reaping
also waits until the relevant thread is no longer running.

## Kernel synchronization

IRQ-safe spinlocks protect short, nonblocking shared-state critical sections.
Scheduler-backed non-recursive mutexes are used for operations that may sleep,
including serialized VFS/filesystem operations. The VFS metadata lock is not
held across filesystem or block I/O; mounted filesystems use a coarse
per-superblock operation mutex and open-file offsets have their own
synchronization.

## Current limitations

The current implementation does not provide:

- x2APIC mode;
- NUMA support;
- topology- or SMT-aware scheduling;
- CPU affinity;
- CPU hotplug;
- per-CPU run queues;
- advanced interrupt steering; or
- topology-aware load balancing.

These are deferred capabilities, not requirements for the current bounded SMP
model.

## Authoritative code

- `kernel/src/acpi.c` and `kernel/include/acpi.h` — MADT topology;
- `kernel/src/cpu.c` and `kernel/include/cpu.h` — CPU-local records;
- `kernel/src/smp.c`, `kernel/src/smp_trampoline.s`, and `kernel/include/smp.h`
  — AP startup;
- `kernel/src/gdt.c`, `kernel/src/idt.c`, and `kernel/src/syscall_entry.s` —
  per-CPU descriptor/syscall entry state;
- `kernel/src/scheduler.c` — scheduling and ownership;
- `kernel/src/lapic.c` and `kernel/src/timer.c` — local timers and IPIs; and
- `kernel/src/vmm.c` — address-space tracking and TLB shootdowns.
