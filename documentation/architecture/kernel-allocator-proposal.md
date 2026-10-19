# Pith kernel allocator architecture proposal

**Status:** the slab core remains diagnostic-only; ordinary allocation still uses the indexed heap. The two-vCPU benchmark demonstrates overlapping allocator calls on both CPUs, but QEMU TCG elapsed-time variance prevents a useful throughput comparison. A bounded bitmap-integrity change reduced instrumented free-path medians, while unprofiled native slab frees remain 1.5–2.2 times slower than indexed-heap frees across tested sizes. The optional one-empty-slab-per-class policy reduced backing operations but did not show a stable latency improvement. Production routing remains unjustified.

The current allocation contract remains in [kernel memory architecture](memory.md). The measured indexed-heap comparison and its qualification limits are also recorded there. This proposal separates current facts, retained measurements, and candidate design decisions.

## Executive decision

The production allocator remains the 32-bin indexed coalescing heap. The dormant hybrid design retains value as a candidate because it reuses the heap's backing, mapping, rollback, and growth contracts, but current measurements do not justify routing ordinary allocations through it.

**Decision: conditionally ready for continued bounded diagnosis, but not production integration.** The current per-word integrity implementation retains pointer, generation, descriptor, bitmap-word, and transition checks while avoiding a scan of unrelated bitmap words on ordinary nonfinal frees. The latest one-vCPU and two-vCPU unprofiled guest samples put slab free medians at about 1.5–2.2 times the indexed-heap free medians for 16-, 72-, 192-, and 384-byte requests. The updated instrumented one-vCPU profile reports 14–24% lower nonfinal end-to-end medians than the retained earlier profile, depending on request size; lifecycle medians also fell, but by smaller and size-dependent amounts. The two-vCPU benchmark proves overlap and cross-CPU frees, but QEMU TCG elapsed-time variance does not establish comparative throughput. One two-vCPU mixed-class slab benchmark failed once with a generic worker failure and zero checksum; three fresh repeats passed, so the failure was not reproduced and its specific operation remains unknown. Validation and metadata mutation remain material costs, while the uncontended class-lock interval is smaller. Do not bring forward per-CPU caches on the assumption that shared-lock contention is the dominant cost.

The available evidence identifies ownership checks, descriptor/bitmap validation, and mutation as material free-path costs. It does not prove that one operation alone dominates because component probes are independently sampled and instrumented. The current implementation stores a 32-bit integrity value per bitmap word, checks the touched word on allocation/free, and scans all words plus reconstructs population before retiring a slab. This moves whole-bitmap validation off ordinary nonfinal frees while retaining a complete check at retirement and in `slab_verify()`. Native benchmark overlap is established, not a reliable SMP throughput advantage. QEMU TCG measurements are diagnostic only; no physical-hardware performance claim follows. The source-grounded workload profiles support retaining the 20 classes through 384 bytes and 4 KiB backing as candidates, not as an activation decision.

The dormant core uses a static root directory sized from the heap reservation, with lazy indexed-heap middle and leaf tables and generation tags for retirement and same-address reuse. The diagnostic module adds a measured 4 KiB root directory, avoiding a reservation-sized classifier map without changing heap capacity or ordinary allocation routing. Workload profiles cover bounded MGFS and service activity, not every kernel workload or future application.

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

The heap reservation in `include/address_layout.h` spans 512 GiB (134,217,728 possible 4 KiB pages); this is virtual capacity, not committed RAM. The former diagnostic classifier stored one byte per possible page, adding exactly 134,217,728 bytes to BSS. Retained target artifacts report 5,240,944 BSS bytes for the ordinary image and 139,461,296 for the dense-classifier diagnostic image. The dense map was never part of ordinary builds, but its static cost was too high for activation.

The classifier uses a static root directory sized from the full fixed heap reservation. For this 512 GiB interval it has 512 64-bit pointers and occupies 4,096 bytes; initialization separately bounds usable root entries by the supplied heap capacity. Each root entry covers 1 GiB; each lazily allocated middle entry covers 2 MiB; each leaf has 512 atomic 64-bit page tokens. Middle and leaf payloads are each 4,112 bytes, allocated through `heap_alloc_raw`, not through slab allocation. The first classified 2 MiB region retains one middle plus one leaf: 8,224 payload bytes and two 48-byte raw-heap block headers, plus the separate static root. Each additional region in the same 1 GiB span retains one leaf plus its block header. Unpublished concurrent candidates return to the heap; published tables remain allocated and stable for the module's lifetime. A failed child-table allocation may leave a valid empty parent for reuse. If every 2 MiB region in the 512 GiB reservation is visited, dynamic table payload is 1,080,041,472 bytes plus 12,607,488 bytes of heap headers, in addition to the 4,096-byte static root. Actual dynamic use follows regions that needed slab classification.

Each page token reserves five low bits for class plus one. Zero class means ordinary or retired storage. The upper 59 bits retain a page generation across retirement. Slab descriptor and bitmap initialization precede release publication of a nonzero class token. `slab_try_free` validates the pointer's heap range and committed-page status, reads the token to choose a class lock, then reloads and compares the complete token after locking before touching the descriptor. Retirement clears the class tag under that lock, preserving the generation, then releases the backing block only after unlocking. Same-address reuse increments the generation before publishing the new class, so a delayed waiter cannot mistake a new slab of the same class for the retired one. The table nodes themselves are atomically published and never relocated or freed. Their mapped-range, alignment, magic, and index checks precede dereference.

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

The isolated guest test initializes the dormant module after the requested CPUs
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
does not qualify production routing or physical hardware.

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
`-O0` kernel flags. Ordinary `kmalloc`/`kfree` routing remains indexed-heap-only.

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

Retain the fixed-depth sparse page classifier for the dormant core. A
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

The latest `size -A` results are: ordinary image `.text` 811,116, `.rodata`
50,972, `.data` 61,030, and `.bss` 5,240,944 bytes; diagnostic slab-only
image 823,850 / 53,404 / 61,030 / 5,248,240 bytes; guest-test image
827,178 / 53,996 / 61,030 / 5,252,592 bytes; no-profile benchmark image
841,775 / 57,908 / 61,030 / 5,392,432 bytes; free-profile benchmark image
848,960 / 59,220 / 61,030 / 5,397,616 bytes. These diagnostic objects are
absent from the ordinary image, which has no slab symbols. The no-profile and
profile benchmark images differ by 5,184 BSS bytes and 7,185 text bytes.
The latest guest workload retained 12,336 bytes of classifier payload (one
middle and two leaf tables) after slab backing retirement; retired pages were
reusable by the ordinary heap. The static classifier root remains 4,096
bytes. Image sections and runtime heap payload are separate costs.

The current evidence is **conditionally ready for further bounded diagnosis,
not production integration**. Per-word integrity checks reduced current
profile medians without removing corruption checks, but unprofiled guest slab
free remains 1.5–2.2 times slower than indexed-heap free in the tested workload.
The one-empty-slab reserve candidate substantially reduced backing operations
for a deliberately churn-heavy host workload, but its timing samples were too
quantized and overlapping to establish a latency benefit; immediate retirement
remains the default. In the three-class churn fixture it held three empty
pages (12,288 bytes) at peak, against the policy cap of 20 pages (81,920
bytes). This candidate has production-source host coverage but has not been
guest-qualified. Do not start per-CPU caches on the assumption that shared
class-lock contention is the dominant cost. Ordinary allocation remains
indexed-only.

Before production routing, preserve pointer-to-class ownership,
cross-CPU-free safety, migration safety, CPU-offline draining, and slab
retirement. Any later local cache must explicitly distinguish cached from
client-owned slots, and no slab may retire until local and remote cached
objects are drained. Ordinary allocation remains indexed-only.

Pith has global scheduler ready queues; threads may migrate while not running,
and the scheduler exposes no affinity controls. A CPU-local cache must select
its CPU afresh for each operation and protect that CPU-local state for the full
operation; it cannot retain a CPU identity across calls. See the current
[scheduler and SMP contract](smp.md).

The diagnostic `slab_verify()` checks each class under its lock and scans only the mapped heap prefix. It does not walk the uncommitted portion of the large virtual reservation.

Each size class has a partial-slab list protected by its own IRQ-safe spinlock. Full slabs need no allocation-list entry but remain classifiable through the page token and descriptor. A slab becomes partial when one slot is freed and full when its last free slot is allocated.

A free validates page ownership, descriptor tag and class, object-area bounds, exact slot alignment, slot index, and the allocated bit. Interior pointers and immediate double frees fail closed. The descriptor and bitmap are in-band and remain vulnerable to memory corruption; validation must panic on impossible state rather than repairing it. As with the current raw-pointer `kfree` contract, no allocator can distinguish a stale pointer from a new live allocation after the same address has been reused.

The 64-byte descriptor uses its former 16 reserved bytes for four 32-bit
bitmap-word integrity values. The descriptor integrity value covers those
checks. Allocation/free validate the bitmap word they use; a final free also
scans all words and reconstructs population before retirement, while
`slab_verify()` remains the full diagnostic scan. Corruption in an unrelated
word can therefore remain latent until that word is accessed, the slab is
retired, or the verifier runs; it cannot authorize a slot from the damaged
word.

When a slab reaches zero allocated objects, remove it from its class list, clear its page token's class tag while retaining its generation, release the class lock, then return the aligned backing block to the indexed heap. This is the default policy and retains no empty slab. A diagnostic-only `SLAB_EMPTY_RESERVE_PER_CLASS=1` build can instead retain one empty page per class; the reserve remains classified and owned by the slab allocator until it is drained. A failed backing request drains reserves outside class locks and retries once. The reserve is bounded at 20 pages (80 KiB of slab backing plus heap metadata), but five-trial one-object host churn showed only fewer backing operations, not a stable timing improvement. Keep immediate retirement as the default; do not use reserve behavior in production routing without new workload evidence and an explicit memory-pressure policy.

### Slab allocation, refill, and lock order

A small-object allocation takes the relevant class lock, checks its partial list, and allocates one free bitmap bit using a bounded scan of that slab’s bitmap. It does not acquire `heap_lock` while holding the class lock. The private `slab_alloc` API currently exercises this path; ordinary `kmalloc` does not route through it.

On a class miss, release the class lock before requesting one page-aligned heap block. The underlying heap allocator retains its current execution-context behavior: it may use already-free capacity in non-sleepable contexts, but any growth still observes the bootstrap window, interrupt state, spinlock-depth check, scheduler, and growth mutex. If backing allocation cannot proceed, `slab_alloc` returns `NULL`; a later `kmalloc` dispatcher must preserve the existing failure contract. It must never sleep while holding the class lock.

Initialize the unpublished descriptor and clear its bitmap while the aligned backing page is exclusively owned by the heap allocation. Then reacquire the class lock and recheck the partial list: a competing allocator may already have created a slot. If so, use that slot and return the extra backing page after dropping the lock. Otherwise mark the new slab's first slot, compute its integrity value, publish its class tag with release ordering, and link it as partial. The descriptor and bitmap are initialized before classification; readers acquire-load the tag and recheck it under the class lock before dereferencing the descriptor. No class lock is held over PMM/VMM mapping, heap growth, scheduler progress, or backing-block free.

A small-object free takes only its identified class lock to validate and update the bitmap/list. If it retires the slab, it releases that lock before calling kfree on the backing page. A large-object free uses only the indexed heap lock. No path nests class locks or holds a class lock while acquiring heap_lock. The one-lock-at-a-time rule prevents a heap-growth/cache-refill lock cycle.

The existing kmalloc semantics remain important in interrupt and spinlock contexts. A hit in an existing slab is bounded and non-sleeping. A cache miss may use a page already available in the heap, but cannot grow the heap from a held-spinlock context. If it cannot obtain backing under the existing heap rules, the request fails. This preserves the present NULL-on-unavailable-growth behavior; it does not reserve emergency objects for interrupt use.

### Production dispatch remains inactive

The normal target does not initialize the slab module or route allocations to
it. A future dispatcher must initialize the heap first, then complete
`slab_init` before enabling slab allocation. If initialization fails before
routing begins, remain indexed-heap-only. Heap allocations made before
activation remain valid: `kfree` must recognize ordinary heap ownership and
continue through the existing free path. A clean `slab_alloc` backing failure
may fall back to ordinary `heap_alloc(size)` to preserve use of smaller free
blocks; corruption must panic and must never enter this fallback. A class hit
does not sleep. Slab refill and any heap fallback must preserve the heap's
existing restrictions on interrupt-disabled and spinlock-held growth.

For `kfree`, the sparse page classifier distinguishes a slab pointer from an
ordinary heap pointer. On a non-slab pointer, dispatch must call the existing
heap free routine, preserving its invalid/interior-pointer diagnostics. This
means every heap free may pay the sparse ownership lookup, including large
allocations and objects allocated before activation. The current guest and
host comparisons call `kfree` and `slab_try_free` directly; neither measures
this combined dispatcher. Production integration is therefore blocked until
the exact dispatch path is benchmarked on both slab-owned and heap-owned
pointers. Once slab objects exist, disabling routing is not a safe rollback:
first quiesce callers, drain live slab objects and class state, then prove all
slab backing has returned to the heap. The simpler activation contract is a
one-way switch after successful initialization for the boot lifetime. Heap
statistics continue to count slab backing capacity, not requested object
bytes; slab class counters remain separate diagnostics.

### Statistics and memory pressure

Keep current heap statistic meanings: total/used/free continue to report heap block payload capacity. A live slab consumes one allocated page-sized heap block regardless of how many client objects are live, so these statistics remain backing-capacity measures, not live requested bytes. The dormant `slab_class_stats_t` currently reports allocation/free/failure counts, bitmap words examined, backing acquisitions/releases, full/partial transitions, retired slabs, live/peak slots, and current partial/full slab counts. It does not retain each object's requested size, so it cannot report exact live requested bytes or accumulated class-rounding waste. The host benchmark computes those costs only for its known request sizes. Do not silently reinterpret the heap statistics.

Per slab page, the object area loses 96 bytes to descriptor and bitmap. The page-aligned heap backing block also has the existing 48-byte heap header before the page. Thus the slab-specific in-page overhead is 2.34% of a 4 KiB page, plus the existing backing-block header and class/tail waste. The core's host tests reconstruct class slot counts and the target build confirms the dormant control-state cost; class stats expose partial/full slab counts for measuring retained backing. The sparse classifier has no fixed reservation-sized BSS. Its table payload is reported through `slab_get_classifier_stats`, excluding raw-heap headers and possible split remainder. The production aligned split may also introduce leading and trailing heap headers and free prefix capacity, which target heap accounting and the host fixture distinguish.

A partially occupied slab remains unavailable to other classes and to large allocations until it becomes empty. Track this separately from class rounding waste. With no empty cache, a completely unused slab is immediately returned to the heap’s coalescing bins. The heap still does not unmap pages or return their frames to PMM. Exhaustion returns NULL according to current heap behavior; there is no silent fallback to an unbounded buffer, new page pool, or repeated growth loop.

### Bootstrap and initialization

Class locks and class statistics are static. The dormant core's one-shot `slab_init(heap_start, heap_capacity)` initializes them and records the supplied heap range; classifier nodes are allocated lazily from the raw indexed heap as needed. Normal heap initialization does not call it. The default target excludes the module, and ordinary `kmalloc`/`kfree` remain indexed-heap only. Keep the current early-boot path available for pre-scheduler objects and stacks.

A future small-object `kmalloc` route may obtain a new slab during bootstrap only if its internal page-aligned heap allocation is safe in the existing BSP-only bootstrap window. Static slab metadata must not rely on a heap allocation or per-CPU record. On each context where backing cannot be obtained without scheduler progress, return `NULL`. Do not enable interrupts early, extend the bootstrap window, or introduce a second pool as part of slab integration.

### SMP evolution

The initial slab version uses a lock per class and supports remote frees through that shared class state. Lock hold time is bounded by a partial-list update, at most four bitmap-word checks, and integrity checks. Slot selection scans at most 16 bits for classes with at most 16 slots and uses one bit-scan instruction for larger classes. It has no per-CPU freelist, owner-CPU rule, migration-sensitive cache, or remote-free queue. The scheduler and spinlock contract already prevents sleeping or migration while a spinlock is held.

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

## Implementation sequence

Keep the kernel bootable and preserve a route back to the current indexed heap throughout implementation. Each change needs its listed evidence before small-object routing proceeds.

| Work area | Scope and dependency | Acceptance and focused evidence | Guest evidence and stop condition |
| --- | --- | --- | --- |
| Aligned heap backing | Add internal page-aligned block allocation/free support to the current heap, without slab routing. | Production-source tests for exact page alignment, prefix/suffix splitting, minimum fragments, end-of-heap boundaries, accounting, overflow, failure, bootstrap, held-lock, and invalid free. Existing heap tests remain passing. | Focused boot smoke because the primitive underlies future slabs. Stop on any change to current public allocation semantics or PMM/VMM ownership ambiguity. |
| Static slab metadata and classifier | Add fixed class descriptors, per-class locks, in-page descriptor/bitmap, sparse dynamic page-classification tree, and verifier only; kmalloc still uses the indexed path. | Host tests independently reconstruct page/class ownership; test bad descriptors, classifier boundaries and concurrent publication, bitmap bounds, reuse generations, corruption, and allocation failure. Target links confirm ordinary BSS is unchanged and the diagnostic core adds no reservation-sized map. | Boot and allocator diagnostics; no ordinary slab routing. Stop if lookup cannot fail closed or retained table growth is not bounded by visited heap regions. |
| Small allocation and free routing | Route selected sizes to slabs, retain indexed heap for larger requests. | Production-source tests for all classes, class rounding, full/partial/empty transitions, concurrency, duplicate refill, remote free, exact pointer checking, failure atomicity, and isolated corruption children. ASan/UBSan and negative controls. | Guest kernel test allocates/frees all classes and verifies heap stats and process survival. Roll back routing if large paths, boot, or existing apps regress. |
| Bootstrap and allocation-context integration | Exercise slab initialization and misses in the BSP bootstrap window and normal IRQ/spinlock contexts. | Tests prove a slab hit works without sleep, miss uses existing backing only in non-sleepable contexts, growth obeys current guard, and initialization does not recurse. | Fresh boot must reach native userspace; early scheduler and AP stacks still use large path. Stop if allocator initialization depends on a scheduler object it allocates. |
| Empty-slab release and pressure recovery | Return fully empty slab backing to indexed heap; no PMM unmapping. Test growth and fragmented pressure. | Deterministic live-anchor workloads prove only truly empty pages are released, the token class is cleared before reuse, heap coalesces the backing block, and large requests can reuse it. Inject free and growth failures. | Guest storage/process churn followed by a large request. Stop if a page can be reused while any object or pending free still references it. |
| Per-CPU cache evaluation | Consider only if measured per-class lock contention justifies it. Any design needs bounded local batches and shared refill/drain, with no scheduler API change unless justified. | Cross-CPU allocation/free barriers, CPU migration, batch duplication, cache accounting, interrupt context, pressure drain, AP startup, and bounded retention tests. Compare against shared-lock version. | Fresh two-vCPU native stress; do not claim broad SMP readiness. Stop if ownership, draining, or allocation-context rules cannot be proven. |
| Page reclamation decision | Assess page-backed spans and PMM return only from measurements; require a separate architecture review before implementation. | Evidence quantifies retained mapped pages, partial slabs, external fragmentation, and large allocation failures. Design must specify VMM shootdown and PMM ownership before code. | No guest behavior change. Do not add reclamation unless memory-pressure evidence demonstrates a material need. |
| Stress and allocator comparison | Long-run fixed-seed mixed-size, adversarial fragmentation, exhaustion/recovery, and repeated trials versus both historical and current code. | Independent ownership model, sanitizer tests, raw reproducible measurements, latency distributions, footprint and failure-recovery results. No tolerance relaxation. | Guest run verifies actual native kernel paths and output. Stop on unexplained corruption, memory leak, unstable measurements, or material large-object regressions. |
| System integration | Qualify processes, filesystem, storage, network, services, and shutdown with the production configuration. | Focused MTS host groups, forced target build, existing allocator/VMM/PMM/sync regression groups, broader host qualification where required, and independent artifact validation. | Two-vCPU networking-enabled guest with process and filesystem work; verify completion and persisted content. This is system qualification, not physical-hardware proof. |

### Rollback boundary

Keep the indexed heap path independently buildable until the slab version has passed host and guest acceptance. A diagnostic configuration may route all sizes back to the indexed heap without changing callers. Do not add an automatic runtime fallback from corrupt slab metadata to the general heap; that would hide ownership errors. A clean allocation failure may return NULL, but an invariant violation must stop in the existing panic path.

## Risks, unknowns, and non-goals

### Implementation invariants and remaining qualification

- The selected classes are supported by one MGFS profile workload on one and two vCPUs, not FAT32/exFAT, meaningful NIC traffic, or every native service. Broader workloads may justify revisiting them.
- Published classifier tables are retained for the diagnostic module's lifetime. The current two-vCPU probe measures the workload's retained table payload; the fully visited reservation has a theoretical 1,080,045,584-byte table-payload bound. Ordinary `kmalloc`/`kfree` remain indexed-heap-only, so classifier storage is absent from normal builds. Production routing still requires the integration and broad guest qualification described in the current memory contract.
- Page-aligned splitting must coexist with the existing physical-neighbor chain and 48-byte block header; ordinary kmalloc must remain unchanged.
- The current kernel heap and PMM growth path have bounded non-sleepable semantics. A slab miss is not guaranteed to succeed in an interrupt handler or while a spinlock is held.
- DMA call sites must retain explicit device contracts. In particular, investigate AHCI’s dma_alloc usage separately; its current implementation reports only the physical translation of the aligned virtual start address, which by itself does not establish multi-page physical contiguity.
- A class lock does not make descriptor lifetime safe if a classifier token is cleared or published outside the specified ordering.
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
