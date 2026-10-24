# Kernel and process memory layout

Mangrove uses a fixed x86-64 high-half kernel layout shared by the loader and
kernel through `include/address_layout.h`. Pith currently builds a four-level
PML4 page-table hierarchy for 48-bit canonical addresses; the fixed regions
below occupy separate high-half PML4 branches where required by the VMM.

| Region | Start | End or size | Purpose |
| --- | --- | --- | --- |
| User addresses | `0x0000000000000000` | below `0x0000800000000000` | Per-process mappings |
| Anonymous mappings | `0x0000400000000000` | below `0x00007fff00000000` | Process `memory_map` allocations |
| User stack | below `0x00007ffffff00000` | 8 pages initially | Executable stack |
| Physical direct map | `0xffff800000000000` | 64 TiB | Ordinary mapped RAM |
| Kernel heap | `0xffffc00000000000` | `0xffffc08000000000` | PMM-backed heap pages |
| MMIO ioremap | `0xffffc08000000000` | `0xffffc10000000000` | Device mappings |
| Leased kernel virtual range | `0xffffc10000000000` | `0xffffc18000000000` | Explicit VMM range leases |
| Kernel image | `0xffffffff80000000` | linked image extent | Pith text, data, and BSS |

The kernel image is loaded at physical address `0x00100000` and linked at
`0xffffffff80000000`. The fixed 1 MiB placement fits the current QEMU/OVMF
map. A fixed placement can conflict with firmware reservations as the kernel
grows; relocatable or firmware-selected placement is not implemented. This is
not a universal UEFI placement guarantee. Direct-map conversions are
valid only for physical addresses below the 64 TiB map limit. MMIO is
deliberately excluded from that map and receives separate cache-controlled
ioremap addresses.

The heap interval is a 512 GiB virtual reservation, not a pre-mapped or
physically reserved 512 GiB pool. Only its monotonically committed prefix has
page mappings and PMM-backed frames. The direct map ends at the heap base; the
512 GiB ioremap interval follows the heap. The next 512 GiB PML4 branch is
reserved for explicit VMM virtual-range leases. The current layout leaves the
image at `0xffffffff80000000` in the upper canonical range. No heap-capacity or
region relocation is part of the slab classifier.

## Bootstrap and permanent mappings

The UEFI loader creates a temporary low identity mapping for the firmware
handoff and a high direct map for supported RAM descriptors. It also maps the
kernel ELF segments with their individual permissions. This bootstrap CR3
exists only to cross the firmware-to-kernel boundary.

Pith builds a new master kernel page-table root. Kernel text is executable and
read-only; read-only data is non-executable; writable data and BSS are
non-executable. The direct map, heap, ioremap branch, and kernel image remain
supervisor-only. The loader requires CPU NX support and enables `EFER.NXE`
before page tables containing NX entries are used. `CR0.WP` is enabled at the
handoff and checked on every initialized CPU, so supervisor stores obey
read-only leaf permissions too.

The PMM bitmap covers the highest firmware-reported usable RAM frame and rounds
its byte count up, leaving padding unavailable. A second bitmap records frames
actually returned by `pmm_alloc_frame`; `pmm_free_frame` rejects reserved,
unaligned, out-of-range, and never-allocated frames instead of treating every
busy bitmap entry as releasable. Bitmap storage is reserved from a usable
descriptor before the allocator publishes its free-frame count. Initial heap
setup reserves four frames before mapping them. On PMM reservation failure it
returns every frame already reserved. If a leaf mapping fails, it returns the
failed frame and the still-unmapped suffix; the mapped prefix remains reachable
in the kernel root and boot stops rather than continuing with an uninitialized
heap.

## Process address spaces

Each process receives a fresh lower half and references only the fixed,
supervisor-only high-half branches from the master kernel root. User page-table
and leaf-frame ownership is tracked separately from x86 permission bits so
destroying a process cannot reclaim shared kernel tables. Address-space
validation rejects lower-half aliases to kernel mappings and any user bit in a
shared kernel branch.

Address-space destruction rejects new switches after `destroy_pending` and
waits for the active-CPU mask to empty. Its caller must first quiesce future
software users: a blocked exit syscall can be inactive in CR3 bookkeeping and
still need its process root when scheduled again. Process collection therefore
requires terminal scheduler state and completion of the final stack handoff,
in addition to the process's published termination status. Failed thread
destruction stops process teardown before any address-space release.

User page mappings inspect the destination hierarchy while holding the
address-space operation mutex. Only missing page-table levels are allocated;
allocation happens outside the metadata spinlock, and the path is revalidated
before installation. Mapping another page in an existing table needs only a
leaf ownership record, not three spare page-table frames.

ELF `PT_LOAD` pages are mapped according to their write and execute flags. The
loader rejects segments below page one, beyond the user limit, outside the
file, or with invalid ranges or alignment. Anonymous mappings are page-rounded
and placed in the dedicated anonymous region. Syscalls validate every user
buffer against the current user mappings before dereferencing it.

## Owned kernel virtual ranges

`vmm_kernel_range_reserve()` leases page-aligned ranges from
`KERNEL_VMM_RANGE_BASE..KERNEL_VMM_RANGE_LIMIT`, a 512 GiB interval in its own
PML4 branch immediately after ioremap. The VMM uses a fixed table of 2,048
reservations, first-fit allocation, and nonzero generation tokens. Handles
must be zero-initialized before reservation; release validates the exact base,
length, and generation, rejects stale copied handles after address reuse,
and clears a handle only after successful release. Reservation metadata is
static and introduces no allocator recursion. Exhausting either the address
interval or the bounded reservation table returns failure.

The new PML4 branch is shared by process roots using the same supervisor-only
high-half rules as the existing kernel regions. `vmm_map()` rejects addresses
in this branch unless the full page is covered by a current active lease. A
reservation itself changes no page table, allocates no page-table frames,
acquires no PMM frames, and performs no mapping or unmapping.

`vmm_kernel_range_unmap()` removes a page-aligned subrange, expressed as a
page offset and count within the lease. One call is limited to 256 pages. It
validates every leaf before changing any PTE, so validation failure leaves the
whole requested subrange unchanged. The lease enters an unmapping state before
the PTEs are cleared; while in that state mapping and release are rejected.
On SMP, the call requires interrupts enabled and no spinlock held before entry;
otherwise it returns failure without changing mappings. This prevents a
shootdown wait from depending on a CPU that cannot service its IPI while
waiting for a caller-owned lock.
The VMM locally executes `invlpg` for every removed address, then sends the
same bounded invalidation to every other online CPU. Each remote handler
invalidates the addresses even when running under a different CR3, then
acknowledges. This is required because process roots share kernel page tables
and kernel leaves may be global; a CR3 reload alone does not reliably evict a
global translation. The shootdown gate serializes leased mapping publication,
teardown, and CPU online/offline membership transitions. A CPU is flushed
before it joins the online set. Mapping locks are released before the sender
waits for acknowledgements.

Unmap returns success only after local invalidation and acknowledgements from
all online remote CPUs have completed. It then decrements the lease's mapped
page count and returns it to the active state. The caller must separately
quiesce software users before unmapping; TLB invalidation does not prevent a
concurrent kernel dereference. IPI delivery failure or acknowledgement timeout
is fail-stop: the cleared lease remains unmapping and cannot be released or
reused. After all leaves have been removed, release validates the generation
and confirms that no mapping remains before consuming the handle. Runtime CPU
hotplug is unsupported. AP startup flushes before online publication; a BSP
timeout cannot remove a possibly executing AP from shootdown targets, and an
AP that fails after publication withdraws itself through the synchronized
CPU-state hook.

Explicit PMM-owned backing is available through
`pmm_owned_frame_acquire()`, which returns one zeroed, page-aligned frame and
a handle containing its physical address, registry slot, and generation. The
handle must be zero-initialized before acquisition. The fixed PMM registry is
bounded to 2,048 simultaneously owned frames; acquisition fails without
changing PMM accounting when the registry or free-frame supply is exhausted.
Frames are acquired individually and are not guaranteed physically
contiguous. This is not a second general-purpose physical allocator.

The handle is the sole release authority. Raw `pmm_free_frame()` cannot free
these frames, `pmm_reserve_range()` rejects them, and
`pmm_owned_frame_release()` rejects stale, copied-after-release, forged,
ordinary, reserved, or currently mapped handles. A successful release
consumes and clears the supplied handle; copied handles remain invalid when
the registry slot is later reused with a new generation. The physical direct
map remains installed, so the owner must also quiesce all software users
before release.

`vmm_kernel_range_map_owned()` maps a frame only into a page of the exact live
lease, checking both the VMM lease generation and PMM frame generation. The
existing `vmm_map()` path also recognizes and pins an owned frame if it is
mapped into a leased range; it rejects mapping an owned frame into other
kernel virtual regions, and `vmm_map_user_page()` rejects owned frames so a
user mapping cannot outlive the owner. An owned frame may have only one
dynamic leased-range mapping at a time. This mapping pin prevents release
until successful range unmap completes. `vmm_kernel_range_unmap()` clears the
PTEs, performs local invalidation, waits for all online CPUs to acknowledge
invalidation (including CPUs running a different CR3), then clears the PMM
mapping pins. The caller
may then release the frame handle and release the now-empty virtual lease.
On validation failure, mappings and ownership remain unchanged. A shootdown
timeout is fail-stop: the lease remains unavailable and the PMM mapping pin
remains set, so the frame cannot be reused.

Mapping claims use the existing VMM-to-PMM lock order for a short metadata
transition. The PMM lock is never held while waiting for remote invalidation;
VMM mapping and lease locks are also dropped before the wait. The owned-frame
mapping pin is cleared only after the acknowledged shootdown completes.

The caller must quiesce software users before unmapping; TLB invalidation does
not stop concurrent kernel dereferences through the lease or the permanent
physical direct map. Intermediate page-table frames deliberately remain
installed. Heap mappings, MMIO mappings, user-address-space reclamation, and
page-table hierarchy reclamation are not provided. Production slab backing
uses the owned-frame and leased-range lifecycle described below; the page-table
frames used to establish those mappings remain installed.

## Heap verification

The kernel heap is a 16-byte-aligned, coalescing block allocator. Its physical
block chain records neighboring blocks; a 32-entry logarithmic size index
contains only free blocks. Allocation searches bins capable of satisfying the
request, then splits the selected block when the remainder can hold another
header and at least 32 payload bytes. Free validates the payload address and
block links, removes adjacent free blocks from their bins, and coalesces them.
Invalid and repeated frees stop in the kernel panic path.

On x86-64, each block header is 48 bytes, including physical-chain links,
free-index links, and an explicit free-bin membership flag. The flag occupies
existing structure padding; the header remains 48 bytes. Payload and statistics
are rounded to 16-byte alignment. Heap total/used/free statistics count payload
capacity and exclude headers; they are maintained under the heap lock and
queried in constant time. For a mapped heap containing *n* blocks, metadata
occupies *n* times 48 bytes in addition to the reported payload capacity. Small
allocations may consume a final unsplittable remainder, so used payload can be
slightly larger than the requested byte count.

The heap starts with four 4 KiB supervisor, writable, non-executable pages and
grows in four-page groups. Before the first `sti`, the BSP may extend it
without a scheduler-owned mutex: interrupts must remain disabled, the BSP
CPU-local record must be active, and no other CPU may be online. This
bootstrap window covers early consumers such as the framebuffer backbuffer,
scheduler stacks, and CPU topology storage. It uses the same PMM-backed heap,
block metadata, accounting, and 512 GiB virtual heap reservation
as later allocations; it is not a separate pool or a fixed pre-reservation.
Exhaustion or failure to obtain/map backing returns `NULL` without exposing an
unmapped page. The window is closed immediately before the init graph enables
interrupts.

After bootstrap, growth is serialized by a mutex and requires a schedulable
thread, enabled interrupts, and no spinlock held on the current CPU. A
per-CPU spinlock depth is tracked by the lock implementation. Timer-driven
preemption remains pending while that depth is nonzero, and scheduler block,
yield, sleep, and termination operations reject a held-spinlock context. This
keeps a thread from sleeping or migrating while it owns a spinlock. `kmalloc` may
still satisfy a request from an existing free block in such a context; if
growth is needed it returns `NULL` before acquiring the growth mutex or
reserving PMM frames. With interrupts disabled, growth is allowed only in the
bounded bootstrap window and with no spinlock held. A missing scheduler
context also makes ordinary growth fail. PMM frame acquisition and VMM mapping
occur without the heap spinlock; that lock protects block metadata and free
bins and is held only for nonblocking allocator work. Interrupt handlers and
other non-sleepable callers can use existing free blocks but cannot depend on
heap growth. Some complete four-page heap tail groups can be reclaimed as
described below.

Each mapped page is owned by the heap only after its supervisor writable,
non-executable leaf mapping succeeds. Page-table levels missing from a kernel
mapping are built detached and published only after the subtree is initialized.
If a page-table allocation fails, those detached table frames are returned to
PMM and the failed page mapping leaves no new intermediate tables behind.
During a four-page heap growth, already mapped pages are retained and
incorporated into the heap if a later leaf mapping fails; frames never mapped
are returned to PMM. `kmalloc` retries once against that published prefix: it
returns the allocation if the prefix is sufficient, otherwise returns `NULL`
without an unbounded retry. If no page was mapped, it returns `NULL` and all
reserved data frames are returned. Normal mapping publication performs the
existing remote TLB synchronization; bootstrap mapping is allowed only while
the BSP is alone, so no remote shootdown is needed. The kernel virtual heap
range is 512 GiB, but actual growth is also bounded by available PMM frames and
page-table memory.

The temporary low identity mapping used for AP startup borrows its reserved
trampoline data page. Removing that mapping reclaims only empty page-table
frames; it never returns the trampoline page itself to the PMM.

After bootstrap, ordinary growth first attempts four PMM-owned frames under an
exact four-page lease at the heap tail. The fixed heap backing ledger limits
these reclaimable groups to three quarters of the shared PMM/VMM ownership
table. This is a ceiling on heap-owned groups, not a per-consumer reservation:
the ownership records remain shared and other users, including slabs, may use
them first. If this heap group budget, lease table, or owned-frame supply is
unavailable, growth uses the legacy PMM mapping path. Such raw-backed pages
remain usable but are not eligible for physical reclamation. A later raw-backed
tail therefore prevents reclaiming older owned groups; reclamation deliberately
does not scan or unmap interior heap pages.

`heap_reclaim_bounded()` withdraws at most four complete, unused owned groups
per call, and only from the committed tail. It requires interrupts enabled, no
spinlock held, and successful acquisition of the heap growth mutex. The heap
lock removes the free tail from its index and publishes the shorter accessible
prefix before dropping the lock for VMM teardown. Allocations cannot reuse the
withdrawn suffix; frees to its left remain separate until the trim commits or
rolls back. The VMM unmaps the four leaves and waits for local and all-online-CPU
TLB invalidation acknowledgements before returning. Only then does the heap
release the four PMM-owned frames and lease, shorten its block chain, and
coalesce the remaining tail. If VMM teardown fails before mappings are
modified, the heap restores the old published range and free-bin membership.
An incomplete shootdown remains fail-stop under the VMM contract. The initial
four heap pages and raw-backed growth pages remain mapped permanently. The
`kmalloc()` pressure path performs at most one reclaim attempt and retries once
only if a complete group was released.

This operation releases owned heap frames to the PMM; it does not reclaim the
intermediate page-table hierarchy or make the direct-map alias disappear.
Successive recommitment reuses the shortened virtual tail and acquires new
generation-owned frames through the same growth path.

The kernel-private `heap_alloc_page_extent()` operation returns a nonempty,
page-aligned, virtually contiguous extent through the same indexed-heap block,
accounting, PMM, VMM, and growth paths. Its output handle must be empty on
entry; a live handle cannot be overwritten by a second allocation. The page
count is checked for multiplication overflow. Each successful extent receives
a nonzero generation lease stored both in the allocation header and handle.
The paired `heap_free_page_extent()` validates nonzero page count, page
alignment, size arithmetic, and payload bounds, then checks the handle
generation under the heap lock before releasing the allocation. This rejects
raw/public heap frees of extent-owned blocks and stale copied handles even if
the same virtual address is later reused. Generations are not reused during a
boot; if the 32-bit sequence is exhausted, new extent acquisition fails
cleanly. A successful release consumes and clears the supplied handle. The
extent does not promise physically contiguous frames, DMA suitability, or a
separate physical allocation. A lock-free committed-range check is available
because the published heap prefix advances only after mappings and block
metadata are ready, and tail reclamation shortens it before unmapping begins.

## Unified heap-and-slab allocation

The ordinary kernel build links the indexed heap and slab backend together.
`KERNEL_HYBRID_ALLOCATOR` defaults to `1` and the default
`KERNEL_HYBRID_ALLOCATOR_CUTOFF` is 256 bytes. Requests through 256 bytes use
the slab class path when initialized; larger requests use the indexed heap.
Set `KERNEL_HYBRID_ALLOCATOR=0` for an indexed-only comparison or recovery
build. The supported cutoff values remain 256, 288, 320, 352, and 384 bytes.
Isolated slab-core diagnostic builds may also link the slab module without
enabling public routing.

Each slab currently occupies one 4 KiB page. Slab backing uses a fixed
2,048-entry static ownership ledger, one PMM-owned frame, and one one-page VMM
lease per backing page. The ledger keeps both generation handles until the
slab is unpublished, the dynamic mapping is removed, the PMM mapping pin is
cleared after SMP shootdown, the frame is returned to PMM, and the virtual
lease is released. Its storage is bounded and does not allocate recursively.
Classifier nodes and per-CPU cache records continue to use the indexed heap;
the slab data pages do not.

Acquisition reserves a ledger entry, acquires a zeroed PMM frame, reserves a
virtual lease, prepares classifier metadata, and maps the frame read/write and
non-executable. The descriptor is initialized before class-locked publication.
If an unpublished candidate loses to a concurrently created slab, its mapping
is torn down and its frame and lease are returned through the same lifecycle.
Any failure before mapping publication rolls back acquired resources. A
failure to complete teardown keeps the ledger entry unavailable for reuse.

The sparse classifier covers the leased 512 GiB virtual interval. A token
identifies the slab class and page generation and carries temporary lookup
pins. Cached slots remain bitmap-owned just like live objects, so a page cannot
retire while any per-CPU cache or refill still refers to one of its objects.
On the final slot release, the class lock withdraws the class tag and marks the
token retiring. New lookups then fail; the backing ledger remains owned until
existing classifier pins drain. Remote frees use the same slot transition,
and a cached remote-free object continues to prevent page retirement until it
is drained.

The backing reclaimer performs no unmapping while holding a slab class/cache
lock, while a tracked spinlock is held, or with interrupts disabled on SMP. It
try-locks the bounded ledger, confirms the retiring token has no pins, and
calls `vmm_kernel_range_unmap()`. The VMM clears the PTE and waits for local
invalidation plus acknowledgements from every online CPU, including CPUs
running another CR3. Only after that acknowledged transition clears the PMM
mapping pin does the slab backend clear the classifier generation, release the
owned frame to PMM, and release the empty virtual lease. Thus a successful
slab backing release means the physical data frame is free in PMM, not merely
reusable by the indexed heap. The permanent direct-map alias remains mapped;
the allocator relies on all slab software references being quiescent and does
not expose that alias as an object pointer. Intermediate page-table frames are
not reclaimed.

If a reclaim pass cannot take a required lock, sees a live classifier pin, or
cannot enter the VMM operation in the current context, it leaves the backing
record pending. A later allocation or explicit drain can retry it. A VMM
shootdown timeout remains fail-stop under the VMM contract; the kernel does
not return the frame to PMM or make the lease available after an incomplete
shootdown. The existing heap page-extent API remains for indexed-heap
operations; slabs no longer use it for backing. Multi-page slabs are not
implemented.

The private slab API supports twenty classes: 16-byte increments through 256
bytes, then 288, 320, 352, and 384 bytes. Each 4 KiB virtual slab page has an
80-byte descriptor and 32-byte allocation bitmap; object payload starts at
offset 112. Two one-byte per-slot arrays occupy the page tail: an encoded
ownership state and a state-dependent CPU owner (last allocating CPU while
client-owned, cache CPU while cached). The slot count is
`floor((4096 - 112) / (class_size + 2))`; the unused remainder is page-tail
waste. Objects retain 16-byte alignment. Slot lookup scans at most four
bitmap words. Classes with at most 16 slots use a bounded bit scan in the
first word; larger classes use a count-trailing-zeros instruction on the first
available word. One IRQ-safe class lock protects shared partial lists,
descriptors, and bitmaps. Per-CPU object transitions use separate class-local
cache locks described below.
Full slabs are found through page classification and are not linked; an empty
slab is unlinked, declassified under its class lock, then queued for
out-of-lock unmapping and PMM release. The sparse page classifier and
descriptor checks reject bad
class tags, unaligned/interior frees, double frees, and unsafe list transitions
by failing closed. The bitmap marks every slot not in the shared-free state,
including client-owned, locally cached, and temporary refill objects. Cached
objects therefore keep their slab backing alive, and `live_slots` remains the
number owned by callers while `cached_slots` counts local ownership. Each
bitmap word has a stored 32-bit integrity check included in the descriptor
integrity value. The diagnostic module has a
compile-time validation policy: `SLAB_DEEP_VALIDATION=0` is the normal slab
path and `=1` enables additional whole-bitmap and neighboring-list checks. The
setting is recorded in the guest test's `SLAB_GUEST_BEGIN` witness; it is not a
runtime branch. It affects isolated slab builds only, not the ordinary kernel.

Both modes retain checks required before accessing or changing ownership: the
free path validates the leased slab range, uses the first page token only to
select a class lock, reloads and compares the complete
class-and-generation token under that lock, validates descriptor address and
contents before using them, checks object-area bounds and exact slot
alignment, validates the touched bitmap word, and rejects a clear allocation
bit. A nonfinal update also checks live-count bounds. Before any list
transition, both modes validate each neighbor's mapped address, live class
token, descriptor integrity, state, and reciprocal link before changing a
link. A final free checks all bitmap words and population before retiring the
slab. Retirement clears the class tag under the class lock and returns backing
only after unlocking.

Normal mode does not scan unrelated bitmap words or validate both neighbors
after every ordinary operation. It still checks the descriptor integrity
value, which covers descriptor fields and all per-word integrity values, and
checks the bitmap word used by the operation. Corruption in an untouched
bitmap word with a correspondingly updated checksum can therefore escape an
ordinary nonfinal operation; it is detected when that word is used, before a
final retirement, or by `slab_verify()`. A reciprocal-link error with an
internally consistent descriptor checksum can likewise be detected at a later
list transition or verifier rather than on every unrelated free. Deep mode
checks all bitmap words and population, plus neighboring descriptor and
reciprocal-link consistency, on each touched operation and after mutations.
This is earlier detection, not equivalent immediate detection in normal mode.

`slab_verify()` remains an explicit whole-module verifier in both modes. It
locks per-CPU caches in CPU-index order before each class lock, validates
cached entries and their uniqueness, walks class lists and active owned
backing records, validates descriptors and bitmap words, and reconstructs caller,
cached, refill, partial, full, and empty counts. It is not run implicitly
after every normal-mode operation. Classifier nodes remain allocated for the
module lifetime, so a lookup that has loaded a node pointer never races its
reclamation. A reused page is checked against its newly published generation
and class tag. As with other raw-pointer allocator interfaces, a stale pointer
whose address has since been reallocated to a new live object cannot be
distinguished from that new object's pointer.

### Per-CPU cache core

`slab_cpu_alloc()` and `slab_cpu_try_free()` are kernel-private APIs. The
default unified public path routes eligible requests through them; the
indexed-only build keeps ordinary `kmalloc()`/`kfree()` on the indexed heap.
The cache holds at most two objects per discovered CPU/class and refills two
at a time. Each CPU has one 64-byte-aligned record per class. At these defaults a
record occupies 128 bytes and metadata uses 2,560 bytes per CPU, at most
655,360 bytes for `CPU_MAX_COUNT == 256`, plus 63 bytes of alignment slack
and indexed-heap block metadata. `slab_init()` allocates and initializes the
records before publishing the module as ready. Compile-time capacity and
refill overrides are limited to 1–16 and exist for diagnostic measurements;
they do not change the default cache policy.

The capacity experiments use equal capacity and refill batch. Their exact
record and conservative ownership bounds are:

| Capacity/refill | Record bytes | Metadata per CPU | Cached plus in-flight slots per CPU/class | One-page-per-slot backing bound per CPU | Bound at 256 CPUs |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 128 | 2.5 KiB | 4 | 320 KiB | 80 MiB |
| 4 | 192 | 3.75 KiB | 8 | 640 KiB | 160 MiB |
| 8 | 256 | 5 KiB | 16 | 1.25 MiB | 320 MiB |
| 16 | 384 | 7.5 KiB | 32 | 2.5 MiB | 640 MiB |

The backing bound counts each cached or in-flight slot as if it were the only
owned slot on a distinct 4 KiB slab page. It is deliberately conservative and
is not expected use: objects of one class share pages, and cached objects may
share pages with caller-owned objects. For the default 2/2 policy the bound is
640 KiB at two CPUs and 2.5 MiB at eight CPUs. It excludes classifier nodes,
caller-owned slab objects, and the optional empty-slab reserve. Do not add
these bounds to measured page counts without checking for shared pages.
For unequal settings the same bound uses `(capacity + refill_batch) * 20 *
4096` bytes per CPU. The measured 2/4 and 4/2 variants both bound cache/refill
ownership at 120 MiB for 256 CPUs; their per-CPU metadata differs with cache
capacity (2.5 KiB and 3.75 KiB respectively).

A cache miss reserves one refill batch for that CPU/class. `refill_active`
prevents another suspended refill from reserving a second batch against the
same record. Refill drops the local-cache lock before shared allocation or
heap growth; a competing miss uses the shared path. Refill slots remain
bitmap-occupied until cached, returned to the shared slab, or handed to a
caller. A free enters the freeing CPU's cache where space remains, regardless
of the allocation CPU. Cache overflow returns the slot through the shared
class lock. Cached and refill slots remain allocated in the descriptor bitmap
and prevent their slab page from retiring.

Local cache hits and frees take only that CPU/class's IRQ-save lock in normal
validation mode. A free is cached on the CPU performing the free, regardless
of where the object was allocated. The per-slot owner byte records the last
allocating CPU while caller-owned and the cache CPU while cached; a cache hit
therefore rejects an entry corrupted to point at another CPU's cached object.
A full cache returns the object through the shared class lock. Before using a
selected cache, the allocator reads the GS-based CPU index, acquires that
cache's IRQ-save lock, and confirms the CPU
index did not change; it retries at most four times, then uses the shared
path. The saved interrupt state is restored by the lock primitive. Interrupt
disable prevents maskable interrupt reentry and scheduler migration while
the cache record is held. NMI-context allocation is unsupported. Refill, PMM
frame acquisition, and VMM mapping happen with no cache lock held. These
operations do not sleep; exhaustion or unavailable backing returns a clean
slab allocation failure for the public heap fallback.

The lock order is cache record then class lock. Shared allocation/free paths
never acquire a cache record while holding a class lock. The runtime
`slab_reclaim_bounded(max_records, max_objects, result)` API provides a
nonblocking, best-effort drain while other allocator users continue running.
It visits at most the requested CPU/class records and returns at most the
requested number of cached objects. It uses one-shot IRQ-save try-locks for
both cache and class records, skips busy records, and rotates its starting
record across calls. Results report records examined, busy locks, refill gates
observed, objects returned, pages actually released, and whether a caller
budget stopped the pass. A true return means the pass ran; it does not claim a
global drain because a concurrent free or refill may add cache entries after
inspection. Calls while uninitialized, over budget zero, or while holding a
tracked spinlock fail immediately.

For each object drained, the cache lock establishes exclusive cache-entry
ownership; the page token is pinned before descriptor access; and the class
lock validates generation, descriptor, exact slot, owner, and `CACHED` state
before returning the slot to `SHARED_FREE`. If the last owned slot leaves a
page, classification is withdrawn under the class lock. The cache and class
locks are released before backing teardown. The reclaimer defers pages with
outstanding pins instead of waiting while holding allocator state. The same
bounded pass retires optional empty reserves. It never invokes slab allocation
while draining.

After PMM/lease/mapping backing acquisition fails, the slab allocator makes one
bounded pressure pass (at most 128 cache records and 32 objects). It retries
backing acquisition only if a complete slab page was returned to PMM. If
objects were returned without releasing a page, the existing shared-class
recheck can use newly available slots of the requested class. A busy lock,
unsupported held-spinlock context, or no progress leaves the allocation on the
existing failure path; there is no retry loop. The public allocator may then
use its documented indexed-heap fallback for a clean slab-unavailable result.

`slab_cpu_drain_all()` remains a quiescent operation: callers must stop all
slab allocation/free users first. It drains every cache, then attempts to
process the fixed backing ledger. If the caller has interrupts disabled or
teardown otherwise cannot proceed safely, the backing record stays pending
for a later safe pass; the return value counts cache objects drained, not
physical frames returned. `slab_drain_empty()` is likewise for quiescent
diagnostics; runtime pressure uses try-lock draining. CPU hotplug is
unsupported; records cover all discovered CPUs and remain allocated for the
module lifetime. Per-CPU normal
fast paths validate token/generation, immutable
descriptor identity, exact object address, and atomic slot state. They do not
read mutable list/count metadata. Deep mode additionally takes the class lock
and performs full descriptor, bitmap, and neighbor checks on local cache
operations. The normal-mode local path can therefore detect some mutable
descriptor damage later, at a class-locked operation or explicit verifier;
this is not immediate detection equivalent to deep mode.

### Public allocation routing and initialization

In the ordinary build, the slab core is initialized after SMP startup. The
default cutoff is 256 bytes, matching a supported size class; the slab backend
supports classes from 16 through 256 bytes in 16-byte steps and also 288, 320,
352, and 384 bytes for builds selecting those cutoffs. Requests above the
cutoff use the indexed heap. `KERNEL_HYBRID_ALLOCATOR=0` retains a complete
indexed-only build, and `KERNEL_HYBRID_ALLOCATOR_CUTOFF` selects among 256,
288, 320, 352, and 384 for hybrid builds.

Before slab initialization, public allocations use the indexed heap. If slab
initialization fails, the kernel logs the failure and continues indexed-only
for that boot; no slab objects have been published. Objects allocated before
initialization remain heap-owned and can be freed after initialization. After
successful initialization, dispatch remains enabled for the boot lifetime;
turning it off while slab objects may be live is unsupported.

For an eligible request, a clean slab unavailability or backing-allocation
failure may fall back to the indexed heap. Corruption or invalid ownership
does not fall back and remains fail-closed. `kfree` uses the generation-aware
slab classifier to identify slab ownership; a classified pointer is validated
and freed by the slab backend, while an unclassified pointer is validated by
the indexed heap. Dispatch does not infer ownership from the current size
cutoff, so objects allocated before initialization or by a different eligible
policy retain their correct release path. Raw heap allocation/free bypass
public routing and provide indexed backing for classifier nodes and CPU-cache
records. The heap page-extent interface remains available to indexed-heap
operations; slab data pages use PMM-owned frames and leased mappings.

Zero-size allocations return `NULL`; oversized or overflowing requests fail
without unsafe retry. The kernel currently exposes `kmalloc`/`kfree`, but no
kernel `krealloc` API; userspace loam `realloc` is a separate contract.

When an indexed-heap allocation fails, the hybrid dispatcher makes at most
one bounded `slab_reclaim_bounded()` pass before its permitted retry. For a
small request it retries the slab path only if cached objects were returned;
it retries backing acquisition only when a complete slab frame was returned
to PMM. It does not repeat heap growth or wait for busy CPU records. The
reclaim path uses PMM/VMM ownership APIs and never calls public `kmalloc`, so
it cannot recurse through the dispatcher. Reclamation is disabled for calls
already holding a tracked spinlock and backing teardown is deferred with
interrupts disabled on SMP. Interrupt-disabled callers may use existing
slab/cache/heap capacity, but reclamation cannot sleep or grow the heap.
Indexed-heap backing cannot be reclaimed from this restricted context. In a
permitted context, bounded reclamation can return fully free, owned four-page
tail groups to PMM; bootstrap and raw-backed pages remain mapped.

### Allocation failure and pressure contract

`kmalloc()` returns `NULL` for zero or overflowing requests, when the caller's
context cannot grow an allocator, or when available memory and bounded
reclamation cannot satisfy the request. Existing heap blocks and slab slots
remain usable without new PMM frames. An eligible small request falls back to
the indexed heap only after a clean slab-unavailable result; a classified
ownership error, damaged metadata, failed ownership rollback, or invalid free
remains fail-closed and is not converted into fallback success.

The PMM-owned-frame registry and kernel virtual-range lease table each have
2,048 entries. Slab backing records use the same 2,048-entry bound. The heap's
owned-tail ledger is limited to 384 four-page groups, so heap-owned growth alone
can occupy at most 1,536 frame and lease records. The remaining capacity is not
reserved per consumer; the registries are shared and can be consumed by slab or
other owned allocations. Exhausting any of these registries is an ordinary
acquisition failure. Heap growth may then
use raw PMM frames if they are available; those pages remain mapped and cannot
be returned to PMM by heap tail reclamation. Slab backing acquisition rolls
back any frame or lease it acquired before a later step fails. Heap growth
retains a successfully mapped prefix if a later leaf fails, releases the
unmapped suffix, and returns `NULL` unless that prefix can satisfy the
request. Impossible ownership transitions and inability to complete a
required rollback remain fail-stop conditions.

After a clean allocation failure, the hybrid dispatcher makes at most one
bounded slab-reclaim pass (128 CPU/class records and 32 cached objects), then
retries only when the pass reports reclaimed objects or backing. A failed
indexed allocation also makes at most one heap-tail pass, which can withdraw
one four-page group from the committed tail, and retries once only after that
group is released. The slab backing allocator has its own single bounded
reclaim-and-retry attempt after backing acquisition fails. Busy records and
spinlock-held or interrupt-disabled teardown contexts are skipped or deferred;
there is no global wait or repeated retry loop. These paths do not invoke the
public allocator recursively.

There is no OOM killer, swap, demand paging, or general process-memory
reclaimer. Kernel callers must check allocation results and unwind resources
they already acquired. A `NULL` result is the supported outcome for genuine
resource exhaustion; it does not indicate heap corruption by itself.

The unified hybrid is now the ordinary build selection; the profile and
performance data below were collected before this routing default changed and
remain evidence about those source-matched workloads, not a claim that all
Pith workloads improve. A source-matched two-vCPU indexed-only Pith profile
counted 18,459 successful
allocations, 14,242 frees, 4,217 live objects at the snapshot, and 2,709
cross-CPU frees. At the 384-byte cutoff, 8,591 requests (46.54%) covered
675,639 bytes (0.294% of requested bytes); the 256-byte candidate covered
7,237 requests (39.21%) and 287,802 bytes (0.125%). The profile's
ideal-packed demand estimate was 51 pages at 256 bytes and 53 at 384 bytes;
the sum of independent per-class peaks was 60 and 66 pages. These are models
from the indexed trace, not observed hybrid occupancy, and they exclude
per-CPU cached slots and alignment gaps.

The current five-trial GCC 14.2 `-O2` host comparison used four pinned,
overlapping workers. The public 384-byte hybrid measured 4.81M/4.89M immediate
allocation/free pairs per second at 16/72 bytes versus 3.90M/3.69M indexed-only.
Single-worker median pair times at 16/72/192/384 bytes were 120.3/121.3/117.2/
120.2 ns for the full hybrid and 133.6/133.8/134.2/134.7 ns indexed-only.
The 256-byte cutoff routed 384-byte requests through the heap and measured
162.7 ns at that size, a workload-specific regression. An immediate-lifetime
replay of the Pith request-size histogram was at parity: 127.70 ns/pair public
hybrid and 127.84 ns indexed-only. The serialized replay of the complete Pith
event order measured 121.18 ns per allocator operation for the public hybrid
and 110.25 ns indexed-only; five-trial ranges overlap. Replay preserves object
identity, lifetime/order, phase, and logical free CPU, but not guest
concurrency, migration, interrupts, or timing.

The historical 1,024-live-object host fixture mapped 344,064 bytes at cutoff
384 and 262,144 bytes at cutoffs 256/288, versus 245,760 bytes indexed-only.
That fixture used the prior heap-backed slab provider; its post-drain heap
extent is not a measurement of the current PMM-backed slab path. These adapter
results do not predict Pith or physical-hardware throughput. QEMU TCG qualifies
public routing and correctness, not allocator throughput. The focused hybrid
guest test has passed in normal and deep
validation on one and two vCPUs, including all classes, public heap fallback,
overlapping allocation/free work, a dedicated remote-free witness, and bounded
reclamation. A separate two-vCPU e1000 smoke test has received ICMP packets in
both indexed-only and hybrid-256 guests; this establishes functional RX only,
not network throughput. The 256-byte policy is selected for ordinary builds
as the initial unified integration, based on the previously qualified routing
and bounded-memory candidate. The indexed-only build remains available for
comparison and recovery. These host results and functional guest tests do not
establish a general physical-hardware performance advantage; broader system
qualification remains required.

`HYBRID_RUNTIME_DIAGNOSTICS=1` adds format-6 runtime counters only to an
explicit hybrid diagnostic build. It records public routing/context totals,
per-class and per-CPU cache activity, reclaim outcomes, and current/peak slab
occupancy. The extension is allocated from the raw indexed heap, not kernel
image `.bss`; its current payload is 496,232 bytes. Together with the existing
1.5 MiB ordered heap-event buffer, diagnostic profile storage uses 2,069,104
bytes of heap payload. This storage is reported in the snapshot and is absent
from ordinary builds. Class and CPU rows are sampled one lock-protected row
at a time, not at one global instant. The event trace still records indexed
heap operations rather than complete slab-object lifetimes, so format-6
captures support counters and state analysis, not full hybrid trace replay.
These counters are not timing instrumentation and do not establish allocator
latency. Current native profiles do not establish a general system-level
benefit. Earlier instrumented two-vCPU, 384-byte-cutoff runs hit the
scheduler's restored-stack range guard during a later verify boot. In the
bounded comparison, equivalent two-vCPU create/verify/read-only guest runs
completed for indexed-only, uninstrumented hybrid-256/384, and diagnostic
hybrid-256. Two fresh diagnostic-hybrid-384 reruns also passed; another attempt
stopped at the UEFI splash and is incomplete. The original stack mismatch
remains unexplained and is not considered fixed. The scheduler guard remains
fail-stop and now logs CPU, thread, stack range, saved/preemption context, and
thread state before panicking; panic output preserves preceding diagnostics.
Later indexed-only two-vCPU runs reproduced the scheduler guard during MGFS
verification, and another failed to create the Ring 3 test process during
startup. These failures occurred without slab routing and have not been
root-caused. Normal and deep hybrid-256 campaigns completed on both one and
two vCPUs, but the intermittent general scheduler/boot failures remain a
system qualification blocker.

The isolated QEMU e1000 receive test runs a separate Ring 3 network child
instead of coupling packet reception to the clock test. Indexed-only and
hybrid-256 two-vCPU runs each received 16 gateway ICMP replies and advanced
the native interface counters by at least 16 RX and TX packets. The ICMP
objects are allocated and freed through the public kernel network API. Each
echo opens and closes a 48-byte kernel object, routed through the slab backend
in hybrid-256. This establishes guest receive-path behavior with two CPUs
online; the fixture does not prove that device interrupts or allocator calls
executed concurrently on both CPUs, and QEMU TCG is not physical-hardware
performance evidence. The ordinary build now selects hybrid-256, while
indexed-only remains available for comparison and recovery. This receive
fixture is functional qualification, not complete system or performance
qualification.

Historical forced GCC 14.2 target builds measured the indexed-only Pith image at 811,218
bytes `.text`, 50,972 `.rodata`, 61,030 `.data`, and 5,240,944 `.bss` (6,164,195
bytes across those sections). The historical 384-byte hybrid image measured
836,518 `.text`, 55,164 `.rodata`, 61,030 `.data`, and 5,249,744 `.bss`
(6,202,487 bytes total). A hybrid runtime-profile diagnostic image measured
852,873 `.text`, 55,204 `.rodata`, 61,030 `.data`, and 6,249,584 `.bss`
(7,218,722 bytes total). The profile's 496,232-byte runtime extension and
1.5 MiB event buffer are heap allocations; the diagnostic image's additional
`.bss` is the existing bounded profile tracking state, not a reservation-sized
slab classifier. The diagnostic build booted in the guest campaigns described
above; these section totals are not a physical-memory or firmware-capacity
claim.

The default policy retires an empty slab immediately after it has no live or
cached slots. It withdraws classification under the class lock, then the
backing reclaimer defers the page until classifier pins drain before SMP-safe
unmapping and PMM frame release. The optional `SLAB_EMPTY_RESERVE_PER_CLASS=1`
diagnostic variant instead retains at most one empty page per class until a later bounded
pressure pass or quiescent drain. This is bounded to 20 PMM frames (80 KiB)
plus per-class reserve metadata and is not the default policy. Historical host
measurements of the reserve variant showed fewer backing operations but
overlapping timer-quantized timings; they did not justify retaining frames in
the default policy. `slab_drain_empty()` withdraws classification under each
class lock and performs backing teardown only after unlocking. The 80-byte
in-page descriptor and per-slot arrays apply to either reserve build.

Each page has 3,984 bytes after the fixed 112-byte object-area offset. Two
additional per-slot bytes store ownership state and CPU owner. The table shows
the exact slots and remaining page-tail bytes for each class. PMM-backed slab
pages have no indexed-heap block header; classifier and CPU-cache metadata
remain separate indexed-heap allocations.

| Object class (bytes) | Slots per page | Unused object-area tail (bytes) |
| ---: | ---: | ---: |
| 16 | 221 | 6 |
| 32 | 117 | 6 |
| 48 | 79 | 34 |
| 64 | 60 | 24 |
| 80 | 48 | 48 |
| 96 | 40 | 64 |
| 112 | 34 | 108 |
| 128 | 30 | 84 |
| 144 | 27 | 42 |
| 160 | 24 | 96 |
| 176 | 22 | 68 |
| 192 | 20 | 104 |
| 208 | 18 | 204 |
| 224 | 17 | 142 |
| 240 | 16 | 112 |
| 256 | 15 | 114 |
| 288 | 13 | 214 |
| 320 | 12 | 120 |
| 352 | 11 | 90 |
| 384 | 10 | 124 |

Requests are rounded to the first class that can hold them. The maximum
per-object rounding waste is 15 bytes through the 256-byte classes and 31
bytes for the 288- through 384-byte classes. This is separate from per-slot
ownership metadata, page-tail bytes, and free slots retained by a partially
occupied slab.

The classifier uses a bounded three-level index. Its root is a static array
whose length is calculated from the full leased virtual range; each entry covers
1 GiB. A root entry points to a lazily allocated middle table, each middle
entry covers 2 MiB, and a leaf contains 512 64-bit page tokens for that range.
A token contains a five-bit class tag, a 47-bit page generation, an 11-bit
temporary lookup-pin count, and a retiring bit. Zero class means an
unclassified leased page. Retirement clears the class tag and
sets the retiring bit while preserving the generation and outstanding pins.
Backing is returned only after the final pin drains. Publishing a slab at a
reused address advances the generation, so stale cache entries cannot
authorize access to a later page incarnation.

For the current 512 GiB leased range, the root has 512 pointer entries and
occupies 4,096 bytes. Each lazily allocated middle and leaf table has a 4,112
byte payload plus one 48-byte indexed-heap block header. The first classified
2 MiB range therefore retains 8,224 bytes of table payload and 96 bytes of
table block headers; the static root is separate. Each additional 2 MiB range
in an existing 1 GiB range costs one more leaf and heap block header. If every
2 MiB range in the reservation were visited, the retained dynamic tables would
consume 1,080,041,472 payload bytes and 12,607,488 bytes of heap block headers,
in addition to the 4,096-byte static root. This is a theoretical full-coverage
bound, not committed memory at initialization. Eligible public allocations in
the ordinary build route through slabs; indexed-only builds keep public
allocation on the heap and omit the slab module.

Middle and leaf pointers are initialized before atomic publication and retained
for the slab module's lifetime. Losing concurrent creators return their
unpublished raw heap allocations. Published tables are neither relocated nor
reclaimed, so lookup does not depend on table reclamation or a global
classifier lock. A failed lower-level allocation may leave a valid empty
parent table retained for retry. Metadata allocation uses the raw indexed heap
and cannot recurse through slab allocation. The root storage covers the full
virtual reservation while dynamic metadata follows only visited classifier
regions.

The former dense diagnostic map used 134,217,728 BSS bytes for one byte per
possible heap page. The historical indexed-only kernel measurement has
5,240,944 BSS bytes; the
diagnostic slab build has 5,248,240, including its 4,096-byte static root and
class/module state. The diagnostic guest-test build has 5,252,592 BSS bytes;
its test hook adds 4,352 bytes over the diagnostic core. The indexed-only
image omits the slab module and its classifier storage; the ordinary hybrid
image includes it. The compact diagnostic root
avoids the dense map's reservation-sized BSS cost; lazily published middle
and leaf tables remain dynamic heap allocations. These build measurements
and the historical dense and sparse-root artifacts are reported in more
detail in the
[allocator proposal](kernel-allocator-proposal.md). Host pthread
tests exercise production slab operations with host backing adapters; guest
execution is required for Pith behavior. See
[focused test coverage](../development/tests.md#kernel-heap-and-slab-tests).

A historical forced indexed-only target image measured `.text/.rodata/.data/.bss` as
811,218 / 50,972 / 61,030 / 5,240,944 bytes, summing to 6,164,164 bytes.
The historical opt-in hybrid image measured 836,394 / 55,164 / 61,030 / 5,249,744 bytes,
summing to 6,202,332 bytes. Thus this build adds 25,176 text bytes, 4,192
read-only bytes, and 8,800 BSS bytes. At the time of that measurement the
hybrid was opt-in; the current ordinary hybrid build links slab symbols, and
`KERNEL_HYBRID_ALLOCATOR=0` selects indexed-only. The `size -A` totals also
include a 31-byte `.comment` section.
These are image-section measurements, not runtime dynamic classifier or cache
allocations.

The focused one- and two-CPU guest probes retained 12,336 bytes of dynamic
classifier table payload (one middle and two leaf nodes). The three allocated
table blocks also have 48-byte headers; headers for any free split fragments
are additional.
Forcing classification into a second 2 MiB leaf
increased the mapped heap prefix by 1,884,160 bytes in the one-CPU run and
1,835,008 bytes in the two-CPU run. Those PMM-backed heap pages include the
temporary ordinary-heap filler allocations used to reach that address range;
they are test-induced retained mappings, not the classifier's metadata cost
alone. Heap reclamation applies only to fully free, owned four-page groups at
the committed tail; it does not reclaim arbitrary or interior heap pages.

The host fixture compiles the same `slab.c` implementation twice:
`SLAB_DEEP_VALIDATION=0` for the normal slab path and `=1` for deep
validation. Deep mode adds whole-bitmap/population and neighboring-list checks
to each touched operation; it does not change slab layout or ordinary kernel
behavior. Build the diagnostic target with `SLAB_CORE_DIAGNOSTICS=1` and the
selected setting. `SLAB_DEEP_VALIDATION` is rejected unless the diagnostic
module is enabled. `make test-slab` runs both policies with ASan/UBSan, and
`guest_slab_test.py --deep-validation 1` independently checks the guest's
reported mode. The default is zero.

`kmalloc` returns 16-byte-aligned storage or `NULL`. `kfree(NULL)` is a no-op;
other invalid frees panic. Statistics describe payload bytes only, not block
metadata, unmapped heap range, or PMM accounting.

Normal allocation validates all bin-head/bitmap pairs and bounds candidate-list
traversal by the mapped heap's maximum possible block count. Insert, removal,
and coalescing check affected free-index links and membership. Invalid or cyclic
metadata and bad accounting detected by the verifier stop in the kernel panic
path; the allocator never repairs or discards a damaged list. Full physical-
chain, exact free-membership, and accounting verification is an opt-in
diagnostic feature: rebuild with `make -B HEAP_VERIFY=1` (or `gmake` on macOS).
A failed diagnostic verification also panics. These full scans, including a
quadratic exact-membership check, are absent from normal builds.

The host `test-heap` fixture extracts and exercises production allocator
functions, including initial heap setup, with pthread-backed locks and
simulated PMM/VMM mapping. Its ASan/UBSan cases cover split boundaries,
coalescing, alignment, independently reconstructed payload/accounting state,
live-range overlap, pre-scheduler stack-sized growth, bootstrap exhaustion,
partial PMM batches, zero, insufficient, and allocation-sufficient partial VMM
growth, randomized ownership, 20,000 mixed-size allocation/free operations,
and concurrent churn. Its pressure cases distinguish unavailable owned-frame
records from raw backing, inject virtual-lease failure after frame acquisition,
and verify that physical exhaustion leaves a live allocation intact and that
allocation recovers when frames become available. The PMM fixture fills all
2,048 owned-frame records while free physical frames remain and verifies that
the next acquisition fails without changing accounting. The public routing
fixture forces a clean slab backing failure, verifies heap ownership and free
dispatch, then restores PMM supply and confirms slab allocation resumes.
`test-vmm` also executes the production kernel mapping
helper with independently tracked table-frame ownership and injects failures
at each missing paging level. These fixtures do not execute hardware page
tables, scheduler context, or actual SMP shootdowns; only a guest boot
exercises those integrated paths.

A separate host comparison in `tests/memory/heap_benchmark.py` extracts the production
allocation/free paths from the original first-fit revision
(`3288152ffffe130ece618942a0c63612f98989b0`) and the indexed implementation.
Both use the same host PMM/VMM and pthread adapters, GCC `-O0`, no sanitizers,
and no full diagnostic verifier. On the measured x86-64 Linux host, the median
of seven trial percentiles for a 4,096-block alternating-free workload was
61.7/70.4/77.3 microseconds at p50/p95/p99 for first-fit and 0.31/0.34/0.36
microseconds for indexed lookup. A same-bin case with 512 undersized free
blocks and a 1,008-byte request took 15.4/17.1/19.8 microseconds with
first-fit and 23.1/26.4/30.8 microseconds
with indexed lookup. The index avoids scanning allocated blocks in the
fragmented case, while candidate traversal within a populated bin remains
linear and can be slower for this adversarial size mix. In a one-free-block
size-boundary case, allocation p50 was 60 ns for first-fit and 260 ns for the
indexed version. The 30 ns median timer pair was retained rather than
subtracted, so the first-fit result is close to measurement resolution. This
shows additional fast-path checking in the indexed implementation in this
harness, but not a kernel-workload latency estimate. Four host pthread workers
measured 2.50 million allocation/free operations per second for first-fit and
1.66 million for indexed lookup in this run; the elapsed-time ranges overlapped
between trials. Median instrumented heap-lock hold time was 0.29 microseconds
for first-fit and 0.47 microseconds for the indexed allocator. These results
show a cost in this pthread adapter, not Pith's IRQ/spinlock contention, and do
not establish a stable kernel contention penalty. The host results characterize
the extracted code; they are not kernel, QEMU, or hardware latency guarantees.

The indexed header is 48 bytes versus 32 bytes in the earlier allocator, an
additional 16 bytes per physical block. In the 4,097-block fragmented fixture
this is 65,552 additional metadata bytes. The allocator's payload statistics
exclude that cost. A minimum 16-byte payload occupies a 48-byte block with the
old header and a 64-byte block with the indexed header. The index also uses a
fixed 32-pointer head array and a 32-bit occupancy bitmap. In a separate
256 KiB alternating-free pressure case, both versions had 99.1% of reported
free payload outside the largest 1 KiB block; a 4 KiB request failed until the
remaining allocations were freed, after which coalescing restored one usable
block. Allocation sizes and lifetimes in these fixtures are selected stress
patterns, not measured kernel allocation frequencies. Source inspection finds
168 direct `kmalloc` call-site lines, 107 under storage, spanning process/thread,
VFS, IPC, network/device objects, filesystem metadata arrays, 64 KiB thread/AP
stacks, and storage buffers sized from filesystem clusters. This source survey
does not provide runtime size or lifetime frequencies.

Growth timings are reported separately and are not used as a first-fit versus
indexed comparison: heap-growth and mapping-failure behavior changed after the
historical first-fit revision. Benchmark metadata, source hashes, extracted
sources, binaries, raw samples, work counters, and summaries are retained in
the ignored build output directory used for each run. See the test guide for
the reproducible command and measurement boundaries. In the 64 KiB growth
fixture, a 2 MiB virtual reservation grew from 16 to 80 KiB mapped (20 pages),
using 16 data-frame allocations and 16 leaf mappings. This is host-adapter
accounting, not a measurement of page-table memory or production PMM/VMM cost.

## Authoritative code

- `include/address_layout.h`
- `kernel/include/address_space.h` and `kernel/include/vmm.h`
- `kernel/include/heap.h`
- `boot/src/bootstrap.c`
- `kernel/src/address_space.c`, `kernel/src/vmm.c`,
  `kernel/src/elf_loader.c`, and `kernel/src/heap.c`
