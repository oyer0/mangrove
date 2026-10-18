# Pith kernel allocator architecture proposal

**Status:** the selected slab core exists in isolated diagnostic builds; ordinary allocation still uses the indexed heap. This document records its design and remaining integration constraints.

The current allocation contract remains in [kernel memory architecture](memory.md). The measured indexed-heap comparison and its qualification limits are also recorded there. This proposal separates current facts, retained measurements, and candidate design decisions.

## Executive decision

Recommend a conservative hybrid allocator:

- Keep the existing 32-bin coalescing heap as the backing allocator and as the path for requests above the small-object cutoff.
- Add size-class slabs for common small allocations behind the existing kmalloc and kfree interface.
- Back each slab with one page-sized, page-aligned allocation from the existing heap. Return a fully empty slab to that heap after dropping the slab lock.
- Preserve the present bootstrap, heap-growth, PMM/VMM ownership, and non-sleepable-context rules.
- Begin with one IRQ-safe lock per size class, no per-CPU object cache, and no page reclamation to PMM. Add CPU-local caching only after measurements justify the extra ownership and draining rules.

This is candidate B, hybrid slab plus indexed heap. It has a narrower integration boundary than a new page-span allocator and uses the existing heap’s tested mapping, rollback, and growth behavior. A one-page slab and the existing heap as backing and large-object path are selected for the first implementation. The class range below is based on bounded Pith guest profiles; it remains subject to implementation and workload-acceptance checks.

**Decision:** Two source-matched Pith TCG profiles justify a bounded first implementation using a 20-class table through 384 bytes, a 4 KiB slab, a dense page-classification map, and one shared lock per class. The evidence covers one MGFS workload on one and two vCPUs, not every kernel workload or future application. It does not measure contention on locks that do not yet exist. The indexed path remains the reference; any later CPU-local cache requires measured class-lock contention and separately proven ownership rules.

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
- A split requires room for another header and at least 32 payload bytes. The heap grows in four-page groups inside the 512 GiB virtual reservation defined by the current address-layout constants. Mapped heap pages are not returned to PMM.
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

### Initial size-class decision

Use 4 KiB slabs and 20 size classes:

- 16-byte increments from 16 through 256 bytes.
- 32-byte increments at 288, 320, 352, and 384 bytes.
- Requests are first rounded to the existing 16-byte minimum alignment, then mapped to the first class that fits.
- Requests above 384 bytes use the current indexed heap.

The two guest traces recorded 18,288 and 18,237 successful allocations. A 384-byte cutoff covered 46.06% and 45.89% of allocation count, but only 0.286% and 0.283% of requested bytes. Raising the cutoff to 512 bytes admitted exactly seven additional requests in each trace, all 512-byte requests, and added one ideal-modeled peak slab page; it did not change accumulated class-rounding waste. That small count gain does not justify the four extra classes and additional modeled page in this workload. The 384-byte class holds ten objects after the 96-byte descriptor/bitmap area, with 160 bytes of page tail. The 20-class table is the bounded initial choice, not a claim that one-page slabs are optimal for all applications.

For normalized sizes, class rounding adds at most 16 bytes in the 16-byte-spaced range and 16 bytes in the 32-byte-spaced range, because normalized requests are multiples of 16. Relative to the original unrounded request, unused bytes are bounded by 31 bytes through the 384-byte cutoff. This excludes slab descriptor/bitmap, free tail, and page-level retention, which are reported separately.

### Slab representation and pointer lookup

Each active slab owns one 4 KiB page-aligned payload block from the existing heap. The internal `heap_alloc_aligned_page`/`heap_free_aligned_page` path uses the indexed heap's block chain and accounting. A leading fragment is created only when it can hold the existing minimum split payload; suffix splitting uses the same threshold as ordinary allocation. The production-source heap tests cover alignment, prefix/suffix boundaries, accounting, and coalescing. The operation does not promise physically contiguous or DMA-safe memory.

At the slab page base, reserve a 64-byte descriptor and a 32-byte allocation bitmap. The object area begins at offset 96 and every slot is class-size aligned, therefore at least 16-byte aligned. The smallest class has at most 250 slots, which fits in the bitmap’s 256 bits. Unused high bits must remain clear. The descriptor records a format tag, class index, slot count, allocated count, state, list links, and integrity value; it does not replace the heap block header immediately before the aligned page.

Use a static atomic byte per possible heap virtual page to classify slab ownership in the diagnostic core. The actual constants span 512 GiB, or 134,217,728 pages, so this table is 134,217,728 bytes (128 MiB, 32,768 4 KiB pages). Zero means ordinary heap page; values 1 through 20 identify one of the selected classes. Forced target links measured 5,240,944 bytes of BSS in the ordinary image and 139,461,296 bytes in the diagnostic image; `nm` reports the 128 MiB map and 2,560-byte class-control array separately. The map alone is one quarter of the 512 MiB profile VM's RAM. This is a substantial cost, not the 512 KiB estimated in the original proposal. The diagnostic build is isolated and the default kernel omits the slab module and map. The array is zero-initialized BSS and one-shot `slab_init` does not clear it. Its size and page-table/image-load consequences make production activation a separate measured decision; the dense representation remains implemented for isolated core tests, not approved as an unconditional permanent production cost.

This first representation favors independent atomic byte loads/stores and simple fail-closed lookup over minimum BSS. A tightly packed 5-bit representation encodes the 21 states in 83,886,080 bytes (80 MiB), saving 48 MiB. A 12-entry-per-64-bit-word layout uses 89,478,488 bytes (about 85.33 MiB), with four spare bits per word. Both require masked atomic updates, including compare/exchange when different class locks update entries in one word. A sparse or committed-region table needs its own descriptor lifetime, bootstrap, and safe retirement protocol and must not recurse into the heap. The dense byte table covers 32,768 pages of kernel image virtual memory if every entry's backing is touched. Actual lookup locality, cache misses, and TLB pressure have not been measured. The much smaller packed map has nontrivial shared-word update complexity; the much larger byte map is absent from normal builds, and its use after activation remains an explicit resource decision.

The byte lookup lets `kfree` choose the persistent class lock without first dereferencing a descriptor that another CPU may be retiring. Reject out-of-range pointers before indexing. After acquiring the class lock, re-read and validate the page entry before touching the in-page descriptor; retirement clears the entry under that same lock before returning the backing block to the heap. Initialize the descriptor and bitmap before publishing the class value with release ordering; classify with acquire ordering. Distinct page entries are independently atomic even when different class locks publish adjacent pages. The map does not remove the need to validate descriptor, slot, and allocation state.

The diagnostic `slab_verify()` checks each class under its lock and scans only the mapped heap prefix. It does not walk the uncommitted portion of the large virtual reservation.

Each size class has a partial-slab list protected by its own IRQ-safe spinlock. Full slabs need no allocation-list entry but remain classifiable through the page map and descriptor. A slab becomes partial when one slot is freed and full when its last free slot is allocated.

A free validates page ownership, descriptor tag and class, object-area bounds, exact slot alignment, slot index, and the allocated bit. Interior pointers and immediate double frees fail closed. The descriptor and bitmap are in-band and remain vulnerable to memory corruption; validation must panic on impossible state rather than repairing it. As with the current raw-pointer `kfree` contract, no allocator can distinguish a stale pointer from a new live allocation after the same address has been reused.

When a slab reaches zero allocated objects, remove it from its class list, clear its page-map bit, release the class lock, then return the aligned backing block to the indexed heap. The first implementation keeps no empty slab cache. This bounds cache-retained empty backing to zero and makes empty slab memory reusable by large allocations. If measurement later shows refill churn, a bounded warm-empty policy may be proposed with explicit accounting and pressure draining.

### Slab allocation, refill, and lock order

A small-object allocation takes the relevant class lock, checks its partial list, and allocates one free bitmap bit using a bounded scan of that slab’s bitmap. It does not acquire `heap_lock` while holding the class lock. The private `slab_alloc` API currently exercises this path; ordinary `kmalloc` does not route through it.

On a class miss, release the class lock before requesting one page-aligned heap block. The underlying heap allocator retains its current execution-context behavior: it may use already-free capacity in non-sleepable contexts, but any growth still observes the bootstrap window, interrupt state, spinlock-depth check, scheduler, and growth mutex. If backing allocation cannot proceed, `slab_alloc` returns `NULL`; a later `kmalloc` dispatcher must preserve the existing failure contract. It must never sleep while holding the class lock.

Initialize the unpublished descriptor and clear its bitmap while the aligned backing page is exclusively owned by the heap allocation. Then reacquire the class lock and recheck the partial list: a competing allocator may already have created a slot. If so, use that slot and return the extra backing page after dropping the lock. Otherwise mark the new slab's first slot, compute its integrity value, publish its class tag with release ordering, and link it as partial. The descriptor and bitmap are initialized before classification; readers acquire-load the tag and recheck it under the class lock before dereferencing the descriptor. No class lock is held over PMM/VMM mapping, heap growth, scheduler progress, or backing-block free.

A small-object free takes only its identified class lock to validate and update the bitmap/list. If it retires the slab, it releases that lock before calling kfree on the backing page. A large-object free uses only the indexed heap lock. No path nests class locks or holds a class lock while acquiring heap_lock. The one-lock-at-a-time rule prevents a heap-growth/cache-refill lock cycle.

The existing kmalloc semantics remain important in interrupt and spinlock contexts. A hit in an existing slab is bounded and non-sleeping. A cache miss may use a page already available in the heap, but cannot grow the heap from a held-spinlock context. If it cannot obtain backing under the existing heap rules, the request fails. This preserves the present NULL-on-unavailable-growth behavior; it does not reserve emergency objects for interrupt use.

### Statistics and memory pressure

Keep current heap statistic meanings: total/used/free continue to report heap block payload capacity. A live slab consumes one allocated page-sized heap block regardless of how many client objects are live, so these statistics remain backing-capacity measures, not live requested bytes. The dormant `slab_class_stats_t` currently reports allocation/free/failure counts, bitmap words examined, backing acquisitions/releases, full/partial transitions, retired slabs, live/peak slots, and current partial/full slab counts. It does not retain each object's requested size, so it cannot report exact live requested bytes or accumulated class-rounding waste. The host benchmark computes those costs only for its known request sizes. Do not silently reinterpret the heap statistics.

Per slab page, the object area loses 96 bytes to descriptor and bitmap. The page-aligned heap backing block also has the existing 48-byte heap header before the page. Thus the slab-specific in-page overhead is 2.34% of a 4 KiB page, plus the existing backing-block header and class/tail waste. The core's host tests reconstruct class slot counts and the target build confirms 128 bytes of static control state per class; the class stats expose partial/full slab counts for measuring retained backing. The 128 MiB classifier is fixed static memory for the entire 512 GiB virtual reservation, including uncommitted heap addresses; it exists only in the isolated slab-core build. The production aligned split may also introduce leading and trailing heap headers and free prefix capacity, which the host fixture measures.

A partially occupied slab remains unavailable to other classes and to large allocations until it becomes empty. Track this separately from class rounding waste. With no empty cache, a completely unused slab is immediately returned to the heap’s coalescing bins. The heap still does not unmap pages or return their frames to PMM. Exhaustion returns NULL according to current heap behavior; there is no silent fallback to an unbounded buffer, new page pool, or repeated growth loop.

### Bootstrap and initialization

Slab descriptors, locks, and the page-classification map are static. The dormant core's one-shot `slab_init(heap_start, heap_capacity)` initializes class locks; the map starts zero from BSS and is not cleared at runtime. Normal heap initialization does not call it. The default target excludes the module, and ordinary `kmalloc`/`kfree` remain indexed-heap only. Keep the current early-boot path available for pre-scheduler objects and stacks.

A future small-object `kmalloc` route may obtain a new slab during bootstrap only if its internal page-aligned heap allocation is safe in the existing BSP-only bootstrap window. Static slab metadata must not rely on a heap allocation or per-CPU record. On each context where backing cannot be obtained without scheduler progress, return `NULL`. Do not enable interrupts early, extend the bootstrap window, or introduce a second pool as part of slab integration.

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

For the selected 20-class table, a hypothetical magazine capacity of four objects per class would retain at most 14,080 bytes of object payload per online CPU, before slab-page waste. At the current 256-CPU discovery bound, the upper bound would be 3,604,480 bytes (about 3.44 MiB) if all CPUs reached that capacity. These figures are a policy cost estimate, not a recommendation to enable magazines.

### DMA, large objects, and page-backed alternatives

The initial hybrid does not provide DMA memory, physically contiguous memory, page alignment for ordinary kmalloc, or more than the existing 16-byte alignment guarantee. Do not route a request to slabs solely because its requested byte count is small if a call site needs a physical or cache-alignment property. Keep xHCI’s PMM frame path and audit AHCI/dma_alloc requirements independently before allocator migration.

Large and variable-size requests continue through the current coalescing heap. Existing large-allocation fragmentation and lack of PMM reclamation therefore remain. Candidate C would require a separate page-run allocator with a free-run map, per-run metadata, detached mapping publication, and a VMM unmap path whose remote TLB completion precedes PMM frame reuse. The page run would promise contiguous virtual addresses only unless a separate contiguous-frame API is used. It must not be implemented as a quick fallback from a partially failed heap request.

## Measurement and performance qualification

### Diagnostic workload profile

The diagnostic-only implementation is in `kernel/src/heap_profile.c` and is included only when `HEAP_PROFILE_DIAGNOSTICS=1`. The normal target build filters that translation unit and contains no profiler calls, symbols, syscall, or static data. The diagnostic kernel uses a fixed snapshot and a bounded, allocation-free live-pointer table; this configuration adds 999,648 bytes of BSS (475,312-byte snapshot plus 524,288-byte live table and small state). No allocator-path logging, filesystem access, blocking lock, timestamp read, or dynamic allocation is used.

The profiler deliberately does not use an event ring or atomic sequence reservation. Allocation success/failure and free hooks commit while the existing heap metadata lock is held; the operation counter advances at that commit point. This is the allocator's actual serialization order, so a delayed request that entered earlier cannot be mistaken for a completed earlier lifetime. Profile counters are therefore serialized by the existing heap lock, while the admission/inflight state uses bounded atomic operations. The fixed pointer table preserves original requested size, allocation CPU, phase, and commit operation. Address reuse starts a new tracked lifetime. Table probe limits, missing frees, counter saturation, inflight-at-freeze, and phase errors set invalidation flags; the offline parser rejects any incomplete snapshot rather than estimating through missing records. Freeze closes admission while holding the heap lock, then snapshots inflight state. Snapshot export is a private diagnostic syscall and copies bounded chunks only after freeze.

Snapshots include exact request-size counts through 1 KiB, logarithmic buckets above that, success/failure and requested-byte totals, live/peak objects and requested bytes, per-CPU allocation/free counts, cross-CPU frees, phase survivors, operation-count lifetime buckets, and candidate-class demand at allocator commit points. Lifetime is measured in successful allocator operation commits, not elapsed time. Candidate page counts are computed as `ceil(live objects / slots per page)` for each class. This is an ideal-packing lower bound: the traces do not assign objects to real slabs and cannot measure partially occupied slab retention caused by object lifetime layout. The sum of independent per-class peaks is an upper bound only on simultaneous ideal-packed demand, not on eventual allocator page use. Production-source slab tests must measure actual partial-slab slack. The diagnostic profile changes timing and memory layout, so it is evidence about demand and ownership overlap, not latency or production performance.

The profile build ran the MGFS guest harness with native service startup, process creation/exit, Shoot, Reed edit/save/Save As/quit, filesystem operations, and read-only verification on one and two vCPUs. The e1000 device remained enabled. The profile-specific Reed interaction omits the external UDP rendezvous/stale-conflict test, and the profile workload does not establish meaningful NIC receive traffic. FAT32/exFAT workloads and other native applications were not profiled. These traces support a bounded first class table but do not represent all Mangrove workloads or future ports.

### Target profile evidence

The reproducible profile artifacts are ignored under `build/stage4e1b-profile-final-1vcpu-v4/` and `build/stage4e1b-profile-final-2vcpu/`. Both reports passed; each phase has its own binary snapshot hash and the reports record identical binary hashes for Pith, Shoot, Reed, FSTest, and the boot image. The guest runner independently extracted the profile from stopped MGFS media, checked snapshot format and completeness flags, and ran the MGFS integrity checker. The one-vCPU run also passed the wrong-content negative control. Both two-vCPU reports observed two online CPUs. These are TCG guest results, not physical-hardware timing or SMP scalability results.

| Create/workload profile | 1 vCPU | 2 vCPU |
| --- | ---: | ---: |
| Successful requests / failures | 18,288 / 0 | 18,237 / 0 |
| Frees / tracked live at snapshot | 14,158 / 4,130 | 14,131 / 4,106 |
| Peak tracked live objects | 4,307 | 4,283 |
| Requests above 1 KiB | 9,807 (53.6%) | 9,809 (53.8%) |
| Requested bytes above 1 KiB | 228,887,096 (99.70%) | 228,977,400 (99.70%) |
| Cross-CPU frees | 0 | 4,215 (29.8% of frees) |
| 384-byte candidate allocation-count coverage | 46.06% | 45.89% |
| 384-byte candidate requested-byte coverage | 0.286% | 0.283% |
| Modeled ideal-packed pages at simultaneous peak | 50 pages | 49 pages |
| Sum of independent per-class ideal peak pages | 62 pages | 60 pages |

The 384-byte candidate's ideal-packed page payload at the modeled simultaneous peak is 204,800 bytes on one CPU and 200,704 bytes on two CPUs. Including one existing 48-byte heap block header per slab gives a baseline backing estimate of 207,200 and 203,056 bytes respectively. The independent per-class ideal-peak sum is 62 and 60 pages, plus 2,976 and 2,880 bytes of one-header-per-page overhead: 256,928 and 248,640 bytes. This is a conservative sum of per-class ideal demand, not an observed simultaneous footprint or an upper bound on real slab pages. Actual partial-slab fragmentation can increase backing use. Both estimates exclude additional heap headers for representable alignment-prefix/suffix fragments and do not model actual heap free-space shape; those costs must be measured by the aligned backing implementation. They estimate a hypothetical slab allocator, not measured current heap consumption.

The boot/service phase left 2,852 tracked objects at the native-workload boundary in the one-CPU trace and 2,855 in the two-CPU trace. Phase survivors included small requests and large long-lived buffers. This shows that slab candidates can coexist with multi-megabyte large allocations; it does not mean small allocations account for comparable byte volume. The 2-vCPU trace recorded activity in both CPUs for 10 of the 20 classes through 384 bytes. Those masks show both CPUs used a class at some point, not overlapping lock acquisition. Cross-CPU free activity is a real ownership proxy (1,751 frees in these classes) but not a measurement of contention on proposed slab locks.

Of completed lifetimes in the create profiles, 8,908/14,158 (62.9%) and 8,869/14,131 (62.8%) ended in fewer than 16 subsequent allocator commits. 1,324 and 1,333 lifetimes (about 9.4%) lasted at least 1,024 subsequent commits. These operation-distance buckets identify both rapid churn and long-lived allocations, but they are activity-relative; they are not durations. At the phase boundary, 2,796 objects in the 384-byte candidate classes remained live in each profile, while those small objects account for only 2,796 of 2,852/2,855 survivors and a small share of the roughly 5.3 MiB survivor bytes.

The profiler evaluated 256, 512, 768, and 1,024-byte cutoffs as well. Moving from 384 to 512 routed only the seven 512-byte calls in each create trace, left cumulative class-rounding waste unchanged (32,777 bytes / 32,417 bytes), and increased modeled ideal-packed simultaneous demand by one page (50 to 51 / 49 to 50). At 768 and 1,024, request-count coverage rose only to 46.16% and 46.38% on one CPU (45.99% and 46.21% on two), while modeled ideal-packed simultaneous demand rose to 53 and 60 pages (52 and 60). Most requested bytes are in large requests handled by the existing heap, so cutoff selection is about operation count and modeled live-object demand, not byte volume.

In the 2-vCPU create trace, 10 of 20 classes had allocation/free activity attributed to both CPUs. Under the 384-byte cutoff, 1,751 of 4,320 frees were cross-CPU. The busiest small class was 16 bytes (4,295 allocations and 1,438 cross-CPU frees), followed by 80 bytes (1,913 allocations, 205 cross-CPU frees) and 288 bytes (947 allocations). This justifies shared class ownership that accepts remote frees. It does not prove simultaneous contention. Start with one IRQ-safe lock per class and no per-CPU cache; measure actual lock waits after the class locks exist.

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

## Implementation sequence

Keep the kernel bootable and preserve a route back to the current indexed heap throughout implementation. Each change needs its listed evidence before small-object routing proceeds.

| Work area | Scope and dependency | Acceptance and focused evidence | Guest evidence and stop condition |
| --- | --- | --- | --- |
| Aligned heap backing | Add internal page-aligned block allocation/free support to the current heap, without slab routing. | Production-source tests for exact page alignment, prefix/suffix splitting, minimum fragments, end-of-heap boundaries, accounting, overflow, failure, bootstrap, held-lock, and invalid free. Existing heap tests remain passing. | Focused boot smoke because the primitive underlies future slabs. Stop on any change to current public allocation semantics or PMM/VMM ownership ambiguity. |
| Static slab metadata and classifier | Add fixed class descriptors, per-class locks, in-page descriptor/bitmap, static page-classification map, and verifier only; kmalloc still uses the indexed path. | Host tests independently reconstruct page/class ownership; test bad descriptors, bad page classes, bitmap boundaries, classification racing with slab retirement, corruption, no recursion, and BSS cost. Target link confirms static-memory size. | Boot and allocator diagnostics; no slab allocation yet. Stop if map/metadata cost is material or lookup cannot fail closed. |
| Small allocation and free routing | Route selected sizes to slabs, retain indexed heap for larger requests. | Production-source tests for all classes, class rounding, full/partial/empty transitions, concurrency, duplicate refill, remote free, exact pointer checking, failure atomicity, and isolated corruption children. ASan/UBSan and negative controls. | Guest kernel test allocates/frees all classes and verifies heap stats and process survival. Roll back routing if large paths, boot, or existing apps regress. |
| Bootstrap and allocation-context integration | Exercise slab initialization and misses in the BSP bootstrap window and normal IRQ/spinlock contexts. | Tests prove a slab hit works without sleep, miss uses existing backing only in non-sleepable contexts, growth obeys current guard, and initialization does not recurse. | Fresh boot must reach native userspace; early scheduler and AP stacks still use large path. Stop if allocator initialization depends on a scheduler object it allocates. |
| Empty-slab release and pressure recovery | Return fully empty slab backing to indexed heap; no PMM unmapping. Test growth and fragmented pressure. | Deterministic live-anchor workloads prove only truly empty pages are released, page map clears before reuse, heap coalesces the backing block, and large requests can reuse it. Inject free and growth failures. | Guest storage/process churn followed by a large request. Stop if a page can be reused while any object or pending free still references it. |
| Per-CPU cache evaluation | Consider only if measured per-class lock contention justifies it. Any design needs bounded local batches and shared refill/drain, with no scheduler API change unless justified. | Cross-CPU allocation/free barriers, CPU migration, batch duplication, cache accounting, interrupt context, pressure drain, AP startup, and bounded retention tests. Compare against shared-lock version. | Fresh two-vCPU native stress; do not claim broad SMP readiness. Stop if ownership, draining, or allocation-context rules cannot be proven. |
| Page reclamation decision | Assess page-backed spans and PMM return only from measurements; require a separate architecture review before implementation. | Evidence quantifies retained mapped pages, partial slabs, external fragmentation, and large allocation failures. Design must specify VMM shootdown and PMM ownership before code. | No guest behavior change. Do not add reclamation unless memory-pressure evidence demonstrates a material need. |
| Stress and allocator comparison | Long-run fixed-seed mixed-size, adversarial fragmentation, exhaustion/recovery, and repeated trials versus both historical and current code. | Independent ownership model, sanitizer tests, raw reproducible measurements, latency distributions, footprint and failure-recovery results. No tolerance relaxation. | Guest run verifies actual native kernel paths and output. Stop on unexplained corruption, memory leak, unstable measurements, or material large-object regressions. |
| System integration | Qualify processes, filesystem, storage, network, services, and shutdown with the production configuration. | Focused MTS host groups, forced target build, existing allocator/VMM/PMM/sync regression groups, broader host qualification where required, and independent artifact validation. | Two-vCPU networking-enabled guest with process and filesystem work; verify completion and persisted content. This is system qualification, not physical-hardware proof. |

### Rollback boundary

Keep the indexed heap path independently buildable until the slab version has passed host and guest acceptance. A diagnostic configuration may route all sizes back to the indexed heap without changing callers. Do not add an automatic runtime fallback from corrupt slab metadata to the general heap; that would hide ownership errors. A clean allocation failure may return NULL, but an invariant violation must stop in the existing panic path.

## Risks, unknowns, and non-goals

### Implementation invariants and remaining qualification

- The selected classes are supported by one MGFS profile workload on one and two vCPUs, not FAT32/exFAT, meaningful NIC traffic, or every native service. Broader workloads may justify revisiting them.
- The classifier's actual BSS cost is 128 MiB because the heap virtual reservation is 512 GiB. Production-source tests cover publication, pointer bounds, descriptor validation, reuse, and retirement ordering. The cost is absent from default builds, but must be reconsidered before ordinary allocation routing is enabled.
- Page-aligned splitting must coexist with the existing physical-neighbor chain and 48-byte block header; ordinary kmalloc must remain unchanged.
- The current kernel heap and PMM growth path have bounded non-sleepable semantics. A slab miss is not guaranteed to succeed in an interrupt handler or while a spinlock is held.
- DMA call sites must retain explicit device contracts. In particular, investigate AHCI’s dma_alloc usage separately; its current implementation reports only the physical translation of the aligned virtual start address, which by itself does not establish multi-page physical contiguity.
- A class lock does not make descriptor lifetime safe if a page-map bit is cleared or published outside the specified ordering.
- Per-class activity and cross-CPU frees do not quantify contention for locks that have not yet been implemented. The initial shared-lock design is a correctness baseline; measure wait time before adding per-CPU state.
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
