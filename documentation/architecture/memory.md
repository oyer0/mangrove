# Kernel and process memory layout

Mangrove uses a fixed x86-64 high-half kernel layout shared by the loader and
kernel through `include/address_layout.h`.

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
descriptor before the allocator publishes its free-frame count. If safe PMM
initialization or the initial heap mappings cannot be established, boot stops
instead of continuing with partially initialized allocators.

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

The heap starts with four 4 KiB pages and grows in four-page groups. Growth is
serialized by a mutex. PMM frame acquisition and VMM mapping occur without the
heap spinlock; only successfully mapped pages become heap-owned. If mapping a
group partially fails, mapped pages remain available to the heap and frames
that were not mapped return to PMM. That allocation attempt still returns
`NULL`, even if the mapped prefix would now satisfy it; a later request can use
the published space. Heap pages are not reclaimed. The heap lock protects block
metadata and free bins with interrupts disabled; its critical sections do not
allocate, block, or call PMM/VMM.

`kmalloc` returns 16-byte-aligned storage or `NULL`. It can use an existing
free block from an interrupt-disabled context, but does not attempt growth
unless interrupts were enabled on entry. Growth also requires a schedulable
thread because its reservation mutex may block. Callers must not rely on heap
growth while holding a spinlock or from an interrupt handler. `kfree(NULL)` is
a no-op; other invalid frees panic. Statistics describe payload bytes only,
not block metadata, unmapped heap range, or PMM accounting.

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
functions with pthread-backed locks and simulated PMM/VMM mapping. Its
ASan/UBSan cases cover split boundaries, coalescing, alignment, independently
reconstructed payload/accounting state, live-range overlap, growth failures,
randomized ownership, concurrent churn, and isolated fatal controls for corrupt
headers, physical links, bins, bitmap, membership, counters, and invalid frees.
These fixtures do not test real page tables, scheduler context, or SMP kernel
execution. The optional timing probe uses a host workload and does not predict
physical-hardware latency.

## Authoritative code

- `include/address_layout.h`
- `kernel/include/address_space.h` and `kernel/include/vmm.h`
- `kernel/include/heap.h`
- `boot/src/bootstrap.c`
- `kernel/src/address_space.c`, `kernel/src/vmm.c`, and
  `kernel/src/elf_loader.c`, and `kernel/src/heap.c`
