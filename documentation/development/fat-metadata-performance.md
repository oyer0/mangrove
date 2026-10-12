# Stage 2J Slice 12: bounded metadata reads

## Scope and lifetime

FAT32 safe-create queries, ownership proofs, old-chain inventories and each
free-cluster search use operation-local FAT-sector views. Each FAT copy is
still decoded and compared for every requested entry; active-FAT selection
and all existing chain, namespace and ownership checks remain intact.

exFAT uses separate one-sector FAT and stream views plus a forward stream
cursor for safe-create directory scans, ownership proofs, bitmap allocation
searches and old-chain inventories. Bitmap and upcase mappings remain
FAT-backed system streams. Exact stream length, allocation, ownership,
entry-set checksum, name hash and compressed-upcase checks are unchanged.
The root-system-entry and upcase readers use these views only while building
an unpublished mount state. Ordinary public read/write paths retain their
uncached wrappers.

Views never belong to a mount, node, transaction or public directory cursor.
Mounted callers hold the existing VFS operation lock. Each view ends before
that phase writes metadata or releases the lock; each allocation search gets
a fresh view before the next allocation write. A backward stream request,
changed first cluster, NoFatChain mode, mount owner or device ID resets its
mapping. Device liveness is checked even on hits. Read failures invalidate
views, and the existing operation return paths retain their error classes,
allocation ledgers, recovery pins and quarantine behavior. No publication
sector is taken from a cached view. No writes or disk formats changed.

This does not make raw concurrent media editing safe, add crash recovery, or
establish power-loss atomicity. Existing mount admission and transaction
containment are still required, including rejection of TexFAT.

## Reproducible measurements

Run from the repository root on macOS (Linux may use gcc):

```sh
python3 tests/fat_metadata_profile.py /opt/homebrew/opt/gcc/bin/gcc-16 --revision 16c09e85909b1926128994410dc8aeee399ee66b
python3 tests/fat_metadata_profile.py /opt/homebrew/opt/gcc/bin/gcc-16
```

The helper instruments disposable copies of production drivers and the
matching production-function fixtures, with native GCC 16.2, O1 and
AddressSanitizer/UndefinedBehaviorSanitizer. It retains all existing failure
assertions. For historical measurement it injects the same new 128-cluster
workload and disables only its new read-count budget. Add `--enforce-budgets`
and `--filesystem fat32` or `--filesystem exfat` to show that the baseline
fails the new budgets (300 and 400 reads respectively).

Counts below cover the locked create/replace hook, including nested ownership
preflight, but exclude subsequent fixture read-back. Every count is a driver
sector request, including failed attempts. These are not physical disk I/O:
the real block cache may absorb requests. FAT_entries counts getter queries,
including allocation searches and chain steps, rather than unique FAT reads.
Repeated_sector_loads counts repeated requested LBAs during the outer hook
(the fixture fits the instrumented 16,384-sector range). Writes are unchanged.

| Workload | Baseline reads | Slice 12 reads | Writes (both) |
| --- | ---: | ---: | ---: |
| FAT32 2 MiB replace, contiguous allocation | 12,296 | 12,294 | 12,289 |
| FAT32 2 MiB replace, fragmented allocation | 16,392 | 12,294 | 12,289 |
| FAT32 2 MiB old/new replacement | 32,771 | 20,582 | 16,384 |
| FAT32 2 MiB exclusive create | 12,292 | 12,292 | 12,288 |
| exFAT 2 MiB replace, contiguous allocation | 8,233 | 4,117 | 8,195 |
| exFAT 2 MiB old/new replacement | 32,827 | 16,415 | 12,289 |
| exFAT 2 MiB replace, fragmented allocation | 44,377 | 20,502 | 16,386 |
| exFAT 2 MiB create, contiguous allocation | 8,246 | 4,117 | 8,195 |
| exFAT 2 MiB create, fragmented allocation | 44,390 | 20,502 | 16,386 |
| FAT32 create, contiguous 128-cluster directory | 513 | 261 | 1 |
| FAT32 create, fragmented 128-cluster directory | 513 | 265 | 1 |
| exFAT create, contiguous 128-cluster directory | 264,599 | 273 | 3 |
| exFAT create, fragmented 128-cluster directory | 264,726 | 282 | 3 |
| exFAT fragmented 128 KiB full upcase read | 32,896 | 261 | 0 |

For the contiguous large-directory exFAT create, FAT queries fall from
260,360 to 518 and repeated loads from 264,466 to 140. For the fragmented
case they fall from 260,487 to 519 and 264,590 to 146. The upcase reader
reduces chain steps from 32,640 to 255. FAT32 large-directory proofs retain
all 256 entry queries across the two scans; repeated loads fall from 383 to
131 (contiguous) and 381 to 133 (fragmented). The traversal improvement
comes from eliminating chain restarts, not avoiding ownership checks.

One paired run measured the following CPU/wall times in milliseconds.
Compilation is excluded; small timings are noisy and are not guest latency
or real mutex wait measurements. The locked hook duration is only a host
execution-cost proxy; lock contention and hardware latency are not measured.

| Workload | Baseline CPU/wall ms | Slice 12 CPU/wall ms |
| --- | ---: | ---: |
| FAT32 contiguous 2 MiB replace | 3.481 / 3.481 | 3.677 / 3.677 |
| FAT32 fragmented 2 MiB replace | 3.706 / 3.706 | 3.930 / 3.930 |
| exFAT fragmented 2 MiB create | 5.537 / 5.588 | 4.218 / 4.217 |
| exFAT contiguous large-directory create | 13.180 / 13.254 | 0.418 / 0.413 |
| exFAT fragmented large-directory create | 12.922 / 12.926 | 0.241 / 0.240 |
| exFAT full upcase read | 1.878 / 1.878 | 0.331 / 0.330 |

Reduced requests do not guarantee reduced CPU time on in-memory fixtures:
FAT32 common contiguous creation is unchanged and cache/device checks add
work. Remaining exFAT fragmented creation still performs 16,386 writes;
write batching is intentionally outside this slice.

A second equivalent run produced identical request, query and allocation
counts. Across these two runs, fragmented exFAT creation took 5.537–5.726 ms
CPU / 5.588–5.769 ms wall before, and 4.218–4.763 ms CPU / 4.217–4.770 ms
wall after. Contiguous large-directory creation took 13.180–13.411 ms CPU
before and 0.391–0.418 ms after. These limited repetitions support the
large traversal reduction, not a general hardware throughput claim.

## Memory and verification boundaries

There are no new heap allocations. Timed allocation counts are unchanged:
FAT32 2 MiB create 8, contiguous/fragmented replace 9; exFAT create 5,
contiguous/fragmented replace 4. Existing old/new fixture allocation counts
also remain unchanged. Requested peak heap remains FAT32 33,805 bytes
(contiguous replacement/create), 34,318 (fragmented replacement), 35,480
(old/new); exFAT 23,320 (replacement), 23,648 (creation), 39,704 (old/new).
These figures are fixture-requested bytes, excluding allocator overhead.

On the 64-bit target the FAT32 view is 2,104 bytes; exFAT FAT and stream
views are 544 and 560 bytes. Views are stack-local; only validity flags are
initialized in per-cluster allocation searches, avoiding repeated buffer
clears. Target GCC stack-usage reports (normal unoptimized target flags)
show FAT32 ownership frame 128 to 2,256 bytes and next-free 64 to 2,176;
exFAT ownership frame 192 to 1,296, directory scan 976 to 1,536, and
next-free 64 to 1,168. The largest driver frames remain the existing
4,256-byte snapshot frames; FAT32's new name-query frame is 3,376 bytes.
These are individual frames, not a whole-kernel stack-depth measurement.

The normal sanitizer fixtures retain all prior corruption, allocation,
publication, recovery and failure tests. New cases exercise forward and
backward fragmented reads, cross-sector reads, cache hits, mirror mismatch,
active FAT selection, changed stream/mount/device identity, removal, read
failures and fresh-operation observation of changed/cyclic chains. Large
contiguous and fragmented directories test successful exclusive creation,
case-insensitive collision rejection and subsequent corruption detection.

Normal macOS `gmake test`, focused sanitizer regressions and forced target
binary build run without temporary source/compiler/linker workarounds.
Profiling instrumentation is separate from the normal suite. Linux execution
and QEMU/KVM guest validation were not performed in this slice; host fixture
results do not establish guest integration, concurrency timing or external
FAT/exFAT interoperability. Directory growth, FAT32 LFN creation,
cross-sector publication and metadata-write batching remain separate work.
