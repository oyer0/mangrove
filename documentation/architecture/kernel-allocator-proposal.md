# Pith kernel allocator architecture proposal

**Status:** architecture proposal; no allocator changes are implemented by this document.

The current allocation contract remains in [kernel memory architecture](memory.md). The measured indexed-heap comparison and its qualification limits are also recorded there. This proposal separates current facts, retained measurements, and candidate design decisions.

## Executive decision

Recommend a conservative hybrid allocator:

- Keep the existing 32-bin coalescing heap as the backing allocator and as the path for requests above the small-object cutoff.
- Add size-class slabs for common small allocations behind the existing kmalloc and kfree interface.
- Back each slab with one page-sized, page-aligned allocation from the existing heap. Return a fully empty slab to that heap after dropping the slab lock.
- Preserve the present bootstrap, heap-growth, PMM/VMM ownership, and non-sleepable-context rules.
- Begin with one IRQ-safe lock per size class, no per-CPU object cache, and no page reclamation to PMM. Add CPU-local caching only after measurements justify the extra ownership and draining rules.

This is candidate B, hybrid slab plus indexed heap. It has a narrower integration boundary than a new page-span allocator and uses the existing heap’s tested mapping, rollback, and growth behavior. The class range below is an initial experiment, not a frozen policy. A target-side allocation-size and lifetime profile must confirm or revise it before the first production implementation.

**Verdict: DESIGN CONDITIONALLY READY.** The preferred architecture and invariants are precise enough to begin implementation preparation. Before committing to the class cutoff and fast-path organization, collect a bounded source-matched Pith size/lifetime profile and a baseline of per-class contention. No evidence currently establishes that a particular size distribution or per-CPU cache policy matches real Mangrove workloads.

This verdict does not claim Linux, XNU, or Windows performance parity. A slab fast path is a testable hypothesis for small objects; Pith guest and, eventually, comparable hardware measurements must establish its value.

## Current Mangrove requirements

### Source-grounded allocation inventory

The current source contains 168 direct kmalloc call expressions in 21 kernel C files; 107 are under storage. These are source counts, not runtime allocation frequencies. The expressions fall into these observed groups:

| Consumer | Source evidence | Shape and lifetime evidence | Allocator implication |
| --- | --- | --- | --- |
| Process, thread, and CPU setup | kernel/src/process.c, kernel/src/scheduler.c, kernel/src/smp.c, kernel/src/cpu.c | Process and thread records, CPU descriptors, topology arrays, and 64 KiB kernel/AP stacks. Stack size is explicit; many object byte sizes are compiler sizeof expressions and must be measured under the target ABI. | Small records are slab candidates if measured sizes fall under the selected cutoff. Stacks remain large heap allocations. CPU startup has a bootstrap dependency. |
| VFS, object, IPC, and syscall state | kernel/src/vfs.c, kernel/src/object.c, kernel/src/ipc.c, kernel/src/syscall.c | Fixed records and variable-capacity snapshots or handle arrays. Some objects survive for an open handle or mount lifetime; transaction and request state is temporary. | Fixed records may benefit from classes; variable arrays remain on the heap. Lifetime and sizes require instrumentation. |
| Networking and device state | kernel/src/net/, kernel/src/pci/, kernel/src/usb/ | Network user objects and device records use kmalloc; packet data is often inline or in bounded structures. Driver rings and receive buffers use dedicated paths. | Do not merge hardware-visible buffers or device ownership into a generic slab merely because a request is small. |
| MGFS, FAT32, exFAT, and formatting | kernel/src/storage/mgfs.c, fat32.c, exfat.c, format.c | Many allocations are records and transaction metadata; variable arrays and I/O buffers are sized from filesystem metadata. MGFS block I/O uses 4 KiB blocks; FAT32 cluster buffers can be up to 64 KiB; exFAT accepts clusters up to 1 MiB. | Small records are candidates; cluster and transaction buffers retain the large-allocation path. Input-controlled counts must keep their existing checked arithmetic and failure handling. |
| Boot and image display | kernel/src/init.c, kernel/src/framebuffer.c, kernel/src/aml.c | Bootstrap and ACPI consumers include large or variable buffers. The framebuffer backbuffer size depends on display dimensions. | Bootstrap remains on the existing heap path. Allocation before scheduler/interrupt readiness must not require a sleeping refill. |
| DMA and page-table ownership | kernel/src/dma.c, kernel/src/usb/xhci_mem.c, kernel/src/usb/xhci_storage.c, kernel/src/vmm.c | xHCI's object allocator uses one zeroed PMM frame and rejects requests above one page. Its storage path can separately request a 32 KiB `dma_alloc` buffer, but verifies each page's physical address and a 64 KiB boundary before using that path. AHCI uses `dma_alloc`, which reserves alignment space through kmalloc and reports the physical translation of the aligned starting address. VMM ownership nodes use small kmalloc records. | Preserve DMA and page-table ownership APIs. Ordinary heap virtual contiguity is not physical contiguity. Audit each device’s actual DMA requirements separately; do not infer a generic DMA guarantee from kmalloc or dma_alloc. |

A source-level survey does not reveal allocation rates, peak simultaneous object counts, or lifetime distributions. Some lifetimes are evident from ownership paths, such as a thread record lasting through thread teardown or a filesystem transaction ending after a mutation, but runtime frequency and overlap must be measured.

### Existing pools and specialized allocators

Pith has fixed-capacity registries and caches, including the IPC request pool, UDP endpoints, ARP entries, block cache entries, device tables, GPT tables, and per-filesystem MGFS dentry-cache entries. These structures serve bounded protocols, identity, or lookup policies. They are not general-purpose size-class allocators and should keep their current semantics.

xHCI's object allocator is a single-frame PMM-backed DMA allocator; its optional 32 KiB storage buffer uses the separately checked `dma_alloc` path. The generic `dma_alloc` wrapper is a distinct interface whose caller and hardware assumptions need to be checked independently. The slab design does not replace either path.

### Current indexed heap

The current implementation is in kernel/include/heap.h and kernel/src/heap.c. Its public contract is kmalloc(size), kfree(ptr), and payload-capacity statistics.

- kmalloc returns 16-byte-aligned storage; zero or unrepresentable requests fail with NULL. kfree(NULL) is a no-op, and invalid non-null frees panic.
- Each physical block has a 48-byte header. Free blocks are indexed in 32 logarithmic bins; allocated blocks are absent from the bins. Neighboring physical blocks coalesce.
- A split requires room for another header and at least 32 payload bytes. The heap grows in four-page groups inside a 2 GiB virtual reservation. Mapped heap pages are not returned to PMM.
- The BSP has a bounded pre-interrupt bootstrap growth window. Normal growth uses a mutex after checking that interrupts are enabled and the current CPU holds no spinlock. An allocation satisfied from existing free capacity does not sleep.
- PMM supplies zeroed frames; VMM mapping and shootdown publish kernel mappings. A successful mapped prefix is retained and can satisfy one retry. Failure must not be disguised as success or followed by an unbounded retry.
- Heap block metadata is in-band. Corrupt heap state fails closed. Full-chain and exact-membership diagnostics are opt-in.
- Heap statistics describe allocated/free block payload capacity and exclude block headers. They are not requested-byte or live-object counters.

The 32-bin index materially improved one fragmented search workload over the historical first-fit source, while the same-bin search remains linear. The retained host-adapter evidence reports a fragmented 4,096-block allocation p50/p95/p99 of 61.7/70.4/77.3 microseconds for first-fit and 0.31/0.34/0.36 microseconds for indexed lookup. A constructed same-bin case was slower with the index: 15.4/17.1/19.8 microseconds versus 23.1/26.4/30.8 microseconds. The four-pthread host run favored first-fit in aggregate throughput, but trial ranges overlapped. These are extracted production functions under a host adapter, not Pith timing or evidence about actual kernel size frequency. The exact source revisions, workload seeds, commands, raw samples, and limitations are retained in build/stage4e-heap-20261018/benchmark-final-qualified/.

The current 48-byte block header costs 16 bytes more per block than the historical 32-byte header. That is expensive for small objects: a 16-byte payload uses a 64-byte block including current metadata. The heap’s coalescing and 16-byte alignment remain useful for arbitrary-size requests and as a slab backing allocator.

## Established allocator designs

The references below describe public design principles and documented APIs. They are research, not source to incorporate.

- The Linux memory allocation guide separates small object allocation, virtually contiguous large allocation, direct page allocation, and context-dependent reclaim behavior. Linux’s current memory API documents SLUB cache operations and CPU-local object batching; the precise internals and available paths change by kernel version. The versioned SLUB guide also documents configurable checking, poisoning, red zones, and failure injection. See [Linux Memory Allocation Guide](https://docs.kernel.org/core-api/memory-allocation.html), [Linux Memory Management APIs](https://www.kernel.org/doc/html/latest/core-api/mm-api.html), and [SLUB user guide](https://kernel.org/doc/html/v6.7/mm/slub.html).
- XNU documents page-granularity VM allocation, fixed-size zones, and a variable-size kalloc layer built from size classes with larger allocations routed to VM. Its startup guide makes allocator initialization ordering explicit. These are XNU contracts, not Pith requirements. See [XNU allocator API basics](https://github.com/apple-oss-distributions/xnu/blob/main/doc/allocators/api-basics.md) and [XNU startup ordering](https://github.com/apple-oss-distributions/xnu/blob/main/doc/lifecycle/startup.md).
- FreeBSD UMA describes zones for identical-size objects, slab backing from virtual pages, bounded per-CPU caches, a shared backing structure, and reclaim hooks. This illustrates that per-CPU caches and slab-page reclamation are separate responsibilities, not free additions to a size-class allocator. See [FreeBSD uma(9)](https://man.freebsd.org/cgi/man.cgi?query=uma&sektion=9).
- Microsoft documents lookaside lists as fixed-size buffer caches that can reduce calls into general pool allocation, and documents pool allocation behavior, alignment, and execution-context requirements. These public driver contracts do not reveal or justify claims about all internal Windows pool size-class implementation details. See [ExInitializeLookasideListEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exinitializelookasidelistex), [ExAllocateFromLookasideListEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exallocatefromlookasidelistex), [ExAllocatePool2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exallocatepool2), and [Windows kernel memory allocation references](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/_kernel/).
- Bonwick’s original slab paper presents object caches, state retention, coloring, and allocator diagnostics as a design family. Later magazine/cache work discusses scaling global locks and managing arbitrary resources. See [The Slab Allocator (USENIX 1994)](https://www.usenix.org/conference/usenix-summer-1994-technical-conference/slab-allocator-object-caching-kernel) and [Magazines and vmem (USENIX 2001)](https://www.usenix.org/conference/2001-usenix-annual-technical-conference/magazines-and-vmem-extending-slab-allocator-many).

Across these designs, the reusable principles are size-class reuse, explicit backing-page ownership, a shared refill/reclaim layer, bounded local caching when SMP measurements justify it, and distinct interfaces for allocation context and device constraints. The data structures and policies are implementation-specific.

## Candidate comparison

| Candidate | Advantages | Costs and risks | Fit for current Pith |
| --- | --- | --- | --- |
| A. Keep indexed heap only | Smallest change; arbitrary sizes; existing growth and rollback tests remain directly applicable; coalescing reuses freed blocks. | Per-object header cost remains; global IRQ-safe lock and same-bin scans remain; fragmentation can reject a large request despite high aggregate free payload; pages are retained. | A sound fallback and baseline, but no longer the best-supported small-object path after the measured metadata/search tradeoff. |
| B. Small slabs over indexed heap; indexed heap for larger requests | Removes a 48-byte heap block header per small client object; bounded bitmap allocation within a slab; existing heap retains arbitrary-size allocation, PMM/VMM ownership, and bootstrap growth. Empty slabs can return as ordinary heap blocks. | Adds class rounding, page-level partially-live retention, a page-aligned backing operation, class metadata, free-pointer classification, and lock-order rules. Large requests retain current heap fragmentation and 48-byte headers. It does not return mapped pages to PMM. | Recommended first architecture. Reuses the tested memory path and permits rollback to the current implementation behind the same kmalloc interface. |
| C. Slabs plus a page-backed large-span allocator | Separates variable-size runs from small-object blocks, can reclaim fully unused mappings and PMM frames, and avoids heap block headers for page runs. | Requires a virtual-run allocator, authoritative per-page ownership, mapping rollback and shootdown lifetime, descriptor bootstrapping, concurrency-safe unmap/reuse, and a large-object migration audit. A virtually contiguous span is not necessarily physically contiguous. | A plausible later architecture if measured large-allocation fragmentation or retained pages warrant it. Not justified as the initial change because it duplicates VMM/heap ownership work and raises the failure surface. |
| D. Explicit typed object caches | Can specialize alignment, constructors, diagnostics, and type separation for stable structures. | Requires per-type API migration, accurate constructors/destructors, cache lifetime rules, and size/lifetime evidence. It does not replace variable-size kmalloc and would create many caches if adopted indiscriminately. | Not the first step. Consider only for demonstrated high-frequency stable types or a real isolation requirement. |

## Proposed hybrid design

### Public interface and ownership

Keep kmalloc(size), kfree(ptr), and their source and ABI contracts unchanged. No caller supplies a cache pointer or size on free. The allocator must recover slab ownership from the returned pointer and otherwise use the existing heap block validation.

Requests over the small-object cutoff go to the indexed heap. Allocations are virtual kernel storage with the current 16-byte minimum alignment; neither the heap nor the slab interface promises physically contiguous memory, DMA suitability, zeroing, or cache-line alignment. Device-visible memory remains owned by the appropriate DMA/PMM path.

A slab is an internal allocation class. No filesystem, VFS, device, or userspace API learns slab layout.

### Initial size-class experiment

Use 4 KiB slabs and a provisional 24-class table:

- 16-byte increments from 16 through 256 bytes.
- 32-byte increments from 288 through 512 bytes.
- Requests are first rounded to the existing 16-byte minimum alignment, then mapped to the first class that fits.
- Requests above 512 bytes use the current indexed heap.

The cutoff is provisional. This table keeps the first implementation’s slab page occupancy useful: after metadata, a 512-byte class fits seven objects in a 4 KiB slab, while the largest class leaves at most 416 bytes of unused tail. It avoids pretending that one-page slabs are efficient for 1 KiB objects. Target profiles may justify adding classes or using multi-page slabs later.

For normalized sizes, size-class rounding adds at most 16 bytes in the 16-byte-spaced range or 16 bytes in the 32-byte-spaced range, because normalized requests are multiples of 16. Relative to the original unrounded request, the total unused bytes are bounded by 31 bytes for requests through 512 bytes. This excludes slab header, bitmap, free tail, and page-level retention, which are reported separately.

### Slab representation and pointer lookup

Each active slab owns one 4 KiB page-aligned payload block from the existing heap. Add an internal, checked page-aligned block allocation path; do not expose a new public kernel API. It must split a leading free fragment only when that fragment can represent a valid heap block, preserve the existing 16-byte physical-chain invariant, and maintain the same payload accounting.

At the slab page base, reserve a 64-byte descriptor and a 32-byte allocation bitmap. The object area begins at offset 96 and every slot is class-size aligned, therefore at least 16-byte aligned. The smallest class has at most 250 slots, which fits in the bitmap’s 256 bits. Unused high bits must remain clear. The descriptor records a format tag, class index, slot count, allocated count, state, list links, and integrity value; it does not replace the heap block header immediately before the aligned page.

Use a static atomic byte per possible heap virtual page to classify slab ownership. The current 2 GiB heap reservation contains 524,288 pages, so this map costs 512 KiB. Zero means ordinary heap page; values 1 through 24 identify a slab class. This lets `kfree` choose the persistent class lock without first dereferencing a descriptor that another CPU may be retiring. Reject an out-of-range pointer before indexing the map. After acquiring the class lock, re-read the page entry and validate it before touching the in-page descriptor; retirement clears the entry while holding that same lock, before releasing the backing block to the heap. Initialize the descriptor and bitmap before publishing the class value with release ordering; classify with acquire ordering. Distinct page entries require atomic access because different class locks can publish pages in the same map. The larger map avoids a global lookup lock or a lockless read of reclaimable in-page metadata. Measure its kernel BSS cost before adoption.

Each size class has a partial-slab list protected by its own IRQ-safe spinlock. Full slabs need no allocation-list entry but remain classifiable through the page map and descriptor. A slab becomes partial when one slot is freed and full when its last free slot is allocated.

A free validates page ownership, descriptor tag and class, object-area bounds, exact slot alignment, slot index, and the allocated bit. Interior pointers and immediate double frees fail closed. The descriptor and bitmap are in-band and remain vulnerable to memory corruption; validation must panic on impossible state rather than repairing it. As with the current raw-pointer `kfree` contract, no allocator can distinguish a stale pointer from a new live allocation after the same address has been reused.

When a slab reaches zero allocated objects, remove it from its class list, clear its page-map bit, release the class lock, then return the aligned backing block to the indexed heap. The first implementation keeps no empty slab cache. This bounds cache-retained empty backing to zero and makes empty slab memory reusable by large allocations. If measurement later shows refill churn, a bounded warm-empty policy may be proposed with explicit accounting and pressure draining.

### Slab allocation, refill, and lock order

A small-object allocation takes the relevant class lock, checks its partial list, and allocates one free bitmap bit using a bounded scan of that slab’s bitmap. It does not acquire heap_lock while holding the class lock.

On a class miss, release the class lock before requesting one page-aligned heap block. The underlying heap allocator retains its current execution-context behavior: it may use already-free capacity in non-sleepable contexts, but any growth still observes the bootstrap window, interrupt state, spinlock-depth check, scheduler, and growth mutex. If backing allocation cannot proceed, kmalloc returns NULL. It must never sleep while holding the class lock.

Initialize a new slab privately before classifying or linking it. Then take the class lock, allocate the requested slot, publish the map bit with release ordering, and link the slab if it has remaining slots. Concurrent missers may each obtain a backing page; each resulting slab must either serve its requester or be returned to the heap after dropping the class lock. No class lock is held over PMM/VMM mapping, heap growth, scheduler progress, or backing-block free.

A small-object free takes only its identified class lock to validate and update the bitmap/list. If it retires the slab, it releases that lock before calling kfree on the backing page. A large-object free uses only the indexed heap lock. No path nests class locks or holds a class lock while acquiring heap_lock. The one-lock-at-a-time rule prevents a heap-growth/cache-refill lock cycle.

The existing kmalloc semantics remain important in interrupt and spinlock contexts. A hit in an existing slab is bounded and non-sleeping. A cache miss may use a page already available in the heap, but cannot grow the heap from a held-spinlock context. If it cannot obtain backing under the existing heap rules, the request fails. This preserves the present NULL-on-unavailable-growth behavior; it does not reserve emergency objects for interrupt use.

### Statistics and memory pressure

Keep current heap statistic meanings: total/used/free continue to report heap block payload capacity. A live slab consumes one allocated page-sized heap block regardless of how many client objects are live, so these statistics remain backing-capacity measures, not live requested bytes. Add separate diagnostic counts for per-class live slots, partial/full slabs, slab-page payload, class rounding waste, and unoccupied slots. Do not silently reinterpret the existing statistics.

Per slab page, the object area loses 96 bytes to descriptor and bitmap. The page-aligned heap backing block also has the existing 48-byte heap header before the page. Thus the slab-specific in-page overhead is 2.34% of a 4 KiB page, plus the existing backing-block header and class/tail waste. The 512 KiB page-classification table is fixed static memory for the entire 2 GiB virtual reservation, including uncommitted pages. Measure its effect on kernel BSS and image size.

A partially occupied slab remains unavailable to other classes and to large allocations until it becomes empty. Track this separately from class rounding waste. With no empty cache, a completely unused slab is immediately returned to the heap’s coalescing bins. The heap still does not unmap pages or return their frames to PMM. Exhaustion returns NULL according to current heap behavior; there is no silent fallback to an unbounded buffer, new page pool, or repeated growth loop.

### Bootstrap and initialization

Slab cache descriptors, locks, and page-classification map are static. Initialize all class locks and clear the map as part of heap initialization before any kmalloc can route through slabs. Keep the current early-boot path available for pre-scheduler objects and stacks.

A small bootstrap kmalloc may use a newly backed slab only if its internal page-aligned heap allocation is safe in the existing BSP-only bootstrap window. Static slab metadata must not rely on a heap allocation or per-CPU record. On each context where backing cannot be obtained without scheduler progress, return NULL. Do not enable interrupts early, extend the bootstrap window, or introduce a second pool as part of the slab integration.

### SMP evolution

The initial slab version uses a lock per class and supports remote frees through that shared class state. Lock hold time is bounded by a partial-list update, at most 256 bitmap bits (four 64-bit words), and integrity checks. It has no per-CPU freelist, owner-CPU rule, migration-sensitive cache, or remote-free queue. The scheduler and spinlock contract already prevents sleeping or migration while a spinlock is held.

Only add per-CPU magazines after the shared-lock implementation has guest and stress evidence showing material contention. A later design must specify:

- Per-CPU structures initialized before a CPU can allocate, with a safe central fallback during BSP bootstrap and AP startup.
- Preemption/interrupt exclusion while selecting the current CPU’s cache, while preserving the existing IRQ and lock-depth rules.
- Bounded capacity and exact retained bytes per CPU and class.
- A batch refill/drain protocol that cannot expose one object to two CPUs.
- Remote frees routed to authoritative slab ownership or a synchronized shared path.
- A live-versus-cached slot state so pages with pointers in a CPU cache cannot be reclaimed.
- Bounded draining during memory pressure and any future CPU-offline operation. CPU hotplug is not currently supported, but startup and shutdown assumptions still require explicit ownership.
- Lock ordering with the slab class lock and heap backing path; no sleeping or PMM/VMM work under a per-CPU critical section.

For the proposed 24-class table, a hypothetical magazine capacity of four objects per class would retain at most 21,504 bytes of object payload per online CPU, before slab-page waste. At the current 256-CPU discovery bound, the upper bound would be about 5.25 MiB if all CPUs reached that capacity. These figures are a policy cost estimate, not a recommendation to enable magazines.

### DMA, large objects, and page-backed alternatives

The initial hybrid does not provide DMA memory, physically contiguous memory, page alignment for ordinary kmalloc, or more than the existing 16-byte alignment guarantee. Do not route a request to slabs solely because its requested byte count is small if a call site needs a physical or cache-alignment property. Keep xHCI’s PMM frame path and audit AHCI/dma_alloc requirements independently before allocator migration.

Large and variable-size requests continue through the current coalescing heap. Existing large-allocation fragmentation and lack of PMM reclamation therefore remain. Candidate C would require a separate page-run allocator with a free-run map, per-run metadata, detached mapping publication, and a VMM unmap path whose remote TLB completion precedes PMM frame reuse. The page run would promise contiguous virtual addresses only unless a separate contiguous-frame API is used. It must not be implemented as a quick fallback from a partially failed heap request.

## Measurement and performance qualification

### Workload profile before class freeze

Add a diagnostic-only, bounded profile build. Do not add logging, dynamic allocation, time reads, or locks to the ordinary allocator build.

For the profile build:

- Maintain fixed static counters for requested-size buckets, returned-class counts, and allocation failures. Derive live-byte high-water offline from a complete event trace rather than adding per-object tracking to the allocator.
- Record a bounded event ring containing operation, pointer, the size information available for that operation (requested size on allocation and class or usable capacity on free), CPU index, and a global sequence number. Publish successful allocation records before returning their pointers and free records before releasing storage for reuse; use an atomic sequence reservation so reuse has a well-defined trace order without synchronized TSCs. Use no kprint on the allocator path. Stop recording or overwrite with an explicit lost-record counter at the configured capacity; any lost record makes lifetime reconstruction incomplete.
- Keep the ring and counters outside kmalloc-managed memory. Expose or dump a snapshot only after profiling is disabled and outside allocator locks.
- Use representative boot, process creation/exit, Shoot/Reed, filesystem, and network workloads. Report that the profile build changes timing and possibly memory layout. It is for size/lifetime distribution, not latency.
- Treat missing frees, pointer reuse, ring overflow, incomplete workloads, and profile overhead explicitly. Do not infer wall-clock lifetime from operation sequence distance.

The profile should establish whether 16-byte-aligned request sizes cluster below 512 bytes, how many simultaneously live slab candidates occur per class, whether long-lived small objects pin pages, and what small/large allocation ratios appear in actual guest workloads. It cannot prove future application demand.

### Three-way controlled benchmark

Compare historical first-fit, current indexed, and slab-plus-indexed versions using the retained allocator benchmark harness where it remains equivalent, and extend it only where necessary. Pin exact source hashes, compiler, flags, host, CPU affinity, warm-up, timer source, seed, sample count, and initial heap state. Use the same production allocator paths and the same PMM/VMM/lock adapter for versions that have equivalent behavior. Do not compare a mock slab to production heap code.

Retain the current unoptimized GCC -O0 host comparison for continuity. Add a separate optimized host configuration if it is representative of the actual target flags. The current kernel C flags do not set a global optimization level; record actual per-object flags rather than assuming a release configuration. Keep sanitizers and full verifiers out of latency runs and run correctness separately.

Workloads must include:

- Single-thread small allocations by every candidate class, cache hit and refill separately.
- Small object churn with realistic source-derived class mixes and multiple live-lifetime distributions.
- Empty, partly occupied, and many-partial-slab states.
- Large 4 KiB, 64 KiB, and multi-page requests.
- The historical fragmented and adversarial same-bin scenarios.
- Failure and recovery with finite heap capacity.
- One and multiple CPUs, with cross-CPU frees once a later per-CPU design exists.
- Allocation and free latency separately, with p50/p95/p99 and full trial ranges.
- Heap metadata lock and class lock timing as separate instrumented runs.
- Requested payload, class capacity, slab metadata, heap backing header, partial-slab slack, static profile/map bytes, mapped frames, and virtual reservation.

Use equal initial capacity and live object sequences. Reset the full allocator state between trials. Do not subtract timer overhead unless both versions use the same documented calibration method; include the floor in reports.

### Guest and other-kernel comparison

A target guest run must use actual Pith kmalloc paths and verify process completion. QEMU TCG can qualify execution and relative same-guest behavior, not physical-hardware competitive performance. For each target change, run both one-CPU and two-vCPU workloads where useful; neither substitutes for physical SMP evidence.

An eventual Linux or other-kernel comparison needs equivalent hardware, firmware, CPU frequency policy, core affinity, compiler optimization, object-size/lifetime trace, allocation context, payload touching, and warm-up. Use a small purpose-built test path for each kernel and report allocator-specific latency and memory metrics separately from whole-system startup or filesystem behavior. Match sleepable and atomic contexts as separate tests, since Linux’s GFP behavior and Pith’s growth policy differ. Do not claim a universal allocator ranking from different kernel services or from QEMU timings.

## Acceptance criteria

Correctness and preservation of current kernel guarantees are release blockers:

1. Existing kmalloc callers build without source changes, except individually reviewed users that need an explicit alignment/DMA contract.
2. No live objects overlap; every allocated slot belongs to one slab and one class; page-map bits, slab descriptors, class lists, bitmaps, and counts agree.
3. Invalid, interior, misaligned, and repeated frees fail closed. Every metadata transition is bounded and corruption is diagnostic; no repair or silent fallback.
4. Class refill and page retirement never hold a slab lock over heap growth, PMM/VMM work, scheduler progress, or a blocking mutex.
5. Bootstrap, IRQ-disabled, and spinlock-held behavior remains compatible: a cached hit is bounded and nonblocking; a miss can fail if the current heap cannot provide backing without growth.
6. All page backing comes from one auditable heap allocation and is returned exactly once after the slab has no live objects. Failure before publication returns its backing allocation.
7. Large allocations retain current error, alignment, virtual-contiguity, and ownership behavior.
8. DMA and other special physical constraints never become implicit consequences of a size class.

Provisional performance goals, to be confirmed after baseline profile:

- Improve uninstrumented p50 for the most common small classes by at least 20% over indexed kmalloc in a controlled single-CPU test, with a statistically separated interval across repeated trials.
- Do not regress large allocation p95 by more than 5% in the same host adapter; slab routing should not enter those paths.
- Under two or more CPUs, improve aggregate small-object operations or reduce lock contention without increasing p99 latency by more than 10% versus the indexed baseline.
- Report small-object committed memory and peak partial-slab retention; do not accept a latency win that causes unexplained or unbounded heap growth. The first version has no empty-slab cache, but partial-slab retention remains workload-dependent.
- Do not claim host goals imply target hardware performance. On guest, require no correctness regression and compare only within the same QEMU configuration.

The percentages are provisional engineering gates, not results supported by current evidence. If a gate is missed, inspect class choice, lock contention, and backing policy before changing the gate.

## Incremental implementation and qualification plan

Each review unit should keep the kernel bootable and preserve a route back to the current indexed heap. A unit is not complete because code exists; it needs its listed evidence.

| Review unit | Scope and dependency | Acceptance and focused evidence | Guest evidence and stop condition |
| --- | --- | --- | --- |
| 0. Profile and ABI census | Add temporary or build-gated counters; compile target sizeof probes for common kmalloc object types. No allocator behavior change. Depends on current baseline. | Bounded event capture, exact class demand, profile-loss negative control, target struct sizes, no normal-build overhead or BSS change. | Run representative boot and native workloads on one and two vCPUs. Stop if the profile is incomplete or workload coverage cannot identify a useful class range. |
| 1. Aligned heap backing primitive | Add internal page-aligned block allocation/free support to the current heap, without slab routing. | Production-source tests for exact page alignment, prefix/suffix splitting, minimum fragments, end-of-heap boundaries, accounting, overflow, failure, bootstrap, held-lock, and invalid free. Existing heap tests remain passing. | Focused boot smoke because the primitive underlies future slabs. Stop on any change to current public allocation semantics or PMM/VMM ownership ambiguity. |
| 2. Static slab metadata and classifier | Add fixed class descriptors, per-class locks, in-page descriptor/bitmap, static page classification map, and verifier only; kmalloc still uses old path. | Host tests independently reconstruct page/class ownership; test bad descriptors, bad page classes, bitmap boundaries, classification racing with slab retirement, corruption, no recursion, and BSS cost. Target link confirms static-memory size. | Boot and allocator diagnostics; no slab allocation yet. Stop if the map/metadata memory cost is material or lookup cannot fail closed. |
| 3. Small allocation and free routing | Route selected sizes to slabs, retain indexed heap for larger requests. | Production-source tests for all classes, class rounding, full/partial/empty transitions, concurrency, duplicate refill, remote free, exact pointer checking, failure atomicity, and isolated corruption children. ASan/UBSan and negative controls. | Guest kernel test allocates/frees all classes and verifies heap stats and process survival. Roll back routing if large paths, boot, or existing apps regress. |
| 4. Bootstrap and allocation-context integration | Exercise slab initialization and misses in the BSP bootstrap window and normal IRQ/spinlock contexts. | Tests prove a slab hit works without sleep, miss uses existing backing only in non-sleepable contexts, growth obeys current guard, and initialization does not recurse. | Fresh boot must reach native userspace; early scheduler and AP stacks still use large path. Stop if allocator initialization depends on a scheduler object it allocates. |
| 5. Empty-slab release and pressure recovery | Return fully empty slab backing to indexed heap; no PMM unmapping. Test growth and fragmented pressure. | Deterministic live-anchor workloads prove only truly empty pages are released, page map clears before reuse, heap coalesces the backing block, and large requests can reuse it. Inject free and growth failures. | Guest storage/process churn followed by a large request. Stop if a page can be reused while any object or pending free still references it. |
| 6. Per-CPU cache experiment | Optional, only if measured per-class lock contention justifies it. Add bounded local batches and shared refill/drain; no scheduler API change. | Cross-CPU allocation/free barriers, CPU migration, batch duplication, cache accounting, interrupt context, pressure drain, AP startup, and bounded retention tests. Compare against shared-lock version. | Fresh two-vCPU native stress; do not claim broad SMP readiness. Stop if ownership, draining, or allocation-context rules cannot be proven. |
| 7. Reclamation design decision | Assess page-backed span allocator and PMM return only from measurements. Separate architecture review before any implementation. | Evidence quantifies retained mapped pages, partial slabs, external fragmentation, and large allocation failures. Design must specify VMM shootdown and PMM ownership before code. | No guest behavior change in that review unit. Do not start unless memory-pressure evidence demonstrates a material need. |
| 8. Stress and three-way comparison | Long-run fixed-seed mixed-size, adversarial fragmentation, exhaustion/recovery, and repeated trials versus both historical and current code. | Independent ownership model, sanitizer tests, raw reproducible measurements, latency distributions, footprint and failure-recovery results. No tolerance relaxation. | Guest run verifies actual native kernel paths and output. Stop on unexplained corruption, memory leak, unstable measurements, or material large-object regressions. |
| 9. System integration | Qualify processes, filesystem, storage, network, services, and shutdown with the final production configuration. | Focused MTS host groups, forced target build, existing allocator/VMM/PMM/sync regression groups, complete host profile where required, independent artifact validation. | Two-vCPU networking-enabled guest with process and filesystem work; verify completion and persisted content. This is system qualification, not physical-hardware proof. |

### Rollback boundary

Keep the indexed heap path independently buildable until the slab version has passed host and guest acceptance. A diagnostic configuration may route all sizes back to the indexed heap without changing callers. Do not add an automatic runtime fallback from corrupt slab metadata to the general heap; that would hide ownership errors. A clean allocation failure may return NULL, but an invariant violation must stop in the existing panic path.

## Risks, unknowns, and non-goals

### Implementation blockers to resolve before production routing

- The class table needs real target sizeof and runtime size/lifetime data; source expressions alone do not establish a beneficial cutoff.
- The 512 KiB static classifier table and its bounds, atomic publication, and reset during initialization need target-link and fault-injection proof.
- Page-aligned splitting must coexist with the existing physical-neighbor chain and 48-byte block header; ordinary kmalloc must remain unchanged.
- The current kernel heap and PMM growth path have bounded non-sleepable semantics. A slab miss is not guaranteed to succeed in an interrupt handler or while a spinlock is held.
- DMA call sites must retain explicit device contracts. In particular, investigate AHCI’s dma_alloc usage separately; its current implementation reports only the physical translation of the aligned virtual start address, which by itself does not establish multi-page physical contiguity.
- A class lock does not make descriptor lifetime safe if a page-map bit is cleared or published outside the specified ordering.
- The current scheduler allows thread migration when not running and has no CPU hotplug. Any future local cache needs explicit CPU-local selection and remote-free ownership.

### Deferred but not required for the first hybrid

- Returning slab frames and heap frames to PMM through VMM unmap after remote TLB acknowledgements.
- Multi-page slab geometry, named typed caches, NUMA placement, per-CPU magazines, object constructors, object coloring, zero-on-free, quarantine, randomized reuse, guard pages, and cache-line separation.
- A generic allocation-context flag family or emergency reserves.
- Changing PMM’s frame scan/zeroing or VMM locking as a side effect of allocator work.
- Changing DMA interfaces, filesystem allocation policy, process ABI, or the 32-bin design without independent evidence.

## References and Mangrove evidence

Internal sources and contracts:

- [Memory layout and current heap contract](memory.md)
- [Pith SMP and synchronization](smp.md)
- [Focused test coverage](../development/tests.md)
- [MTS inventory and guest evidence](../development/test-suite.md)
- [kernel/include/heap.h](../../kernel/include/heap.h) and [kernel/src/heap.c](../../kernel/src/heap.c)
- [kernel/src/pmm.c](../../kernel/src/pmm.c), [kernel/src/vmm.c](../../kernel/src/vmm.c), [kernel/src/spinlock.c](../../kernel/src/spinlock.c), and [kernel/src/mutex.c](../../kernel/src/mutex.c)
- [kernel/src/dma.c](../../kernel/src/dma.c), [kernel/src/usb/xhci_mem.c](../../kernel/src/usb/xhci_mem.c), and [kernel/src/usb/xhci_storage.c](../../kernel/src/usb/xhci_storage.c)
- [kernel/src/process.c](../../kernel/src/process.c), [kernel/src/scheduler.c](../../kernel/src/scheduler.c), [kernel/src/smp.c](../../kernel/src/smp.c), [kernel/src/vfs.c](../../kernel/src/vfs.c), [kernel/src/object.c](../../kernel/src/object.c), [kernel/src/ipc.c](../../kernel/src/ipc.c), [kernel/src/net/](../../kernel/src/net/), and [kernel/src/storage/](../../kernel/src/storage/)

Retained benchmark evidence and source hashes:

- build/stage4e-heap-20261018/benchmark-final-qualified/metadata.json
- build/stage4e-heap-20261018/benchmark-final-qualified/summary.json
- build/stage4e-heap-20261018/benchmark-final-qualified/raw.jsonl
- [tests/heap_benchmark.py](../../tests/heap_benchmark.py)

The benchmark artifacts are ignored build evidence. This proposal does not change their interpretation or claim a kernel performance result.
