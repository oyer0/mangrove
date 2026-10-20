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
512 GiB ioremap interval follows the heap. The current layout leaves the image
at `0xffffffff80000000` in the upper canonical range. No heap-capacity or
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
heap growth. Heap pages are not reclaimed.

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

The kernel-private aligned-page heap operation returns one virtually aligned
4 KiB payload block through the same coalescing heap, accounting, PMM, VMM, and
growth paths. It may create a valid leading free block and applies the ordinary
minimum split threshold to trailing space. The operation does not promise
physical contiguity or DMA suitability. Its paired release uses ordinary heap
free validation. A lock-free committed-range check is available because mapped
heap pages are never unmapped; its published high-water address advances only
after new mappings and block metadata are published.

## Slab core and optional hybrid routing

`kernel/src/slab.c` is omitted from the default indexed-only kernel. It is
included by an isolated core-test build (`SLAB_CORE_DIAGNOSTICS=1`) or by the
explicit `KERNEL_HYBRID_ALLOCATOR=1` configuration. The latter initializes
the slab core after SMP startup and enables the optional hybrid routing
described below. `KERNEL_HYBRID_ALLOCATOR` defaults to zero; ordinary builds
remain indexed-heap-only unless the option is explicitly selected.

The private slab API supports twenty classes: 16-byte increments through 256
bytes, then 288, 320, 352, and 384 bytes. Each 4 KiB virtual slab page has a
72-byte descriptor and 32-byte allocation bitmap; object payload starts at
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
slab is unlinked, declassified under its class lock, then returned to the heap
after unlocking. The sparse page classifier and descriptor checks reject bad
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
free path validates the heap range and committed page, uses the first page
token only to select a class lock, reloads and compares the complete
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
cached entries and their uniqueness, walks class lists and committed heap
pages, validates descriptors and bitmap words, and reconstructs caller,
cached, refill, partial, full, and empty counts. It is not run implicitly
after every normal-mode operation. Classifier nodes remain allocated for the
module lifetime, so a lookup that has loaded a node pointer never races its
reclamation. A reused page is checked against its newly published generation
and class tag. As with other raw-pointer allocator interfaces, a stale pointer
whose address has since been reallocated to a new live object cannot be
distinguished from that new object's pointer.

### Per-CPU cache core

`slab_cpu_alloc()` and `slab_cpu_try_free()` are kernel-private APIs. The
indexed-only build does not call them from ordinary `kmalloc()`/`kfree()`;
the explicit hybrid build routes eligible requests through them. The default
cache holds at most two objects per discovered CPU/class and refills two at a
time. Each CPU has one 64-byte-aligned record per class. At these defaults a
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
the cache record is held. NMI-context allocation is unsupported. Refill and
backing growth happen with no cache lock held and retain the heap's existing
non-sleepable-context failure behavior.

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
page, classification is withdrawn under the class lock. Both locks are
released before the pin wait and indexed-heap backing release. The existing
bounded classifier pin wait fails closed on timeout. The same bounded pass
retires optional empty reserves. It never invokes slab allocation while
draining.

After a slab backing allocation fails, the slab allocator makes one bounded
pressure pass (at most 128 cache records and 32 objects), then retries the
aligned heap backing request only if a slab or empty-reserve page was actually
returned to the heap. If objects were returned without releasing a page, the
existing shared-class recheck can use newly available slots of the requested
class. A busy lock, unsupported held-spinlock context, or no progress leaves
the allocation on the existing failure path; there is no retry loop. This
pressure path releases capacity to the indexed heap, not to PMM: committed
heap mappings and their physical frames remain owned by the heap.

`slab_cpu_drain_all()` remains a quiescent diagnostic operation: a complete
drain requires callers to stop all slab allocation/free users first. It locks
each record and class in that order and retires now-empty slabs through the
same classifier pin protocol. `slab_drain_empty()` is likewise for quiescent
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

### Optional hybrid `kmalloc` routing

Build with `KERNEL_HYBRID_ALLOCATOR=1` to include and initialize the slab core
after SMP startup. `KERNEL_HYBRID_ALLOCATOR_CUTOFF` selects the largest request
routed to `slab_cpu_alloc()`; supported values are 256, 288, 320, 352, and 384
bytes, with 384 as the optional-build default. The ordinary build still sets
`KERNEL_HYBRID_ALLOCATOR=0`. A clean slab unavailability or backing failure
falls back to the indexed heap; slab corruption remains a fatal allocator
error. Requests above the selected cutoff always use the indexed heap. The
public free path first asks the
generation-aware slab classifier; classified pointers are validated and freed
by the slab backend, while unclassified pointers go through indexed-heap
validation. Raw heap allocation/free interfaces bypass routing and remain the
ownership path for classifier nodes, cache records, and slab backing.

Before slab initialization and in the default `KERNEL_HYBRID_ALLOCATOR=0`
build, all public allocations use the indexed heap. If slab initialization
fails, the optional hybrid build remains indexed-only for that boot; no slabs
have yet been published. Once initialized, routing is a boot-lifetime choice:
turning it off while slab objects are live is not a supported rollback.
Zero-size allocations return `NULL`, and requests that cannot fit the heap
reservation do not trigger reclamation.

When an indexed-heap allocation fails, the hybrid dispatcher makes at most
one bounded `slab_reclaim_bounded()` pass. For a small request it retries the
slab path only if cached objects were returned; it retries the heap path only
if slab backing was returned to the heap. It does not repeat heap growth or
wait for busy CPU records. The reclaim path uses raw heap release and never
calls public `kmalloc`, so it cannot recurse through the dispatcher. It is
disabled for a call already holding a tracked spinlock. Interrupt-disabled
callers may use existing slab/cache/heap capacity, but reclamation cannot
sleep or grow the heap. Returning slab backing makes that virtual heap block
reusable; heap pages remain mapped and PMM frames are not returned.

This configuration is optional, not the default allocator selection. The
source-matched indexed-only Pith profile measured 46.47% of successful
allocation requests at or below 384 bytes, representing 0.293% of requested
bytes in the captured two-vCPU boot, Shoot, Reed, and MGFS scenario. Its
commit-ordered trace includes 18,435 successful allocations, 14,230 frees,
4,205 live objects at snapshot, and 5,198 cross-CPU frees. A modeled 256-byte
cutoff covers 39.15% of requests and 0.125% of requested bytes; 288 bytes
covers 44.29% and 0.237%. The ideal-packed demand model estimates 51 slab pages
for either cutoff and 53 at 384 bytes. These are lower-bound demand models
from the trace, not observed slab occupancy; they exclude per-CPU cached slots
and alignment gaps.

Five GCC 14.2 `-O2` host trials with four pinned, overlapping workers measured
16-/72-byte immediate-pair throughput of 4.68M/4.81M pairs/s for the public
384-byte hybrid versus 3.73M/3.57M indexed-only. Cutoffs 256 and 288 measured
4.77M/4.65M and 4.74M/4.61M. Single-worker 16-byte pair medians were 134.4 ns
indexed-only, 118.5 ns full hybrid, and 120.5/120.6 ns for cutoffs 256/288;
at 384 bytes the corresponding medians were 140.3, 126.4, 178.5, and 163.6 ns.
The size-frequency replay uses immediate lifetimes and is not a lifetime
trace. A separate replay of the full two-vCPU Pith event order measured median
costs of 105.4 ns per allocator event indexed-only, 106.4 ns shared slab,
132.5 ns direct per-CPU slab, and 144.8 ns for the public 384-byte hybrid.
Public 256- and 288-byte cutoffs measured 141.7 and 151.3 ns per event.
Five-trial ranges were broad and overlapping. This replay preserves size,
object lifetime/order, phase, and free CPU, but runs serially on the host and
does not reproduce Pith concurrency, migration, interrupts, or timing.

The 1,024-live-object host fixture mapped 344,064 bytes at cutoff 384 and
262,144 bytes at cutoffs 256/288, versus 245,760 bytes indexed-only. After
draining, all slab backing was returned to heap capacity, while the mapped
high-water extent remained. These adapter results do not predict Pith or
physical-hardware throughput. QEMU TCG qualification confirms public routing,
two-CPU cross-free behavior, and bounded reclaim, not allocator throughput or
NIC receive traffic. The indexed-only build therefore remains the normal
configuration; the hybrid and cutoff variants remain explicit qualification
options.

The default diagnostic policy retires an empty slab immediately: it clears
the page's class tag while holding the class lock, then returns the aligned
backing block to the indexed heap after unlocking. An optional diagnostic
build variant, `SLAB_EMPTY_RESERVE_PER_CLASS=1`, retains at most one empty page
per class. A failed backing request may drain reserves for classes reached by
its bounded pressure scan before one retry. This is bounded to 20 pages (80
KiB of slab backing, in addition to heap block metadata) and is not the default
policy. The host lifecycle fixture showed much lower backing-operation counts
but timer-quantized, overlapping per-operation timings, so it does not justify
retaining that memory in the default policy. `slab_drain_empty()` withdraws
classification under each class lock and releases backing only after
unlocking; it is a quiescent diagnostic helper. The reserve variant has
production-source host coverage but has not been guest-qualified. Its class
state adds an 8-byte reserve pointer and two 64-bit counters per class (480
bytes across 20 classes); it is disabled by default. The 72-byte in-page
descriptor and per-slot arrays apply to either reserve build.

Each page has 3,984 bytes after the fixed 112-byte object-area offset. Two
additional per-slot bytes store ownership state and CPU owner. The table shows
the exact slots and remaining page-tail bytes for each class; the separate
48-byte heap block header and any valid leading/trailing split blocks remain
part of the backing heap's accounting.

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
whose length is calculated from the full heap reservation; each entry covers
1 GiB. A root entry points to a lazily allocated middle table, each middle
entry covers 2 MiB, and a leaf contains 512 64-bit page tokens for that range.
A token contains a five-bit class tag, a 47-bit page generation, an 11-bit
temporary lookup-pin count, and a retiring bit. Zero class means ordinary
heap storage or an unclassified page. Retirement clears the class tag and
sets the retiring bit while preserving the generation and outstanding pins.
Backing is returned only after the final pin drains. Publishing a slab at a
reused address advances the generation, so stale cache entries cannot
authorize access to a later page incarnation.

For the current 512 GiB heap reservation, the root has 512 pointer entries and
occupies 4,096 bytes. Each lazily allocated middle and leaf table has a 4,112
byte payload plus one 48-byte indexed-heap block header. The first classified
2 MiB range therefore retains 8,224 bytes of table payload and 96 bytes of
table block headers; the static root is separate. Each additional 2 MiB range
in an existing 1 GiB range costs one more leaf and heap block header. If every
2 MiB range in the reservation were visited, the retained dynamic tables would
consume 1,080,041,472 payload bytes and 12,607,488 bytes of heap block headers,
in addition to the 4,096-byte static root. This is a theoretical full-coverage
bound, not committed memory at initialization. Normal allocation remains on
the indexed heap, and the diagnostic slab module is omitted from ordinary
builds.

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
possible heap page. The current ordinary kernel has 5,240,944 BSS bytes; the
diagnostic slab build has 5,248,240, including its 4,096-byte static root and
class/module state. The diagnostic guest-test build has 5,252,592 BSS bytes;
its test hook adds 4,352 bytes over the diagnostic core. The ordinary image
omits the slab module and its classifier storage. The compact diagnostic root
avoids the dense map's reservation-sized BSS cost; lazily published middle
and leaf tables remain dynamic heap allocations. These build measurements
and the historical dense and sparse-root artifacts are reported in more
detail in the
[allocator proposal](kernel-allocator-proposal.md). Host pthread
tests exercise production slab operations with host backing adapters; guest
execution is required for Pith behavior. See
[focused test coverage](../development/tests.md#kernel-heap-and-slab-tests).

A forced indexed-only target image measured `.text/.rodata/.data/.bss` as
811,218 / 50,972 / 61,030 / 5,240,944 bytes, summing to 6,164,164 bytes.
The opt-in hybrid image measured 836,394 / 55,164 / 61,030 / 5,249,744 bytes,
summing to 6,202,332 bytes. Thus this build adds 25,176 text bytes, 4,192
read-only bytes, and 8,800 BSS bytes; the ordinary default still has no slab
symbols. The `size -A` totals also include a 31-byte `.comment` section.
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
alone. The allocator does not return committed heap pages to PMM.

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
and concurrent churn. `test-vmm` also executes the production kernel mapping
helper with independently tracked table-frame ownership and injects failures
at each missing paging level. These fixtures do not execute hardware page
tables, scheduler context, or actual SMP shootdowns; only a guest boot
exercises those integrated paths.

A separate host comparison in `tests/heap_benchmark.py` extracts the production
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
