# Userspace synchronization

Mangrove provides C17 scalar atomics, a native process-local wait/wake
facility, and native workers through `<mg/thread.h>`. These interfaces support
coordination between concurrently executing contexts; they are not a POSIX
threads layer.

## C17 atomics

`<stdatomic.h>` uses the native GCC `__atomic` compiler intrinsics. Naturally
aligned integer, boolean, and pointer scalar atomics up to 64 bits are
lock-free on Mangrove's x86-64 target. The header supplies C17 memory orders,
load/store, exchange, compare-and-exchange, integer read-modify-write,
pointer `atomic_fetch_add`/`atomic_fetch_sub` with C element-scaled pointer
arithmetic, `atomic_flag`, and thread/signal fences. Fetch operations return
the prior value and evaluate each argument once. These operations link without
`libatomic`. Larger or aggregate atomic objects are outside the supported
contract; native links intentionally do not supply an atomic helper runtime.
Atomic objects must meet their compiler-provided natural alignment. Pointer
fetch operations use compare/exchange with the requested success order and
the corresponding legal failure order; internal captures evaluate that order
once.

Acquire/release operations provide the normal publication relationship. For
example, a producer stores shared data and then publishes a state with
release ordering; a consumer that observes that state with acquire ordering
may then read the published data. `memory_order_relaxed` only orders the
atomic object itself.

## Address-keyed wait and wake

`<mg/sync.h>` provides `mg_sync_wait_u32()` and `mg_sync_wake_u32()` for a
naturally aligned 32-bit atomic word. A wait is keyed by the caller's address
space and virtual address, so equal virtual addresses in separate processes
are independent. Shared-memory synchronization between processes is not
provided.

The wait syscall obtains a VMM read lease, then under the wait-queue lock
compares the word with the expected value and registers the current thread as
one indivisible operation relative to wake. A value mismatch returns
`MG_ERR_WOULD_BLOCK` without sleeping. The lease remains active until the
waiter has been removed from the queue and the syscall is finishing. Thus
unmap or protection changes that affect the page fail while a wait is active;
address-space destruction also waits for the lease. The wake syscall holds a
short read lease while it selects waiters. No VMM or queue lock is held while
the caller is scheduler-blocked.

Wait deadlines are relative milliseconds measured against Mangrove monotonic
time. `MG_SYNC_WAIT_FOREVER` requests an indefinite wait. A wake selects up to
the requested number of matching waiters; `MG_SYNC_WAKE_ALL` selects all.
Wake returns the number selected, including zero. A successful wait returns
only after a wake and an acquire read observes a value different from the
expected value; a wake that finds the word unchanged atomically re-registers
the waiter. Kernel scheduler events may cause an internal spurious reschedule,
but do not by themselves complete the wait. A selected notification racing a
scheduler blocking refusal also rechecks the word, interruption and deadline.
Blocking refusal without a selected notification returns `MG_ERR_BUSY`, with
no registered waiter or retained lease. If the value already differs at
entry, wait returns `MG_ERR_WOULD_BLOCK`; timeout, Ctrl+C, and orderly shutdown
return `MG_ERR_TIMEOUT`, `MG_ERR_INTERRUPTED`, and `MG_ERR_SHUTDOWN`. Process
exit wakes a blocked waiter and prevents address-space teardown until its
syscall and lease unwind.

The caller publishes the new word value with release ordering before calling
wake. The kernel's expected-value read is acquire ordered. The wait operation
only compares the value; it does not modify it. The word must remain mapped and
alive for every concurrent atomic, wait, and wake operation. A successful
unmap after all operations return permits the address to be reused; a stale
wake in a later allocation names only the new address-space/address key.

## Mutexes and conditions

`mg_mutex_t` and `mg_condition_t` in `<mg/sync.h>` can be statically
initialized with `MG_MUTEX_INITIALIZER` and `MG_CONDITION_INITIALIZER`, or
initialized with their corresponding functions. A mutex uses an atomic
userspace fast path; only contention enters the wait/wake syscalls. Lock may
return `MG_ERR_INTERRUPTED` or `MG_ERR_SHUTDOWN` without acquiring the mutex.
The caller must own a mutex when unlocking it; the current object does not
store an owner identity and cannot detect an unlock by the wrong thread.

Condition waits require the caller to hold the associated mutex. They capture
the condition sequence, release the mutex, and wait for that sequence to
change. A signal that occurs between release and wait registration changes
the sequence, so the expected-value check prevents a lost notification.
Waits may return spuriously; callers must always recheck their predicate while
holding the mutex. The mutex is reacquired before a condition wait returns,
including timeout and ordinary interruption results. Reacquisition remains
interruptible by process shutdown. On `MG_ERR_SHUTDOWN`, the wait returns
without reacquiring so shutdown can retire a service whose sibling may also be
stopping. A process-wide exit request terminates at the syscall boundary after
the active syscall and lease have unwound.

Destroy returns `MG_ERR_BUSY` while a mutex owner, mutex waiter, or condition
operation is active. Successful destruction closes admission to later
operations; they return `MG_ERR_BAD_ARGUMENT`. Destruction must not race with
object reinitialization or storage reclamation. Initializing an object
requires fresh or otherwise quiescent storage. These rules do not make the
allocator, stdio, or other mutable loam facilities thread-safe.

## Implementation and verification boundary

`kernel/src/user_sync.c` owns the single address-keyed wait queue and bridges
to the existing scheduler event-wake mechanism. Waiter records live on the
blocked syscall's kernel stack, so queue registration itself does not allocate
memory. The lock order is VMM lease acquisition and release before taking the
wait-queue lock; the wait-queue lock may call the scheduler's short wake path.
Neither lock is held across blocking. Process syscall admission and the VMM
lease jointly protect process, stack, address-space, and word lifetimes.

Host sanitizer tests execute the production kernel wait/wake and loam
synchronization sources with controlled scheduler/VMM fixtures. They exercise
host pthread concurrency and do not replace native SMP qualification. The
native worker lifecycle acceptance runner exercises Ring 3 creation, TLS,
wait/wake, mutex/condition coordination, join/detach, and safe resource reap on
two KVM CPUs. loam's allocator, stdio streams, and other mutable shared state
remain outside the thread-safety contract.
