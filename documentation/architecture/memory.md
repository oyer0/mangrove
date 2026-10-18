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
block metadata, accounting, and 2 GiB virtual heap limit
as later allocations; it is not a separate pool or a fixed pre-reservation.
Exhaustion or failure to obtain/map backing returns `NULL` without exposing an
unmapped page. The window is closed immediately before the init graph enables
interrupts.

After bootstrap, growth is serialized by a mutex and requires a schedulable
thread with interrupts enabled. PMM frame acquisition and VMM mapping occur
without the heap spinlock; that lock protects block metadata and free bins
and is held only for nonblocking allocator work. Callers must not rely on heap
growth while holding a spinlock or from an interrupt handler. Existing free
blocks remain usable in interrupt-disabled contexts. Heap pages are not
reclaimed.

Each mapped page is owned by the heap only after its supervisor writable,
non-executable leaf mapping succeeds. Page-table levels missing from a kernel
mapping are built detached and published only after the subtree is initialized.
If a page-table allocation fails, those detached table frames are returned to
PMM and the failed page mapping leaves no new intermediate tables behind.
During a four-page heap growth, already mapped pages are retained and
incorporated into the heap if a later leaf mapping fails; frames never mapped
are returned to PMM. `kmalloc` retries once against that published prefix: it
returns the allocation if the prefix is sufficient, otherwise returns `NULL` without an
unbounded retry. If no page was mapped, it returns `NULL` and all reserved data
frames are returned. Normal mapping publication performs the existing remote
TLB synchronization; bootstrap mapping is allowed only while the BSP is alone,
so no remote shootdown is needed. The kernel virtual heap range is 2 GiB, but
actual growth is also bounded by available PMM frames and page-table memory.

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
partial PMM batches, and zero, insufficient, and allocation-sufficient
partial VMM growth. `test-vmm` also
executes the production kernel mapping helper with independently tracked table
frame ownership and injects failures at each missing paging level. These
fixtures do not execute hardware page tables, scheduler context, or actual SMP
shootdowns; only a guest boot exercises those integrated paths. The optional
timing probe uses a host workload and does not predict physical-hardware
latency.

## Authoritative code

- `include/address_layout.h`
- `kernel/include/address_space.h` and `kernel/include/vmm.h`
- `kernel/include/heap.h`
- `boot/src/bootstrap.c`
- `kernel/src/address_space.c`, `kernel/src/vmm.c`,
  `kernel/src/elf_loader.c`, and `kernel/src/heap.c`
