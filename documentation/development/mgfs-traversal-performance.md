# MGFS large-directory traversal performance

The baseline exclusive-create name scan called `mgfs_read_directory_entry()`
for every entry. Both the header read and full entry read called
`mgfs_read_directory_bytes()`, which called `mgfs_read_directory_block()`.
That function traversed and CRC-64 validated the **entire** parent extent-list
chain for each requested data block. Sparse lists with one extent per list
block amplified this work even when consecutive entries occupied the same
logical data block. The checks were deliberate integrity validation; repeating
an unchanged chain for every entry was the algorithmic bottleneck.

The optimized scan validates the complete chain once through the same production
validator and stores its extents in the transaction's existing bounded parent
inventory. Its mapped entry reader retains entry checksum, UTF-8, padding,
length, address, and publication-uncertainty checks. Capacity and inline-array
bounds are checked before populating the inventory. No partially validated
mapping is consumed.

The view exists only during the name scan with the superblock operation lock
held, before persistent mutation. It is not retained across lock release,
parent mutation, staging, publication, recovery handoff, or another create.
The parent preparation path rebuilds its inventory afterward. No mount-wide
cache, additional heap allocation, new on-disk field, or new lock is introduced.
The mapped reader uses one 4 KiB stack buffer, as the baseline byte reader did;
it does not nest a list-block traversal buffer beneath that byte buffer.

## Reproduction

Run from the checkout using native GNU GCC (the Makefile's `HOST_CC`):

```sh
python3 tests/mgfs_stage_profile.py /opt/homebrew/opt/gcc/bin/gcc-16 \
  --revision c6d19cd1eef2c767d389743720fa1b131a69cce6
python3 tests/mgfs_stage_profile.py /opt/homebrew/opt/gcc/bin/gcc-16 \
  --revision 16c09e85909b1926128994410dc8aeee399ee66b
```

On Linux substitute the native `gcc` path. The optional revision selects the
production MGFS source and fixture from Git; both use current checkout headers
and the same portable host linker selection. The profiler instruments temporary
copies, leaving repository and target code untouched. Counters measure calls
to production block-read and checksum functions (including failed attempts),
checksum input bytes, complete directory-mapping validator invocations, and
fixture allocator requests. Compilation is outside timed groups. Wall time
uses CLOCK_MONOTONIC; CPU time uses `clock()`.

Measured on Apple Silicon arm64 macOS, Homebrew GCC 16.2.0, `-O2`, with both
AddressSanitizer and UndefinedBehaviorSanitizer enabled. These are the original,
unchanged `large_create_tests()` cases, including sparse-list failure cases:

| Large-create group | Baseline traversal | Validated-mapping reuse |
| --- | ---: | ---: |
| CPU seconds | 50.544747 | 0.228318 |
| Wall seconds | 50.635639 | 0.228533 |
| Production block-read calls | 4,120,822 | 51,451 |
| Checksum calls | 4,097,583 | 28,212 |
| Checksum input bytes | 16,694,424,816 | 26,281,200 |
| Complete directory-chain validations | 46,562 | 11 |
| Fixture heap requests | 50 | 50 |
| Cumulative requested heap bytes | 958,944 | 958,944 |
| Reported create peak requested heap bytes | 126,696 | 126,696 |
| Reported retained create transaction bytes | 106,152 | 106,152 |

CPU time improved approximately 221 times in this group. An earlier paired
run measured 49.712187 versus 0.222989 CPU seconds with identical I/O/checksum
counts. Other timed groups totaled 0.263611 seconds before and 0.247789 after
in the repeat run. Heap values exclude allocator overhead, host sanitizer
bookkeeping and VFS objects. The 32-retained-create fixture still requests
3,396,864 bytes. No additional mapping allocation is needed: the existing
inventory is bounded by the safe-create parent limit of 512 extents/blocks.

## Verification and limitations

The production-function fixture preserves all existing large-directory,
sparse-chain, boundary, allocation/cancellation, torn-write and recovery cases.
Additional cases compare fragmented mapped reads across boundaries, check a
spanning entry's checksum, reject malformed owner/reserved/checksum/address/
count/cycle fields, inject mapping/data read failures, reject undersized
inventories without I/O, guard against overrun, block uncertain-publication
reads, and confirm a subsequent create sees a newly published entry. A
130-block sparse-parent create has an explicit read-count ceiling rather than
a timing assertion. Repeated saves and failure containment still use the
existing recovery pins and quarantine paths.

These measurements use controlled block I/O, heap, authorization, operation
locks and recovery pins. They are not disk latency, guest SMP, QEMU, crash
recovery or power-loss durability measurements. Normal directory lookup and
enumeration retain their existing traversal paths. Data blocks are still read
for each entry's header/full entry; mapping lookup is a bounded linear search.
Those costs, ordinary lookup traversal and bitwise CRC-64 remain possible
remaining bottlenecks. The optimization does not bypass
ownership preflight or cache mappings across operations.
