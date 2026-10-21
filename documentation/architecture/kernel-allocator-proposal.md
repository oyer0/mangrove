# Pith kernel allocator architecture proposal

**Status:** the ordinary production build now selects hybrid routing with a
256-byte cutoff. Set `KERNEL_HYBRID_ALLOCATOR=0` for indexed-only comparison
or recovery; supported hybrid cutoffs remain 256, 288, 320, 352, and 384
bytes. The cache remains two objects per CPU/class with a two-object refill.
Initialization is after SMP startup; bounded reclamation is attempted once
after allocation failure. The performance and integration analysis below
records the evidence that preceded the default change and should be read as
historical qualification, not as a claim that all Pith workloads improve. The
memory contract and limits are in [kernel memory architecture](memory.md).

Source-matched Pith demand profiling, public-path host comparisons, and
one-/two-vCPU guest qualification cover the hybrid configuration. A fresh
two-vCPU indexed-only Pith profile counted 18,459 public allocations and
14,242 frees; 8,591 requests (46.54%) were at or below 384 bytes, accounting
for 675,639 requested bytes (0.294%). Hybrid runtime counters show local cache
misses/refills and overflow in the same workload; they do not provide latency
attribution or a complete slab-object event trace. The 384-byte public hybrid
retains a fixed-size four-worker host advantage, while the exact Pith event
replay remains slower at its median with overlapping trial ranges. A
1,024-live-object fixture still uses more mapped heap extent with slabs. These
adapter and QEMU results do not establish physical-hardware performance.

## Historical performance decision

At the time of that Stage 4.6 decision, the 32-bin indexed coalescing heap
remained the default. The hybrid was available as an explicit production
build option because it reused the heap's backing, mapping, rollback, and
growth contracts and offered measured small-object reuse benefits in some
workloads. The historical evidence did not justify selecting it as the
default at that time.

**Historical decision: keep the hybrid optional and indexed-only as the
default.** The public dispatcher preserves pointer, generation, descriptor, bitmap, failure,
and transition checks. A current GCC 14.2 `-O2` host run with five independent
trials measured four-worker immediate-pair medians of 4.81M/4.89M pairs/s for
the public 384-byte hybrid at 16/72 bytes, versus 3.90M/3.69M indexed-only.
The exact-size immediate replay was effectively tied: 127.70 ns/pair for the
public hybrid versus 127.84 ns indexed-only. The complete Pith event-order
replay measured 121.18 ns per allocator operation for the public hybrid
versus 110.25 ns indexed-only; five-trial ranges overlap (111.79–131.88 ns and
100.16–129.67 ns). This is serialized host replay, not a concurrent Pith
throughput test. A 1,024-live-object public fixture mapped 344,064 bytes at
cutoff 384 and 262,144 bytes at cutoffs 256/288, versus 245,760 bytes
indexed-only. After drain, slab backing returned to heap capacity while the
mapped high-water extent remained. Fixed-size host reuse is a repeatable
workload-specific benefit; the Pith trace and live-population result do not
establish a general system benefit. QEMU TCG qualifies execution and ownership,
not throughput. No physical-hardware comparison is available.

The available evidence identifies ownership checks, descriptor/bitmap validation, and mutation as material free-path costs. It does not prove that one operation alone dominates because component probes are independently sampled and instrumented. The shared path stores a 32-bit integrity value per bitmap word, validates the touched word when changing shared bitmap state, and scans all words plus reconstructs population before retiring a slab. Per-CPU cache hits instead validate generation and slot state under the cache record lock without changing the shared bitmap. Deep mode adds whole-descriptor, bitmap, and neighbor checks; `slab_verify()` remains available in either mode. Host multicore results are adapter measurements with verified pinned workers; native QEMU overlap proves concurrency correctness but its short-phase timer cannot establish a reliable throughput advantage. QEMU TCG measurements are diagnostic only; no physical-hardware performance claim follows. The source-grounded workload profiles support retaining the 20 classes through 384 bytes and 4 KiB backing as candidates, not as proof of a default-selection benefit.

The format-6 hybrid runtime profile adds counters without timing the allocator.
In one-vCPU cutoff-384 create activity it admitted 18,593 public allocation
calls, routed 8,659 to slabs, recorded 70 clean heap fallbacks, and reported
no allocation failures or reclaim attempts. Per-CPU cache counters recorded
5,348 hits and 3,311 misses/refills (61.8% of hit-or-miss allocation attempts
were hits); class 16 alone had a 50.5% hit rate and 1,626 overflows. The
snapshot showed 59 current slab pages, an 8,224-byte classifier, and 2,560
bytes of CPU-cache metadata. The two-vCPU cutoff-384 create snapshot was
independently accepted with both CPUs online and a two-CPU cache-section
overlap witness; it counted 8,533 slab allocations, 4,399 slab frees, 5,206
hits, 3,327 misses/refills, 2,480 overflows, and 639 cross-CPU slab frees.
There were 58 current slab pages, 40 cached objects, and 5,120 bytes of
CPU-cache metadata. The subsequent verify boot in that full profile campaign
panicked with `scheduler: restored stack outside kernel stack`; a separate
384-byte profile attempt panicked during create. The uninstrumented two-vCPU
384-byte safe guest completed its three phases. These observations do not
establish allocator corruption or attribute the panic to a specific class,
but the profiled 384-byte campaign is not a complete pass.

Final-source 256-byte profile campaigns completed on one and two vCPUs. Their
create snapshots recorded 7,119/7,124 slab allocations, 4,244/4,208 cache
hits, and 2,875/2,916 misses/refills, for hit fractions of 59.6%/59.1% among
cache hit-or-miss allocation attempts. The sum of per-class occupancy rows was
53/54 current slab pages and 57/59 summed per-class peak pages; these rows are
sampled separately and are not global atomic snapshots. The two-vCPU run
recorded 594 cross-CPU slab frees and observed both CPUs in overlapping
per-CPU cache critical sections. Both runs reported zero allocation failures,
backing-allocation failures, and pressure-reclaim attempts. Their final public
route totals were 18,366/18,375 allocation attempts, including 43 clean heap
fallbacks in each run. These are bounded workload samples, not stable system
rates. The profile's ordered heap trace does not contain slab-object lifetime
events; validation covers heap events and hybrid aggregate counters
separately, so it cannot reconstruct a complete hybrid replay.

For the observed Pith activity, class-16 objects include persistent VMM
ownership records with large live populations, while the MGFS name-tracking
candidate falls in class 288 and was short-lived in the indexed trace. The
generic profile does not show a benefit from another ownership layer on
either type. No dedicated object cache is justified by these measurements.

The backend uses a 4 KiB static root directory with lazy indexed-heap middle and leaf tables and generation tags for retirement and same-address reuse. It does not allocate classifier storage proportional to the 512 GiB virtual heap reservation. The indexed-only recovery build omits the slab module. The captured MGFS workload profile is bounded and does not represent every kernel workload.

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
| A. Keep indexed heap only | Smallest change; arbitrary sizes; existing growth and rollback tests remain directly applicable; coalescing reuses freed blocks. | Per-object header cost remains; global IRQ-safe lock and same-bin scans remain; fragmentation can reject a large request despite high aggregate free payload; pages are retained. | Remains available as the indexed-only comparison and recovery build. Historical profile replay was near parity and the indexed heap used less memory in the tested live population. |
| B. Small slabs over indexed heap; indexed heap for larger requests | Removes a 48-byte heap block header per small client object; bounded bitmap allocation within a slab; existing heap retains arbitrary-size allocation, PMM/VMM ownership, and growth. Empty slabs can return as ordinary heap blocks. | Adds class rounding, page-level partially-live retention, class metadata, free-pointer classification, and lock-order rules. Large requests retain current heap fragmentation and 48-byte headers. It does not return mapped pages to PMM. | Implemented and selected for the ordinary build at a 256-byte cutoff. Fixed-size immediate-reuse workloads improved on the host adapter; historical profile-size replay was effectively tied and the tested live-population footprint was larger. |
| C. Slabs plus a page-backed large-span allocator | Separates variable-size runs from small-object blocks, can reclaim fully unused mappings and PMM frames, and avoids heap block headers for page runs. | Requires a virtual-run allocator, authoritative per-page ownership, mapping rollback and shootdown lifetime, descriptor bootstrapping, concurrency-safe unmap/reuse, and a large-object migration audit. A virtually contiguous span is not necessarily physically contiguous. | A plausible later architecture if measured large-allocation fragmentation or retained pages warrant it. Not justified as the initial change because it duplicates VMM/heap ownership work and raises the failure surface. |
| D. Explicit typed object caches | Can specialize alignment, constructors, diagnostics, and type separation for stable structures. | Requires per-type API migration, accurate constructors/destructors, cache lifetime rules, and size/lifetime evidence. It does not replace variable-size kmalloc and would create many caches if adopted indiscriminately. | Not the first step. Consider only for demonstrated high-frequency stable types or a real isolation requirement. |

## Hybrid allocator design

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

The two guest traces recorded 18,288 and 18,237 successful allocations. A 384-byte cutoff covered 46.06% and 45.89% of allocation count, but only 0.286% and 0.283% of requested bytes. Raising the cutoff to 512 bytes admitted exactly seven additional requests in each trace, all 512-byte requests, and added one ideal-modeled peak slab page; it did not change accumulated class-rounding waste. That small count gain does not justify the four extra classes and additional modeled page in this workload. The 384-byte class holds ten objects; its 112-byte fixed object-area offset plus two ownership bytes per slot leaves 124 bytes of page tail. The 20-class table is the bounded initial choice, not a claim that one-page slabs are optimal for all applications.

For normalized sizes, class rounding adds at most 16 bytes in the 16-byte-spaced range and 16 bytes in the 32-byte-spaced range, because normalized requests are multiples of 16. Relative to the original unrounded request, unused bytes are bounded by 31 bytes through the 384-byte cutoff. This excludes slab descriptor/bitmap, free tail, and page-level retention, which are reported separately.

### Slab representation and pointer lookup

Each active slab owns one 4 KiB page-aligned payload block from the existing heap through `heap_alloc_page_extent()`/`heap_free_page_extent()`. The extent handle carries a nonzero 32-bit generation mirrored in the heap block and slab descriptor; a stale copied handle cannot release a later allocation at the same virtual address. The page count is currently one for slab descriptors. This remains virtual-heap backing and does not transfer an independently tracked physical-frame set. A leading fragment is created only when it can hold the existing minimum split payload; suffix splitting uses the same threshold as ordinary allocation. The production-source heap tests cover alignment, prefix/suffix boundaries, accounting, coalescing, and stale extent handles. The operation does not promise physically contiguous or DMA-safe memory.

At the slab page base, reserve an 80-byte descriptor and a 32-byte allocation bitmap. The object area begins at offset 112 and every slot is class-size aligned, therefore at least 16-byte aligned. One byte per slot records its encoded state and one byte records its last client-allocation CPU; the smallest class has 221 slots, which fits in the bitmap’s 256 bits. Unused high bits must remain clear. The descriptor records a format tag, class index, slot count, allocated count, state, backing extent generation, list links, static/dynamic integrity values, and per-word bitmap checks; it does not replace the heap block header immediately before the aligned page.

The heap reservation in `include/address_layout.h` spans 512 GiB (134,217,728 possible 4 KiB pages); this is virtual capacity, not committed RAM. The former diagnostic classifier stored one byte per possible page, adding exactly 134,217,728 bytes to BSS. Retained target artifacts report 5,240,944 BSS bytes for the ordinary image and 139,461,296 for the dense-classifier diagnostic image. The dense map was never part of ordinary builds, but its static cost was too high for activation.

The classifier uses a static root directory sized from the full fixed heap reservation. For this 512 GiB interval it has 512 64-bit pointers and occupies 4,096 bytes; initialization separately bounds usable root entries by the supplied heap capacity. Each root entry covers 1 GiB; each lazily allocated middle entry covers 2 MiB; each leaf has 512 atomic 64-bit page tokens. Middle and leaf payloads are each 4,112 bytes, allocated through `heap_alloc_raw`, not through slab allocation. The first classified 2 MiB region retains one middle plus one leaf: 8,224 payload bytes and two 48-byte raw-heap block headers, plus the separate static root. Each additional region in the same 1 GiB span retains one leaf plus its block header. Unpublished concurrent candidates return to the heap; published tables remain allocated and stable for the module's lifetime. A failed child-table allocation may leave a valid empty parent for reuse. If every 2 MiB region in the 512 GiB reservation is visited, dynamic table payload is 1,080,041,472 bytes plus 12,607,488 bytes of heap headers, in addition to the 4,096-byte static root. Actual dynamic use follows regions that needed slab classification.

Each page token reserves five low bits for class plus one, 47 bits for generation, 11 bits for ownership pins, and the high bit for retirement. Zero class means ordinary or retired storage. Slab descriptor and bitmap initialization precede release publication of a nonzero class token. Shared frees and per-CPU cache entries validate the heap range and committed-page status, use the token only to select a class, then pin and revalidate its class/generation before touching descriptor state. Retirement withdraws the class and waits for pins before returning the backing block. Same-address reuse increments the generation before publishing the new class, so a stale local-cache reference cannot authorize a later page incarnation. Classifier table nodes are atomically published, never relocated or freed, and validated for mapped range, alignment, magic, and index before dereference.

Forced target builds with the current sources measured the ordinary image at
811,116 text, 50,972 read-only data, 61,030 data, and 5,240,944 BSS bytes
(ELF file 1,074,576 bytes). The diagnostic slab image measured 823,850 text,
53,404 read-only data, 61,030 data, and 5,248,240 BSS bytes (ELF file
1,094,104 bytes). The guest-test image measured 827,178 text, 53,996
read-only data, 61,030 data, and 5,252,592 BSS bytes (ELF file 1,094,976
bytes). The ordinary image omits `slab.c`; its BSS is unchanged. The
diagnostic module adds 7,296 BSS bytes over ordinary, including the 4,096-byte
root; the guest hook adds another 4,352 bytes. The retained dense diagnostic
BSS was 139,461,296 bytes. These totals are not a production-activation
measurement. Runtime classifier payload counts aligned middle/leaf node
payload requested from the indexed heap; corresponding 48-byte heap headers
and any unsplittable block remainder are additional committed heap use.

An earlier isolated diagnostic guest test initialized the slab module after the requested CPUs
are online, exercises all 20 classes, forces classifier growth across a 2 MiB
leaf boundary, retires slab backing, and verifies ordinary heap reuse. Fresh
one- and two-vCPU QEMU TCG runs pass with one and two CPUs online; the
two-vCPU witness reports a CPU mask of `0x3` and 96 cross-CPU frees. Both
retained boot logs contain the complete witness, both native Shoot test
processes completed, and independent `mgfsck` checks reported clean images
before and after guest execution. The normal e1000 NIC remains configured.
Each guest retained 12,336 dynamic classifier payload bytes (one middle and
two leaf nodes), plus 144 bytes of indexed-heap headers, after slab retirement.
Boundary stress mapped an additional 1,884,160 heap bytes on one vCPU and
1,835,008 bytes on two vCPUs; this includes temporary ordinary-heap filler
blocks used to reach the second classifier leaf, not classifier metadata
alone. Heap growth maps PMM-backed pages, and those pages remain mapped under
the current heap policy. This validates diagnostic execution in QEMU TCG; it
predates the public hybrid dispatcher and does not qualify production routing
or physical hardware.

The `-O2` GCC 14.2 host comparison varies lookup addresses over 600 live
slab pages in two classifier leaves and consumes per-sample results. Complete
production token lookup measured 7.80 ns p50 with the available CPU set and
7.36 ns pinned to CPU 3; the earlier dynamic-root tree measured 10.96 ns in
the source-matched baseline artifact. The direct-root lower bound measured
6.13 and 5.95 ns; a dense atomic-byte lower bound measured 0.69 and 0.53 ns.
The direct-root candidate bypasses the production helper's index guard and
call overhead but retains middle/leaf validation; the dense byte load omits
generation validation. These are not equivalent implementations. The earlier
fixed-page result of 0.249 ns is not used as representative evidence.

A paired benchmark compares the production selector with the exact previous
four-word, bit-by-bit selector over the same 1,024 runtime-selected descriptors
and matched non-inlined call boundaries. For 16-byte slots, at occupancies 31,
63, and 127 of 250, historical/production p50 values ranged across the two
runs from 11.70–14.12/3.18–3.82, 19.37–22.71/3.14–3.15, and 20.14–21.59/
3.73–4.06 ns. The all-CPU occupancy-31 result was noisy; CPU-3 provides a
serialized reference, not multicore data. At occupancy 31 of 50 slots in the
80-byte class, results were 12.14–12.74/3.29–3.53 ns. For the 10-slot
384-byte class, the production path uses a bounded short scan instead of
`ctzll`; low-occupancy measurements are comparable within host noise. A full
bitmap checks one word instead of the historical four, but this measures a
rejection boundary, not ordinary allocation from a partial slab. The results
support bit scanning when larger classes have a later free bit, not a universal
win at every occupancy. Target disassembly shows the intrinsic has no helper
dependency and executes only on the nonzero, larger-class path.

Earlier same-source host end-to-end pair runs were slower and variable. Slab/
heap p50 ratios were 1.100/1.018 at 16 bytes, 1.037/1.018 at 72 bytes, and
1.044/1.066 at 384 bytes (available CPU set / CPU-3 affinity). The latest
available-CPU four-worker run favored the heap at both sizes; the pinned run
was near parity, with slabs slightly higher at 16 and 72 bytes. Pinned workers
share one CPU, so those results are a serialized contention proxy rather than
multicore throughput. Allocation-only batches sometimes favored slabs, while free-only
batches and the paired end-to-end results did not show a stable advantage.
Refill/retirement samples include timer overhead and are not used to claim a
latency value. These host adapters do not model Pith interrupt costs or prove
physical-hardware performance. Source hashes, compiler flags, affinities, and raw samples are
retained in `build/stage4e3a1-baseline/extended-baseline.json` and
`build/stage4e3a1/benchmarks/qualified-v2-all-cpus.json` /
`qualified-v2-cpu3.json`; reproduce with `python3 tests/slab_benchmark.py gcc
--output build/<new-result>.json`.

### Earlier native Pith diagnostic comparison (superseded)

The benchmark-only kernel is compiled with the target's current GCC defaults
(`-O0`; `KERNEL_CFLAGS` sets no optimization level). Ordinary `kmalloc` and
`kfree` continue to call the indexed heap. The diagnostic invokes both
production implementations directly after the slab core tests. Timed sections
contain no logging or benchmark allocation. Each of seven retained samples
times a batch of 16 allocations or 16 frees while 16 other objects remain
live and 16 slots are reusable; ownership, payload, alignment, and overlap are
checked outside the timed section. Two warm-up trials are discarded. The
separate four-worker phase runs 512 allocation/free pairs per worker for a
same-class and mixed-class case. Its allocator-call brackets locally disable
interrupts so CPU identity and active-CPU observations cannot be changed by a
local timer preemption; cross-free owner capture uses the same protection.
This adds equal wrapper work to both paths and means these operations execute
with interrupts disabled, unlike an ordinary interrupt-enabled caller. The
phase records CPU masks, cross-CPU frees, and overlapping call brackets. The
one-vCPU elapsed time is scheduler- and TCG-sensitive; it is not a
per-operation latency measurement.

On the source-matched one-vCPU QEMU TCG run, the median serialized TSC ticks
per operation were:

| Request | Heap alloc/free | Slab alloc/free | Slab/heap ratio from summed medians |
| --- | ---: | ---: | ---: |
| 16 bytes | 15,223 / 5,868 | 11,915 / 15,221 | 1.29 |
| 72 bytes | 15,079 / 6,074 | 12,173 / 15,153 | 1.29 |
| 192 bytes | 15,306 / 6,140 | 18,730 / 23,803 | 1.98 |
| 384 bytes | 15,087 / 6,278 | 21,693 / 30,445 | 2.44 |

Each median uses seven 16-operation samples. The ratio is the sum of separate
allocation and free medians, not a measured pair median. Serialized `RDTSC`
samples require the same CPU and unchanged local scheduler-tick count, but
QEMU host descheduling can still advance virtual TSC without a guest tick.
The serialized TSC-pair minimum was 66 ticks and is reported as a timer
reference, not subtracted; it excludes the `bench_stamp` CPU/tick checks and is
not the full timing-bracket overhead. The observed sample ranges were wide,
and seven samples do not support useful p95/p99 estimates. These values are
diagnostic TCG ticks, not cycles or
nanoseconds, and do not establish physical-hardware latency. Allocation alone
favored slabs at 16 and 72 bytes; slab frees were about 2.6, 2.5, 3.9, and 4.8
times the heap medians respectively. At 192 and 384 bytes, both slab
operations were slower. The source and generated target code show that free
traverses the sparse classifier, checks the generation token, acquires the
class lock, validates the descriptor and slot, then updates the bitmap. The
native samples do not time these substeps separately, so they identify a slow
path but not a single dominant instruction group. The target disassembly also
showed that `slab_try_free` evaluated both `% slot_size` and `/ slot_size`,
emitting two 64-bit divide instructions in the default `-O0` build. The core
now computes the quotient once and checks exact slot alignment by multiplying
the quotient by the validated nonzero slot size. The fresh disassembly has one
divide. Existing class-by-class interior-pointer panic tests still pass. The
before/after TCG samples are noisy and do not establish a latency improvement
from this arithmetic simplification.

The one-vCPU four-worker diagnostic recorded 36/30 ms for heap same-class/
mixed workloads and 40/52 ms for slab, with every allocator call on CPU 0 and
no concurrent CPU activity. This is a serialized scheduler workload, not SMP
contention. In the fresh two-vCPU benchmark both CPUs came online, but the
first measured workload ran on CPU 1 only (`cpu_mask=0x2`,
`peak_active_cpus=1`, four cross-CPU frees). The validator rejected the run;
it provides no two-vCPU throughput comparison or class-lock contention
measurement. The separate two-vCPU slab correctness guest passed with both
CPUs online and cross-CPU frees, but does not supply timed contention
evidence. The ordinary e1000 device was configured; these allocator
workloads generated no network traffic. The final source-matched rerun is
retained under `build/stage4e3a2/guest-1vcpu-benchmark-final6/` and
`build/stage4e3a2/guest-2vcpu-benchmark-final6/`; each report records HEAD
`77e90324b6338a985b248dda8908c479d63fc1d4` at run time, plus per-file hashes
and the target kernel hash that identify the modified build inputs. The
one-vCPU report passed; the two-vCPU
report is a failed benchmark gate, not a boot failure.

The diagnostic kernel image hash is
`fdb364b90babc84f5136ef15d64bb851822c2a7711b163003a66304ac6c73414`; the
changed `kernel/src/slab.c` hash is
`a610551e7dab9b4cf4c0d4a7d8504f10fc4c682bf73fde658076bf98b0b866d4`.

The native run measured 8,224 bytes of retained sparse classifier payload
(one middle and one leaf), plus the 4,096-byte static root. First slab refill
increased heap payload use by 12,320 bytes: one 4 KiB slab page and the 8,224
classifier bytes. Retirement released the slab page but retained classifier
metadata; final heap payload use remained 8,224 bytes above the pre-classifier
baseline. The tested 16/72/192/384-byte classes reached 32 live slots each and
ended with zero live slots and equal backing allocation/release counts. The
single-refill/retirement TSC intervals crossed scheduler ticks and are raw
lifecycle evidence, not latency estimates. Short-lived sample preparation
repeatedly empties slabs, so its backing-allocation counts must not be
interpreted as application refill frequency.

An earlier diagnostic image-size comparison is superseded by the forced
build measurements recorded below.

The host `-O2` comparison also did not show an end-to-end latency gain. In the
latest run its paired slab/heap p50 ratios were 1.021, 1.042, and 1.043 for
16-, 72-, and 384-byte requests. Four-worker host throughput favored slabs at
16 bytes (7.13 versus 6.61 million pairs/s) and 72 bytes (7.12 versus 5.18
million pairs/s); this is a pthread adapter, not Pith SMP, and one run does
not establish a stable throughput advantage. The host hot
classifier p50 was 7.87 ns for the sparse lookup, 7.11 ns for an inline
direct-root lower bound, and 0.53 ns for an atomic-byte lower bound that omits
generation checking and equivalent ownership validation. These are host
results, not target timings. Neither host adapters nor QEMU TCG provide
performance parity evidence. The classifier and the complete slab-free path
are implicated by source and path comparison, but their individual native
costs and shared class-lock contention remain unmeasured. The latest host raw
samples and source hashes are retained in
`build/stage4e3a2/host-after-single-div.json`; the earlier same-method run is
`build/stage4e3a2/host-baseline.json`. The final benchmark source hash is
`c0be81cc42c2c3cb54ed88200250e9fa8b0f3c20eef6c41fadaebd722914df56`; the
target used GCC 14.2's default `-O0` kernel configuration. The final bounded
changes moved cross-free owner capture inside the same local interrupt
exclusion as allocation and removed the duplicate division; the diagnostic
target and guest runs were repeated. The two-vCPU failure and measurements
described in this subsection were superseded by the later runs below. This
subsection is retained to explain the earlier diagnostic result.

### Latest free-path and SMP evidence

The current no-profile comparison build sets
`SLAB_CORE_GUEST_BENCHMARK=1 SLAB_CORE_GUEST_FREE_PROFILE=0`, so its free-path
timing hooks are compiled out. A separate profile build enables
`SLAB_CORE_GUEST_FREE_PROFILE=1` and is used only to attribute work; its
latencies are not used for allocator comparison. Both use GCC's target default
`-O0` kernel flags and predate the public hybrid dispatcher. They characterize
the diagnostic slab APIs; current public-route measurements are summarized at
the start of this document.

The latest no-profile diagnostic has one source-matched one-vCPU run and
three source-matched two-vCPU runs after a preceding two-vCPU run failed its
mixed-class slab phase. Each steady-state measurement consists of 31 batches
of 16 operations per allocator, request size, and phase, after two warm-up
cycles. The two-vCPU steady-state measurement itself runs on one CPU; it is
not the SMP contention phase. Ratios below are slab/heap, calculated from the
sum of separate allocation and free medians rather than a measured pair
latency:

| Request | 1 vCPU | 2 vCPU run 1 | 2 vCPU run 2 | 2 vCPU run 3 |
| --- | ---: | ---: | ---: | ---: |
| 16 bytes | 1.074 | 0.998 | 1.038 | 0.784 |
| 72 bytes | 0.949 | 1.018 | 1.006 | 0.963 |
| 192 bytes | 1.212 | 1.285 | 1.320 | 1.284 |
| 384 bytes | 1.362 | 1.398 | 1.496 | 1.354 |

Across these runs, median slab free versus heap free ranged from 1.60–1.70
times at 16 bytes, 1.46–1.63 at 72 bytes, 2.04–2.07 at 192 bytes, and
2.03–2.23 at 384 bytes. Slab allocation is faster in the smaller classes in
these samples, but the more expensive free erases that benefit for most
allocation/free pairs; 192- and 384-byte pairs are slower in every current
run. The one-vCPU ratio for 72 bytes is slightly below one, so the data do not
support a consistent pair-latency advantage. The sample count does not
support tail-percentile claims. These are serialized TSC ticks under QEMU
TCG, not calibrated elapsed time, cycles, or physical-hardware performance.
Timer reads use `lfence; rdtsc; lfence`, with CPU and scheduler-tick checks;
host descheduling can still perturb virtual TSC.

The production-source host adapter was also rerun with GCC 14.2, `-O2`, and
128 steady samples per request size. The retained pre-change run
(`build/stage4.3-baseline/host-baseline.json`, slab source SHA-256
`032697c41a1f2a7d885ac3ad6c9fca92ede9e1e14b22806ffd295e51243c9255`)
reported slab/heap p50 ratios of 0.944, 0.973, and 0.908 at 16, 72, and
384 bytes. The two current runs report 0.716/0.798, 0.687/0.693, and
0.742/0.717 respectively. Their source-matched current slab SHA-256 is
`eb727d7c5abeea0a85fc2bc5e3ad10b5cb166da464897ed0bfd872125d797997`.
This indicates lower single-worker host medians in this fixture, not a native
Pith or hardware result. Four-worker host throughput varied: the first current
run favored slabs at both sizes, while the second was near parity (slab about
3% slower at 16 bytes and 4% faster at 72 bytes). Host scheduling and the
adapter limit the conclusion.

The separate one-vCPU free-path profile ran three trials of 1,024 operations
per case and retained 64 samples per trial for each of 16-, 72-, 192-, and
384-byte requests. It covers nonfinal frees, full-to-partial transitions, and
final-object retirement. Each stage was sampled in a separate run, so stage
medians must not be summed as one operation. Profiling adds timestamp and
counter work; only the no-profile build above is used for allocator comparison.

For nonfinal frees, the current instrumented end-to-end medians range from
11.45k to 12.94k ticks, with 192 samples per request size across three
trials. Independently measured stage medians were:

| Stage | Median range (ticks) |
| --- | ---: |
| Sparse classification | 2.77k–2.94k |
| Class-lock acquisition | 1.09k–1.49k |
| Locked token and descriptor validation | 3.27k–3.40k |
| Slot and bitmap validation/update | 2.31k–2.48k |
| Metadata mutation and transition checks | 2.57k–3.00k |

The uncontended lock interval is materially smaller than classification and
validation/mutation. This does not show that contention is absent under other
workloads, but it does not justify per-CPU caches as the first free-path
correction. Full-to-partial end-to-end medians were 15.8k–16.6k ticks.
Final-object retirement medians were 29.3k–31.0k ticks, including 7.4k–8.0k
ticks for the separately measured indexed-heap backing release. Profile
instrumentation affects these intervals, and lifecycle measurements are not
ordinary-free costs.

The current correction replaces a per-operation full-bitmap scan with
per-word integrity values. The descriptor integrity value covers these checks
and the descriptor fields; each allocation/free validates the target bitmap
word, including unused tail bits, before mutation. A last-object free checks
all bitmap words and independently verifies the population count before it
retires backing. `slab_verify()` also scans the complete bitmap. This preserves
detection before a corrupted word is used or the slab is retired, while
unrelated-word corruption may remain latent until that boundary. These checks
are integrity diagnostics, not cryptographic protection against an attacker
able to forge allocator metadata. The generation token is still revalidated
under the class lock before descriptor access. Invalid-pointer, interior,
double-free, and generation checks remain enabled. Target assembly still
contains one slot-size divide in `slab_try_free`; this slice did not change the
slot arithmetic.

Compared with the retained earlier profile, current nonfinal medians are about
14–24% lower by request size. Full-to-partial medians are 13.66k–15.76k versus
15.77k–16.58k ticks; final-retirement medians are 26.19k–28.79k versus
29.27k–31.00k ticks. These are independent instrumented intervals, not
calibrated elapsed time, and must not be summed. In separate unprofiled guest
benchmarks, steady nonfinal slab-free medians were 1.5–2.1 times indexed-heap
medians on one CPU and 1.5–2.2 times on two CPUs. The profile improvement is
useful evidence that whole-bitmap validation was avoidable on the ordinary
path, but it does not establish that slabs should serve production frees.

The repaired two-vCPU protocol keeps the BSP as a measured participant while
the AP reaches a bounded rendezvous, then starts four workers. Per-CPU
operation counters and cache-line-separated sequentially consistent active
flags are checked independently. The preceding failed run had both CPUs
online and overlapping allocator brackets, but the mixed-class slab worker
recorded one failure and a zero checksum; the retained log did not identify
the operation. A failure-only diagnostic field was added to report allocator
NULL, payload mismatch, or free rejection if this recurs. Three fresh repeats
then passed with both CPUs online, CPU mask `0x3`, peak active CPU and
operation counts of two, and four cross-CPU frees per allocator/workload.
Each repeat recorded 512 allocations/512 frees on CPU 0 and 2,048
allocations/2,052 frees on CPU 1. The gate requires both CPUs to execute
allocator brackets and proves at least one overlap; two online CPUs alone do
not pass it. The failure was not reproduced, so its cause remains unknown.

### Ownership classification and validation assessment

The slab backend uses the fixed-depth sparse page classifier. A
page-aligned descriptor does not identify slab ownership by itself: `kfree`
must reject an arbitrary or ordinary-heap pointer before reading descriptor
bytes, and must distinguish an old page incarnation from a newly published
slab at the same address. The independent 64-bit page token supplies both the
class dispatch hint and the generation used for the under-lock recheck; the
middle and leaf nodes are atomically published and retained, so readers do not
need a reclamation or reader-pinning protocol.

The alternatives have no established end-to-end advantage. A dense byte per
page costs 128 MiB for the 512 GiB reservation, while a dense 64-bit token map
costs 1 GiB. A two-level map with one root pointer per 2 MiB region requires a
2 MiB static root, versus the current 4 KiB root and lazily allocated
middle/leaf nodes. A descriptor-at-page-base shortcut still needs an
independent safe ownership test before dereferencing that descriptor. A
compact extent or hash registry could use less metadata for scattered slabs,
but needs either an operation-held registry lock or a separate reader/
retirement protocol; no source-matched measurement shows it would reduce
complete free cost. Keep the current sparse representation unless such a
design demonstrates a useful whole-operation improvement while preserving
the token recheck and retirement ordering.

On every free, current descriptor integrity, class, state, exact slot bounds,
the touched bitmap word, and its allocated bit are checked before mutation.
For a partial slab, the current path also validates its linked neighbor
descriptors and reciprocal links even when this free will not change list
membership. That eagerly detects list corruption but repeats classifier and
descriptor validation. The existing profile combines these operations into
one token/descriptor interval, so it does not establish the cost of neighbor
checks alone. Moving reciprocal-link checks to the list-transition boundary
could reduce ordinary-free work, but would delay detection until a transition
or another operation that inspects that list position, or until `slab_verify()`;
it is not equivalent corruption coverage. Keep those checks unchanged until a
separate production-source experiment measures the specific work and validates
both transition-time fail-closed behavior and retirement races. The current
diagnostic is not sufficient to justify a weaker check placement.

This qualifies overlapping allocator calls, not comparative SMP throughput.
Parallel elapsed TSC totals varied by several times between matching TCG runs,
including reversals between heap and slab. The overlap-tracker calibration is
reported but not subtracted. Same-class and mixed-class churn plus cross-CPU
frees were exercised. e1000 remained configured, but these workloads generated
no network traffic.

Reports are under `build/stage4e3a3-final4-perf-1vcpu-a/`,
`build/stage4e3a3-final4-perf-1vcpu-b/`,
`build/stage4e3a3-final4-perf-2vcpu-a/`, and
`build/stage4e3a3-final4-perf-2vcpu-b/`; the instrumented profile is under
`build/stage4e3a3-final4-profile-1vcpu/`. Reports include source and kernel
hashes, child completion, boot logs, stopped-image hashes, and independent
MGFS checks. The ordinary target build omits slab symbols, and the no-profile
comparison diagnostic contains no free-profile hooks.

The latest one-vCPU comparison and profile reports are retained under
`build/stage4.3-failure-detail-1vcpu/` and
`build/stage4.3-failure-detail-profile-1vcpu/`. The preceding failed
two-vCPU run is under `build/stage4.3-final-native-2vcpu/`; three fresh
passing repeats are under `build/stage4.3-failure-detail-2vcpu/`,
`build/stage4.3-failure-detail-2vcpu-r2/`, and
`build/stage4.3-failure-detail-2vcpu-r3/`. They use the same allocator source
hash; the repeat build adds failure-only work diagnostics to the guest
benchmark. Each report includes the relevant source hashes and independent
guest checks.

Historical `size -A` results were: indexed-only image `.text` 811,116, `.rodata`
50,972, `.data` 61,030, and `.bss` 5,240,944 bytes; diagnostic slab-only
image 823,850 / 53,404 / 61,030 / 5,248,240 bytes; guest-test image
827,178 / 53,996 / 61,030 / 5,252,592 bytes; no-profile benchmark image
841,775 / 57,908 / 61,030 / 5,392,432 bytes; free-profile benchmark image
848,960 / 59,220 / 61,030 / 5,397,616 bytes. These diagnostic objects are
absent from that indexed-only image, which had no slab symbols. The no-profile and
profile benchmark images differ by 5,184 BSS bytes and 7,185 text bytes.
The latest guest workload retained 12,336 bytes of classifier payload (one
middle and two leaf tables) after slab backing retirement; retired pages were
reusable by the ordinary heap. The static classifier root remains 4,096
bytes. Image sections and runtime heap payload are separate costs.

At the time of these measurements, the hybrid was optional and indexed-only
was the default. Source-matched host results showed repeatable fixed-size,
four-worker immediate-reuse gains, but the exact Pith profile-size replay was
effectively tied and the tested 1,024-live-object population used 40% more
committed heap extent with slabs. The one-empty-slab reserve candidate reduced
backing operations in a churn fixture, but timing was too quantized to
establish a benefit, so immediate retirement remains the default. These
historical results are not measurements of the new default configuration or
physical-hardware performance.

The public route preserves pointer-to-class ownership, cross-CPU-free safety,
migration safety, bounded runtime cache draining, and slab retirement. Cache
records cover all discovered CPUs; Pith has no runtime
CPU hotplug contract, so cache-record removal and CPU-offline draining are
unsupported. No slab may retire while a local cache owns one of its slots.
`slab_reclaim_bounded()` uses try-locks and reports partial progress while
users continue; it does not claim a global drain. A failed slab-backing
request makes one bounded reclaim attempt and retries only after a slab page
is returned to the heap. `slab_cpu_drain_all()` still requires quiescent slab
users for a complete drain. The ordinary heap has no callback into the slab
slab core; only the public hybrid dispatcher performs the bounded failure-path
reclaim attempt. The indexed-only build remains available for comparison and
recovery.

Pith has global scheduler ready queues; threads may migrate while not running,
and the scheduler exposes no affinity controls. A CPU-local cache must select
its CPU afresh for each operation and protect that CPU-local state for the full
operation; it cannot retain a CPU identity across calls. See the current
[scheduler and SMP contract](smp.md).

The diagnostic `slab_verify()` checks each class under its lock and scans only the mapped heap prefix. It does not walk the uncommitted portion of the large virtual reservation.

Each size class has a partial-slab list protected by its own IRQ-safe spinlock. Full slabs need no allocation-list entry but remain classifiable through the page token and descriptor. A slab becomes partial when one slot is freed and full when its last free slot is allocated.

A free validates page ownership, descriptor tag and class, object-area bounds, exact slot alignment, slot index, and the allocated bit. Interior pointers and immediate double frees fail closed. The descriptor and bitmap are in-band and remain vulnerable to memory corruption; validation must panic on impossible state rather than repairing it. As with the current raw-pointer `kfree` contract, no allocator can distinguish a stale pointer from a new live allocation after the same address has been reused.

The 80-byte descriptor contains four 32-bit bitmap-word integrity values and
a separate static-field integrity value. The mutable descriptor integrity
also covers the bitmap checks and backing extent generation. The bitmap begins
at offset 80 and the object area begins at offset 112. Allocation/free
validate the bitmap word they use; a final free also scans all words and
reconstructs population before retirement, while `slab_verify()` remains the
full diagnostic scan. Corruption in an unrelated word can therefore remain
latent until that word is accessed, the slab is retired, or the verifier
runs; it cannot authorize a slot from the damaged word.

When a slab reaches zero allocated objects, remove it from its class list, clear its page token's class tag while retaining its generation, release the class lock, then return the aligned backing block to the indexed heap. This is the default policy and retains no empty slab. A diagnostic-only `SLAB_EMPTY_RESERVE_PER_CLASS=1` build can instead retain one empty page per class; the reserve remains classified and owned by the slab allocator until it is drained. A failed backing request drains reserves outside class locks and retries once. The reserve is bounded at 20 pages (80 KiB of slab backing plus heap metadata), but five-trial one-object host churn showed only fewer backing operations, not a stable timing improvement. Keep immediate retirement as the default; do not use reserve behavior in production routing without new workload evidence and an explicit memory-pressure policy.

### Slab allocation, refill, and lock order

A small-object allocation takes the relevant class lock, checks its partial list, and allocates one free bitmap bit using a bounded scan of that slab’s bitmap. It does not acquire `heap_lock` while holding the class lock. Both the slab core API and eligible ordinary `kmalloc` requests use this path.

On a class miss, release the class lock before requesting one page-aligned heap block. The underlying heap allocator retains its current execution-context behavior: it may use already-free capacity in non-sleepable contexts, but any growth still observes the bootstrap window, interrupt state, spinlock-depth check, scheduler, and growth mutex. If backing allocation cannot proceed, the slab path returns `NULL` and public `kmalloc` falls back to the indexed heap. It must never sleep while holding the class lock.

Initialize the unpublished descriptor and clear its bitmap while the aligned backing page is exclusively owned by the heap allocation. Then reacquire the class lock and recheck the partial list: a competing allocator may already have created a slot. If so, use that slot and return the extra backing page after dropping the lock. Otherwise mark the new slab's first slot, compute its integrity value, publish its class tag with release ordering, and link it as partial. The descriptor and bitmap are initialized before classification; readers acquire-load the tag and recheck it under the class lock before dereferencing the descriptor. No class lock is held over PMM/VMM mapping, heap growth, scheduler progress, or backing-block free.

A small-object free takes only its identified class lock to validate and update the bitmap/list. If it retires the slab, it releases that lock before calling kfree on the backing page. A large-object free uses only the indexed heap lock. No path nests class locks or holds a class lock while acquiring heap_lock. The one-lock-at-a-time rule prevents a heap-growth/cache-refill lock cycle.

The existing kmalloc semantics remain important in interrupt and spinlock contexts. A hit in an existing slab is bounded and non-sleeping. A cache miss may use a page already available in the heap, but cannot grow the heap from a held-spinlock context. If it cannot obtain backing under the existing heap rules, the request fails. This preserves the present NULL-on-unavailable-growth behavior; it does not reserve emergency objects for interrupt use.

### Production hybrid dispatch

`KERNEL_HYBRID_ALLOCATOR=1` is the ordinary build selection; its default
cutoff is 256 bytes. Set `KERNEL_HYBRID_ALLOCATOR=0` for an indexed-only
comparison or recovery build. After `smp_start()` succeeds, boot initializes
the slab core and publishes it ready before the init graph proceeds. Heap
allocations made before that point remain valid. If cache-record
initialization fails, the kernel stays indexed-only for that boot because no
slab object has been published. Once initialized, routing remains enabled for
the boot lifetime; disabling it while slab objects exist is not a valid
rollback.

Requests from 1 through the configured cutoff (256 by default) first call
`slab_cpu_alloc()`. Supported cutoffs are 256, 288, 320, 352, and 384. A
clean unavailable/backing failure falls back to the indexed heap, preserving
the existing `kmalloc` failure behavior where an ordinary heap block can
satisfy the request. Corruption and invalid internal ownership panic and do not fall
back. Larger requests always use the indexed heap. `kfree()` first consults
the sparse generation-token classifier. A classified pointer is validated
and freed by the slab path; an unclassified pointer continues through ordinary
heap validation, so invalid and interior pointers are not silently accepted.
Raw heap operations bypass public routing and are used for classifier/cache
metadata and slab backing. The zero-size result remains `NULL`; impossible
heap-reservation requests do not trigger reclamation.

After a public heap allocation fails, the hybrid dispatcher makes at most one
bounded `slab_reclaim_bounded()` attempt when the size could fit in the heap
reservation. It does not reclaim while the caller holds a tracked spinlock.
For a small request, it retries the slab path only if cache objects were
returned; for any request, it retries the heap only if a backing page was
actually returned. Reclamation uses raw heap release and cannot recurse into
the public allocator. Busy records are skipped, partial progress is reported,
and exhaustion remains `NULL` rather than causing an unbounded retry. The
backing becomes reusable by the indexed heap but remains virtually mapped and
PMM-owned; this is not physical-page reclamation.

The public-path host fixture measures both slab-owned and heap-owned frees
through `kfree`, plus indexed-only, shared-slab, per-CPU-slab, and hybrid
builds. One-/two-vCPU QEMU probes exercise all public size classes and large
heap routing in the hybrid configuration. This validates the route and its
ownership contract, not physical-hardware performance. The normal build still
includes the module and selects hybrid-256; callers can select the
indexed-only build for baseline and recovery. Heap statistics continue to count slab backing
capacity, not client requested bytes; slab class counters remain separate.

### Statistics and memory pressure

Keep current heap statistic meanings: total/used/free continue to report heap block payload capacity. A live slab consumes one allocated page-sized heap block regardless of how many client objects are live, so these statistics remain backing-capacity measures, not live requested bytes. The `slab_class_stats_t` reports allocation/free/failure counts, bitmap words examined, backing acquisitions/releases, full/partial transitions, retired slabs, live/peak slots, and current partial/full slab counts. It does not retain each object's requested size, so it cannot report exact live requested bytes or accumulated class-rounding waste. The host benchmark computes those costs only for its known request sizes. Do not silently reinterpret the heap statistics.

Per slab page, the fixed descriptor and bitmap use 104 bytes; the object area begins at 112 and two per-slot ownership bytes reduce slot capacity. The page-aligned heap backing block also has the existing heap header before the page. The core's host tests reconstruct class slot counts and target tests report per-class live and cached slots. The sparse classifier has no fixed reservation-sized BSS. Its table payload is reported through `slab_get_classifier_stats`, excluding raw-heap headers and possible split remainder. The per-CPU cache metadata is dynamically allocated from the indexed heap and scales as 2,560 payload bytes per discovered CPU. The cache and in-flight refill bounds are recorded in the memory contract.

A partially occupied slab remains unavailable to other classes and to large allocations until it becomes empty. Track this separately from class rounding waste. With no empty cache, a completely unused slab is immediately returned to the heap’s coalescing bins. The `slab_reclaim_bounded()` pass uses nonblocking cache/class try-locks, returns explicit progress and budget/lock-skip information, and is safe while other slab users run; it cannot promise global completion under concurrent frees/refills. The hybrid public dispatcher invokes one bounded reclaim pass after indexed allocation failure; the indexed heap itself never calls into slab code, preserving the nonrecursive ownership direction. The heap still does not unmap pages or return frames to PMM. Exhaustion returns `NULL` according to current heap behavior; there is no silent fallback to an unbounded buffer, new page pool, or repeated growth loop.

### Bootstrap and initialization

Class locks and class statistics are static. The one-shot `slab_init(heap_start, heap_capacity)` initializes them and records the supplied heap range; classifier nodes are allocated lazily from the raw indexed heap as needed. Normal heap initialization does not call it. The ordinary hybrid build invokes it only after SMP startup; all earlier allocations remain indexed-heap objects. The indexed-only target excludes the module. Keep the current early-boot path available for pre-scheduler objects and stacks.

The ordinary small-object `kmalloc` route is not initialized during bootstrap. All allocations before SMP startup completes use the indexed heap; slab initialization allocates its CPU/class records through the raw heap interface before publishing the backend ready. If initialization fails, no slab objects have been published and the boot remains indexed-only. Slab misses continue to obey existing non-sleepable heap-growth rules; no emergency object reserve is provided for interrupt or spinlock-held callers.

### SMP evolution

The diagnostic per-CPU implementation defaults to one IRQ-safe record per CPU and class, with capacity two and a two-object refill. Host-only compile-time sweeps measured capacities/refills 2, 4, 8, and 16; the default remains two because the larger settings did not establish a stable multiworker advantage and increase metadata and retained-capacity bounds. Selecting a record is migration-safe: the code reads the GS CPU index, acquires the selected record's irqsave lock, and confirms the CPU index is unchanged before using it. A mismatch releases and retries up to four times, then uses the shared slab path. Maskable interrupt reentry and scheduler migration are excluded while the record lock is held; NMI allocation is unsupported.

Slots have explicit shared-free, client, cached, and refill states. The bitmap and slab live count represent all non-shared slots, so cached/refill objects prevent retirement. Local and cross-CPU frees enter the freeing CPU's cache when space remains; overflow returns through the shared class lock. Cache-local transitions use atomic state changes and do not modify shared descriptor/list fields. Cache refill and drain follow cache-lock-to-class-lock ordering. Refills drop the cache lock before class allocation or heap growth, with one in-flight batch per CPU/class; conflicting misses use the shared allocator. Draining walks all discovered CPU records in order and returns objects to shared ownership before a now-empty slab can retire. There is no CPU hotplug; records remain for the diagnostic module lifetime.

At the default capacity, each cache record occupies 128 bytes, or 2,560 metadata bytes per discovered CPU across twenty classes. At 256 CPUs the record payload is 655,360 bytes. With two cached and two possible in-flight objects per CPU/class, a deliberately conservative one-object-per-page assumption bounds cache/refill-pinned backing at 80 MiB; normal classes pack multiple objects on a page. Capacity 16 with a 16-object refill would raise that bound to 640 MiB. These bounds are upper limits, not measured working-set costs; the complete capacity table and formulas are in the current memory contract. Capacity two remains selected because larger sweeps did not establish a stable multithreaded benefit. The hybrid is the ordinary build selection; indexed-only remains available for controlled comparison and recovery.

### DMA, large objects, and page-backed alternatives

The initial hybrid does not provide DMA memory, physically contiguous memory, page alignment for ordinary kmalloc, or more than the existing 16-byte alignment guarantee. Do not route a request to slabs solely because its requested byte count is small if a call site needs a physical or cache-alignment property. Keep xHCI’s PMM frame path and audit AHCI/dma_alloc requirements independently before allocator migration.

Large and variable-size requests continue through the current coalescing heap. Existing large-allocation fragmentation and lack of PMM reclamation therefore remain. Candidate C would require a separate page-run allocator with a free-run map, per-run metadata, detached mapping publication, and a VMM unmap path whose remote TLB completion precedes PMM frame reuse. The page run would promise contiguous virtual addresses only unless a separate contiguous-frame API is used. It must not be implemented as a quick fallback from a partially failed heap request.

## Measurement and performance qualification

### Diagnostic workload profile

The diagnostic-only implementation is in `kernel/src/heap_profile.c` and is included only when `HEAP_PROFILE_DIAGNOSTICS=1`. The normal target build filters that translation unit and contains no profiler calls, symbols, syscall, or static data. The diagnostic kernel uses a fixed snapshot and a bounded live-pointer table; these add about 999,648 bytes of BSS (475,312-byte snapshot plus 524,288-byte live table and small state). Format 4 introduced a bounded 65,536-record event array (1.5 MiB) allocated once from the raw heap during profile initialization, before event recording. Format 5 uses the same record size and repurposes its reserved 32-bit word for an immediate caller RVA; it adds no event storage. The event hook does not allocate, log, access filesystems, block, or read a clock. If the array cannot be allocated, aggregate profiling may continue, but an event trace is unavailable and cannot be used for replay.

The profiler deliberately does not use an event ring or atomic sequence reservation. Allocation success/failure and free hooks commit while the existing heap metadata lock is held; the operation counter advances at that commit point. This is the allocator's actual serialization order, so a delayed request that entered earlier cannot be mistaken for a completed earlier lifetime. Profile counters are therefore serialized by the existing heap lock, while the admission/inflight state uses bounded atomic operations. The fixed pointer table preserves original requested size, allocation CPU, phase, and commit operation. Address reuse starts a new tracked lifetime. Table probe limits, missing frees, counter saturation, inflight-at-freeze, and phase errors set invalidation flags; the offline parser rejects any incomplete snapshot rather than estimating through missing records. Freeze closes admission while holding the heap lock, then snapshots inflight state. Snapshot export is a private diagnostic syscall and copies bounded chunks only after freeze.

Snapshots include exact request-size counts through 1 KiB, logarithmic buckets above that, success/failure and requested-byte totals, live/peak objects and requested bytes, per-CPU allocation/free counts, cross-CPU frees, phase survivors, operation-count lifetime buckets, and candidate-class demand at allocator commit points. Formats 4 and 5 preserve each allocation, free, and failed request in a bounded event trace with profile-local object IDs, request size, CPU, context, and phase; format 5 additionally records the immediate caller RVA. Trace publication occurs under the existing heap lock after the record is complete; ordering is heap commit order rather than call-entry order or wall-clock overlap. Address reuse receives a new ID. The parser checks event sequence, object lifetime, totals, digest, and completeness before exporting the trace for offline replay. The profile build is indexed-only, so it describes operations entering the indexed heap and does not observe successful hybrid slab fast paths. The counters do not distinguish IRQ handlers from other IRQ-disabled code. Lifetime is measured in allocator operation commits, not elapsed time. Candidate page counts are computed as `ceil(live objects / slots per page)`. This ideal-packing model does not assign objects to real slabs and cannot measure partial-slab retention caused by allocation order. The diagnostic profile changes heap use and timing, so it is evidence about demand and lifetime relationships, not production latency or footprint.

The profile build ran the MGFS guest harness with native service startup, process creation/exit, Shoot, Reed edit/save/Save As/quit, filesystem operations, and read-only verification on one and two vCPUs. The e1000 device remained enabled. The profile-specific Reed interaction omits the external UDP rendezvous/stale-conflict test, and the profile workload does not establish meaningful NIC receive traffic. FAT32/exFAT workloads and other native applications were not profiled. These traces support a bounded first class table but do not represent all Mangrove workloads or future ports.

### Format-5 caller profile and current routing replay

Format 5 reuses the event record's former reserved 32-bit word for the
immediate allocator caller's RVA from `__kernel_text_virt_start`; it does not
add event storage or a stack unwind. `0xffffffff` identifies a return address
outside the linked kernel text. Format-4 snapshots remain accepted by the
analyzer and are reported with caller identity unknown. The diagnostic hook is
present only in `HEAP_PROFILE_DIAGNOSTICS=1` builds. It observes indexed-heap
entries, not successful slab hits, and records the caller at the public/raw
heap entry rather than a semantic allocation type. Resolve an RVA against the
matching Pith ELF and its linker text start; an RVA without that binary is not
a stable source identity.

Fresh 1-vCPU and 2-vCPU QEMU TCG profile runs are retained under
`build/unified-profile-callsite-20261020-1vcpu/` and
`build/unified-profile-callsite-20261020-2vcpu/`. Both runner reports passed;
all three MGFS child phases exited zero, snapshots passed independent
validation, and stopped-image MGFS checks passed. The build kept e1000
configured but generated no RX traffic. The profiles were collected from an
indexed-only diagnostic kernel and therefore do not measure successful slab
fast-path calls. Format-5 profile outcomes were:

| Create profile | 1 vCPU | 2 vCPU |
| --- | ---: | ---: |
| Successful allocations / failures | 18,485 / 0 | 18,393 / 0 |
| Frees / live at snapshot | 14,256 / 4,229 | 14,209 / 4,184 |
| Peak live objects | 4,406 | 4,361 |
| Cross-CPU frees | 0 | 3,055 |
| Requests at or below 384 bytes | 8,542 (46.21%) | 8,525 (46.35%) |
| Requested bytes at or below 384 | 0.291% | 0.291% |

The two-vCPU run's 3,055 cross-CPU frees differs from the prior format-4
capture's 5,198 despite similar request totals. This confirms that the count is
sensitive to run scheduling and workload interleavings; neither trace
establishes allocator-lock contention or a stable cross-CPU-free rate. In the
format-5 two-vCPU profile, allocation contexts were 18,310 normal and 83
interrupt-disabled; free contexts were 12,832 normal, 58 interrupt-disabled,
and 1,319 under tracked spinlocks. These are observed heap-entry contexts, not
a count of interrupt-handler calls.

The source-matched ELF maps the dominant 16-byte allocation site to
`vmm_map_user_page`, which allocates `vmm_owned_frame_t` nodes. That site
recorded 4,113 allocations, 1,572 frees, a peak of 3,646 live nodes, 2,541
nodes live at the snapshot, and 1,177 cross-CPU frees. Its freed objects had
about 1,444 successful allocation/free commits of average lifetime distance.
A second 16-byte site in the same function allocated 129 reservation nodes.
The 272-byte `mgfs_seen_name_add` site recorded 946 allocations/frees, peak 43 live
objects, and 4 cross-CPU frees, averaging about 16.5 successful
allocation/free commits per freed object's lifetime. `mgfs_finddir_checked`
recorded 1,432 72-byte allocations still live at the snapshot;
`location_cache` recorded 396 320-byte allocations, peak 20, with 17 still
live. The VMM records are a
large long-lived population; the MGFS seen-name records are a bounded transient
population. This is evidence for workload shape, not proof that a type-specific
cache beats the existing class pools. The general size-class caches already
serve allocations of those sizes, while a truly separate typed cache would
need additional ownership, accounting, and reclamation policy.

The source trace summary and the final five-trial public-API/replay campaign
are retained under `build/unified-profile-callsite-20261020/`. The
32,602-event two-vCPU trace was replayed serially on the host in heap commit
order. Median cost was 95.46 ns per allocator event for indexed-only,
91.00 ns for direct shared slab, 106.47 ns for direct per-CPU slab, and
110.47 ns for public hybrid routing at 384. Trial minimum/maximum were
95.24/97.49 ns for indexed-only, 88.47/92.73 ns for shared slab,
103.29/112.07 ns for per-CPU slab, and 106.84/119.82 ns for public hybrid.
These host adapter times include the ordered allocation/free lifetimes and
logical CPU IDs, but no guest overlap, scheduling, IRQ state, or TCG timing.
The shared-slab direct result does not represent the production public API;
the public path is the relevant hybrid comparison. The public path's trial
range did not overlap the indexed range in this campaign, but the evidence is
still one serialized host replay of one Pith workload, not guest SMP
throughput. The size-frequency-only replay medians were 118.57 ns per pair
indexed-only, 117.70 ns direct per-CPU slab, and 117.50 ns public hybrid. The
public and indexed ranges overlapped (113.71–125.76 and 117.25–118.64 ns).
That immediate-lifetime replay omits the captured object lifetimes and is not
the deciding workload result.

In the same five-trial public benchmark, single-worker pair medians for
16/72-byte requests were 123.9/126.8 ns indexed-only and 109.5/115.5 ns for
the public hybrid. Four pinned workers completed 3.89/3.81 million indexed
pairs/s and 4.91/4.91 million hybrid pairs/s. For 192/384-byte requests the
single-worker medians were 126.8/126.8 ns indexed-only and 117.3/118.8 ns
hybrid. These fixed reuse workloads show a benefit in this host adapter, but
they do not reproduce the captured object lifetimes. A selective 256/288
cutoff makes the 384-byte request use the indexed heap after dispatcher
selection: its measured single-worker pair median was about 158 ns versus
126.8 ns indexed-only. This is why the smaller cutoff is not selected from
coverage percentages alone.

The 256-byte policy covered 39.05% of allocations and 0.124% of requested
bytes, with a 50-page ideal-packed peak model and 18,318 bytes of size-class
rounding. The 288-byte policy covered 44.20%/0.236%, modeled 51 pages, and
incurred 33,457 bytes of rounding; 384 covered 46.35%/0.291%, modeled 53
pages, with the same 33,457-byte rounding. In the live-population fixture,
the earlier source-matched measurements still show 245,760 bytes of committed
heap extent indexed-only, 262,144 at cutoffs 256/288, and 344,064 at cutoff
384. Those are adapter virtual extents, not total system RAM or PMM
reclamation. The narrower cutoff's ordered replay was slower than indexed-only
and its fixed 384-byte request path pays classifier dispatch while using the
heap. The full cutoff reduces that boundary penalty but has the larger fixture
extent. At that time no candidate had demonstrated a representative
system-level speed/memory tradeoff to justify changing the indexed-only
default; Slice 2 has since selected hybrid-256 for the ordinary build without
new performance data. This selection remains subject to later qualification.
The per-type 16-byte demand and 272-byte churn do not yet justify a
dedicated cache: the VMM records are long-lived and frequently freed remotely,
while the MGFS population peaks at 43 and is already served by a general size
class. No fixed-type cache was implemented or integrated.

Historical focused hybrid guest qualification used the allocator sources
with the guest-test configuration in normal and deep validation modes. All
four one-/two-vCPU reports passed with native child status zero and independent
`mgfsck` checks before and after execution. In the two-vCPU normal run, the
public-API witness observed both CPUs, 110 overlapping allocator calls, and
96 cross-CPU frees. Deep validation observed 71 overlaps and 66 cross-CPU
frees. The one-vCPU runs appropriately reported no cross-CPU frees. Bounded
reclamation released backing in every run, and IRQ-disabled reclaim preserved
the entry interrupt state. These are focused correctness checks, not system
performance or network-RX qualification; the e1000 device was configured but
no traffic was sent. Reports are retained under
`build/unified-hybrid-guest-{normal,deep}-20261020-{1,2}vcpu/`.

The forced GCC 14.2 builds at that time confirmed the ordinary kernel was
indexed-only: its symbol table had no slab or heap-profiler functions. The
opt-in hybrid image linked `slab_init` and `slab_cpu_alloc`. For those matched
fresh builds, the hybrid image added 25,176 bytes of `.text`, 4,192 bytes of `.rodata`,
8,800 bytes of `.bss`, and 38,168 bytes total loadable footprint. This static
image cost is separate from dynamically allocated per-CPU cache/classifier
metadata and 4 KiB slab backing.

### Target profile evidence

The format-2 table immediately below is retained historical Stage 4E.1b
evidence. Format 3 was an intermediate Stage 4.6 capture. The format-4 event
trace later in this section is retained historical evidence; the format-5
profile and routing replay above are the current source-matched measurements.
Earlier snapshots remain in their original ignored build artifacts.

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

In the 2-vCPU create trace, 10 of 20 classes had allocation/free activity attributed to both CPUs. Under the 384-byte cutoff, 1,751 of 4,320 frees were cross-CPU. The busiest small class was 16 bytes (4,295 allocations and 1,438 cross-CPU frees), followed by 80 bytes (1,913 allocations, 205 cross-CPU frees) and 288 bytes (947 allocations). This supports accepting remote frees. It does not establish lock contention. The diagnostic per-CPU path records frees on the CPU performing them and bounds each local cache. The corrected pinned host benchmark shows a workload-dependent gain for immediate reuse, while the live-population workload and native guest comparison do not establish an overall advantage.

The intermediate Stage 4.6 format-3 profile rebuilt Pith with
`HEAP_PROFILE_DIAGNOSTICS=1`, indexed-only routing, GCC 14.2, and QEMU TCG.
Its counts are retained below as historical context; the format-4 capture
that follows was the next source-matched event profile and is now historical
to the format-5 profile above. Both runs were
diagnostic-only and do not establish production timing. e1000 remained
configured, without receive traffic.

| Format-3 create phase | 1 vCPU | 2 vCPU |
| --- | ---: | ---: |
| Successful allocations / failures | 18,264 / 0 | 18,375 / 0 |
| Frees / live objects at phase end | 14,146 / 4,118 | 14,200 / 4,175 |
| Peak live objects | 4,295 | 4,352 |
| Cross-CPU frees | 0 | 4,243 (29.9% of frees) |
| Requests <=384 bytes | 8,399 (45.99%) | 8,507 (46.30%) |
| Requested bytes <=384 | 653,783 (0.285%) | 666,119 (0.290%) |
| 384-byte cutoff ideal-packed page peak | 52 pages | 53 pages |
| Sum of independent per-class peak pages | 65 pages | 65 pages |
| Modeled class-rounding waste | 32,617 bytes | 33,337 bytes |

These counts apply to requests that enter the indexed heap in the production
configuration; the profile does not observe successful slab hits, identify
call sites, or measure elapsed lifetimes. Of create-phase allocator entries,
83 allocations occurred with interrupts disabled on each run, none occurred
while a tracked spinlock was held, and no allocation failed. Frees included
58 IF-disabled operations, 1,319 spinlock-held operations, and 12,769 / 12,823
normal-context operations. These are mutually exclusive entry categories,
not a claim that interrupt handlers account for all IF-disabled calls. The
size profile supports the tested small-object domain by count but shows that
those requests account for less than 0.3% of requested bytes. Ideal page
counts are model estimates, not observed hybrid heap consumption.

### Historical format-4 event profile and routing comparison

This subsection preserves the prior callerless trace and its measurements for
comparison. It is superseded by the format-5 callsite evidence above.

The retained format-4 source-matched profile artifacts are under
`build/stage46-profile-v4c-confirm-1vcpu/` and
`build/stage46-profile-v4c-confirm-2vcpu/`. The two-vCPU create workload
completed with zero allocation failures and zero tracking misses. Its frozen
trace contains 18,435 successful allocations, 14,230 frees, 32,665 total
events, a peak of 4,382 tracked live objects, and 4,205 live objects at the
snapshot. The 2-CPU profile observed 5,198 cross-CPU frees (36.5% of frees).
There were 83 allocations with interrupts disabled, no allocation while a
tracked spinlock was held, and 1,319 frees while a tracked spinlock was held.
The largest exact request sizes were 16 bytes (4,251 requests), 72 bytes
(1,490), 80 bytes (522), and 272 bytes (946). Normalizing requests to slab
classes maps 4,295 requests to the 16-byte class. Requests at or below 384 bytes
were 46.47% of count but only 0.293% of requested bytes (672,919 of
229,695,087 bytes). The profile covers boot/service startup, Shoot, Reed
editing/saving, and MGFS operations. FAT32/exFAT and actual network receive
traffic were not exercised.

The trace records allocator commit order under the indexed heap lock, not
call-entry or wall-clock order. CPU identity is captured for each operation,
but replaying that sequence on the host is serial and does not reproduce Pith
overlap, scheduling, IRQ state, or migration. Lifetime identity and ordering
are exact for this indexed-only diagnostic run; they are not elapsed-time
lifetime measurements. The trace's 1.5 MiB event buffer is raw-heap allocated
before recording and contributes diagnostic heap pressure. The profile is not
a production memory or latency measurement.

The event trace evaluated compile-time public routing cutoffs of 256, 288,
and 384 bytes. Allocation-count/requested-byte coverage was 39.15%/0.125%,
44.29%/0.237%, and 46.47%/0.293%, respectively. The ideal-packed lower-bound
model estimated 51, 51, and 53 simultaneously needed slab pages. Modeled
class-rounding waste was 18,598 bytes at 256 and 33,737 bytes at 288/384; the
additional waste at 288 is mostly the 272-byte request rounded to 288. These
models exclude actual slab placement history, per-CPU cache ownership, and
alignment gaps. A cutoff does not alter free dispatch: `kfree()` continues to
use the page ownership classifier, so pointers allocated before activation or
through the heap remain correctly routed.

The 1,024-live-object host public-API fixture used indexed-only, cutoff 256,
cutoff 288, and cutoff 384. Peak heap committed extent was 245,760 bytes for
indexed-only, 262,144 bytes for either selective cutoff, and 344,064 bytes for
384. The full cutoff accumulated 53 slab backing allocations and 46 releases
before drain, leaving 7 partial slabs and 14 cached objects; the selective
cutoffs accumulated 40 allocations and 34 releases, leaving 6 partial slabs
and 12 cached objects. A complete drain released the remaining slab pages to
the indexed heap in all cases. The high-water mapped extent remained at its
peak after that drain. Classifier payload (8,224 bytes) and four-CPU cache
records (10,240 bytes) remained allocated; together with heap metadata the
post-drain allocated heap payload was 18,528 bytes. Indexed-only used 60
committed heap pages, selective routing 64, and full routing 84 in this
fixture. This isolates the fixture's extent result; it is not total system
physical memory or proof that all extent bytes remain unavailable to the
heap. Slab backing is reusable heap capacity after drain, while heap mappings
and PMM frames remain retained.

The fixture holds 128 objects at each of 16, 32, 72, 128, 192, 256, 384, and
385 bytes (187,520 requested bytes total). At cutoff 384, the seven slab
classes consume 40 pages at the live-set fill peak: 163,840 bytes. The 896
slab-routed requests total 138,240 bytes; rounding the 72-byte requests to the
80-byte class adds 1,024 bytes. The 40 pages provide 153,104 bytes of slot
capacity, of which 13,840 bytes exceed the class-rounded capacity of the 128
caller-owned objects per slab class (some capacity is held in local caches).
Descriptor,
bitmap/object-area offset, per-slot ownership bytes, and page tails account for
the remaining 10,736 bytes. Thus this fixture's modeled slab backing is
25,600 bytes above its requested slab payload, before heap block headers and
backing alignment effects. Cutoffs 256/288 use 27 pages (110,592 bytes) for
the six slab classes; the 128 384-byte objects instead use ordinary heap
blocks. They avoid 13 slab pages and reduce the total heap's high-water extent
by 20 pages compared with cutoff 384. The benchmark records the total extent,
but does not attribute every extra mapped page between configurations to a
single split, alignment, or growth event. This result is a workload-specific
cost of routing the 384-byte class through page-aligned backing, not a general
physical-memory multiplier.

Five GCC 14.2 `-O2` host trials replayed the complete 32,665-event trace once
untimed and once timed in fresh processes. Median cost per alloc/free event was
105.4 ns indexed-only, 106.4 ns shared slab, 132.5 ns direct per-CPU slab,
144.8 ns public hybrid at cutoff 384, 141.7 ns at cutoff 256, and 151.3 ns at
cutoff 288. The trial-median ranges were 98.9–190.8, 100.3–123.8,
119.1–173.4, 131.3–176.1, 118.9–189.9, and 117.4–180.8 ns respectively.
Variation is large; this serial host replay is useful lifetime/order evidence,
not a Pith or SMP throughput ranking. Separate synthetic immediate-pair host
trials favor the hybrid for tested 16- and 72-byte reuse, while the full trace
replay and live-population footprint do not establish a general production
benefit. The 256/288 cutoffs reduce this fixture's committed extent relative
to full routing, but still exceed indexed-only and the event replay is slower
at their public paths. At the time, the indexed-only production default
remained unchanged; cutoff values were controlled build-time experiments.
The later Slice 2 integration selected hybrid-256 as the ordinary build
default; these historical results do not establish its performance.

Reproduce the public host comparison with the summary and matching event CSV
from one profile run:

```sh
python3 tests/hybrid_public_benchmark.py gcc \
  --profile-summary build/stage46-profile-v4c-confirm-2vcpu/mgfs-0/create/heap-profile/summary-event-ready.json \
  --profile-events build/stage46-profile-v4c-confirm-2vcpu/mgfs-0/create/heap-profile/allocation-events.csv \
  --trials 5 --output build/<new-output>.json
```

### Three-way controlled benchmark

Use the current indexed heap as the primary baseline and compare it with shared-slab and per-CPU-slab paths using the retained production-source benchmark harness. Historical first-fit is a separate reference only. Pin exact source hashes, compiler, flags, host, CPU affinity, warm-up, timer source, seed, sample count, and initial heap state. Use the same production allocator paths and the same PMM/VMM/lock adapter for versions that have equivalent behavior. Do not compare a mock slab to production heap code.

Retain the current unoptimized GCC -O0 host comparison for continuity. Add a separate optimized host configuration if it is representative of the actual target flags. The current kernel C flags do not set a global optimization level; record actual per-object flags rather than assuming a release configuration. Keep sanitizers and full verifiers out of latency runs and run correctness separately.

Workloads must include:

- Single-thread small allocations by every candidate class, cache hit and refill separately.
- Small object churn with realistic source-derived class mixes and multiple live-lifetime distributions.
- Empty, partly occupied, and many-partial-slab states.
- Large 4 KiB, 64 KiB, and multi-page requests.
- The historical fragmented and adversarial same-bin scenarios.
- Failure and recovery with finite heap capacity.
- One and multiple CPUs, comparing indexed, shared-slab, and diagnostic per-CPU paths with cross-CPU frees.
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
2. No live objects overlap; every allocated slot belongs to one slab and one class; page tokens, slab descriptors, class lists, bitmaps, and counts agree.
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
- Report small-object committed memory and peak partial-slab retention; do not accept a latency win that causes unexplained or unbounded heap growth. The default has no empty-slab cache, but partial-slab retention remains workload-dependent. The optional diagnostic reserve is bounded to one empty slab per class and is not enabled by default.
- Do not claim host goals imply target hardware performance. On guest, require no correctness regression and compare only within the same QEMU configuration.

The percentages are provisional engineering gates, not results supported by current evidence. If a gate is missed, inspect class choice, lock contention, and backing policy before changing the gate.

## Current integration boundary

The ordinary hybrid build routes public allocations from 1 byte through the
configured `KERNEL_HYBRID_ALLOCATOR_CUTOFF` (256, 288, 320, 352, or 384 bytes)
through the per-CPU slab path and sends larger requests and raw allocator
metadata through the indexed heap. Public frees use generation-aware
classifier dispatch. Clean slab unavailability can fall back to the heap;
corruption does not. The indexed-only build remains available as a build-time
comparison and recovery configuration.
The bounded failure-triggered reclaim and retry policy is described in the
[memory contract](memory.md#public-allocation-routing-and-initialization).

The captured profile contains boot, Shoot, Reed, and MGFS activity. FAT32,
exFAT, broader service workloads, physical-hardware performance, and e1000 RX
under SMP remain unqualified. The heap retains mapped pages and PMM frames
after slab backing is returned to heap capacity. These limits mean the new
default still requires broader system qualification; they do not imply a new
heap or VMM ownership mechanism.

### Rollback boundary

The indexed-only `KERNEL_HYBRID_ALLOCATOR=0` build remains independently
buildable for comparison and recovery. The ordinary hybrid configuration is
selected at build time and cannot be disabled at runtime while slab objects are live. Do not add
an automatic fallback from corrupt slab metadata to the heap: clean slab
unavailability may use the indexed path, but an ownership or integrity
violation remains fatal.

## Risks, unknowns, and non-goals

### Implementation invariants and remaining qualification

- The selected classes were profiled under boot, Shoot, Reed, and MGFS activity on one and two vCPUs, not FAT32/exFAT, meaningful NIC traffic, or every native service. Broader workloads may justify revisiting the cutoff.
- Published classifier tables are retained for the slab module's lifetime. The current two-vCPU probe measures the workload's retained table payload; theoretical full-reservation growth is distinct from committed physical memory. The indexed-only recovery build omits classifier storage, while the ordinary hybrid build includes it.
- Page-aligned slab backing uses the existing physical-neighbor chain and 48-byte block header; ordinary public allocation retains its 16-byte alignment contract.
- The current kernel heap and PMM growth path have bounded non-sleepable semantics. A slab miss is not guaranteed to succeed in an interrupt handler or while a spinlock is held; available cache or heap capacity may still satisfy such an allocation.
- DMA call sites must retain explicit device contracts. In particular, investigate AHCI’s dma_alloc usage separately; its current implementation reports only the physical translation of the aligned virtual start address, which by itself does not establish multi-page physical contiguity.
- A class lock does not make descriptor lifetime safe if a classifier token is cleared or published outside the specified ordering.
- Host pthread timings are not Pith timing. The corrected pinned fixture shows gains for fixed-size immediate reuse, while the profile-size replay is near parity and the live-population workload uses more committed heap extent. Native QEMU TCG confirms functional routing and concurrency, not stable allocator throughput. No physical-hardware performance comparison is available.
- CPU-local records cover all discovered processors; runtime CPU hotplug and cache-record reclamation are unsupported. Do not free the metadata or shrink cache bounds while an AP may still reference a record.

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
