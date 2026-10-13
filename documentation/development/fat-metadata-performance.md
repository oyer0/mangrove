# FAT32 and exFAT metadata-read performance

## Scope and lifetime

FAT32 safe-create queries, ownership proofs, old-chain inventories and each
free-cluster search use operation-local FAT-sector views. Ownership file-chain
traversal keeps two mirrored-sector views to cache alternating fragmented runs.
Its chunked claims, segmented queue and reusable scratch buffer share the
[documented proof memory budget](../storage/lifecycle.md). Each FAT copy is
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
python3 tests/fat_metadata_profile.py /opt/homebrew/opt/gcc/bin/gcc-16 --revision fabc274fd61ea4dd44961214a938131792c613dc
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

| Workload | Baseline reads | Optimized reads | Writes (both) |
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

| Workload | Baseline CPU/wall ms | Optimized CPU/wall ms |
| --- | ---: | ---: |
| FAT32 contiguous 2 MiB replace | 3.481 / 3.481 | 3.677 / 3.677 |
| FAT32 fragmented 2 MiB replace | 3.706 / 3.706 | 3.930 / 3.930 |
| exFAT fragmented 2 MiB create | 5.537 / 5.588 | 4.218 / 4.217 |
| exFAT contiguous large-directory create | 13.180 / 13.254 | 0.418 / 0.413 |
| exFAT fragmented large-directory create | 12.922 / 12.926 | 0.241 / 0.240 |
| exFAT full upcase read | 1.878 / 1.878 | 0.331 / 0.330 |

Reduced requests do not guarantee reduced CPU time on in-memory fixtures:
FAT32 common contiguous creation is unchanged and cache/device checks add
work. Remaining exFAT fragmented creation still performs 16,386 writes.
The measured implementation does not batch metadata writes.

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
and QEMU/KVM guest validation are not established by these measurements. Host fixture results do not establish guest integration, concurrency timing or external
FAT/exFAT interoperability. Directory growth, cross-sector FAT32 LFN
publication and broader metadata-write batching remain unsupported.
The bounded same-sector FAT write optimization is described below.


## Transaction FAT-write coalescing

Safe create and replacement combine allocation of a fresh cluster and linking
its previous fresh cluster only when both FAT entries occupy the same
512-byte FAT sector. This is a two-entry bound, not a deferred write cache.
The previous entry must still be EOC; FAT32 additionally requires the new
entry to be free in every authoritative FAT copy, preserves each entry's high
reserved nibble, and reads/validates all copies before writing any. exFAT
permits stale values in free-cluster FAT entries, checks the previous fresh
EOC exactly, and admits only the existing ordinary one-FAT volume layouts.

Both candidates are already in the fresh allocation ledger before attempting
the paired write. exFAT persists bitmap ownership first, as before. The next
cluster's data is written after its allocation/link update; all contents and
allocation writes must succeed before directory publication. A cross-sector
pair follows the unchanged individual allocation and link sequence. Bitmap
updates, old-chain reclamation, cancellation, FSInfo, volume dirtying,
publication and flush ordering remain unchanged. No raw multi-sector write or
assumption of atomic sector writes is introduced.

The transaction records both affected cluster identities. Before/proposed
images still cover the complete sector, including unrelated bytes. FAT32
retains the current copy and completed-copy count; exFAT retains configured
FAT-copy images and progress. The uncertainty flag is set before each write
callback and cleared only after confirmed success. A torn write can publish
only one of the two changes; the entire sector and both ledger candidates then
remain retained under quarantine and an unresolved recovery pin. A failed
read or expected-value mismatch writes neither entry. Existing admission
routes mismatches to uncertainty and read failures to cancellation; failed
cancellation itself remains uncertain. No failed write is blindly retried.

All work uses the existing mount operation lock and transaction lifetime.
There is no new lock, mount-wide cache, heap allocation, data copy, or disk
format change. The partner field fits existing transaction padding in the
measured 64-bit fixtures, so requested heap and retained transaction sizes
remain unchanged. FAT32 adds four saved scalar entry words to the bounded
helper scratch, not additional sector arrays. Recovery is runtime containment:
a torn FAT sector can still damage unrelated entries or the old chain.
Quarantine retains evidence; it cannot restore readable old data or promise
crash atomicity, persistence ordering across power failure, or durability.

With normal unoptimized target flags, GCC reports a 2,224-byte FAT32 paired
helper frame versus 2,208 bytes for the baseline helper; the exFAT helper
remains 2,160 bytes. Single-entry wrappers add bounded 80-byte (FAT32) and
48-byte (exFAT) frames. These are function-frame measurements, not complete
kernel stack-depth measurements.

### Write benchmark and regression budgets

Use the profiler against the uncoalesced revision and the current checkout:

```sh
python3 tests/fat_metadata_profile.py /opt/homebrew/opt/gcc/bin/gcc-16 --revision fabc274fd61ea4dd44961214a938131792c613dc
python3 tests/fat_metadata_profile.py /opt/homebrew/opt/gcc/bin/gcc-16 --enforce-write-budgets
```

The same pre-existing full-size fixture workloads, O1 sanitizers, driver
request counters and timing boundaries described above apply. Nested ownership
preflight is included; subsequent read-back is excluded. These are driver
requests, not physical I/O. No data-write batching is performed.

| 2 MiB workload | Reads before / after | Writes before / after | Heap requests (both) |
| --- | ---: | ---: | ---: |
| FAT32 contiguous replacement | 12,294 / 8,231 | 12,289 / 8,226 | 9 |
| FAT32 fragmented replacement | 12,294 / 8,263 | 12,289 / 8,258 | 9 |
| FAT32 old/new replacement | 20,582 / 16,519 | 16,384 / 12,321 | 4,104 |
| FAT32 exclusive creation | 12,292 / 8,229 | 12,288 / 8,225 | 8 |
| exFAT contiguous replacement | 4,117 / 4,117 | 8,195 / 8,195 | 4 |
| exFAT old/new replacement | 16,415 / 16,415 | 12,289 / 12,289 | 5 |
| exFAT fragmented replacement | 20,502 / 16,471 | 16,386 / 12,355 | 4 |
| exFAT contiguous creation | 4,117 / 4,117 | 8,195 / 8,195 | 5 |
| exFAT fragmented creation | 20,502 / 16,471 | 16,386 / 12,355 | 5 |

For fragmented exFAT creation, repeated requested-sector loads fall from
20,431 to 16,400; decoded FAT query counts remain 4,116. Contiguous exFAT
allocation uses NoFatChain, so it has no link writes to combine. Cross-sector
transitions and fragmented clusters in different FAT sectors reduce the gain.
Existing requested peak heap and retained-memory figures above are unchanged;
there are no additional inventories or sector buffers retained by transactions.

One paired host run measured fragmented exFAT creation at 4.209 / 4.208 ms
CPU/wall before and 3.660 / 3.661 ms after; FAT32 contiguous creation at
3.486 / 3.486 ms before and 3.294 / 3.293 ms after. FAT32 old/new replacement
was 5.106 / 5.106 ms before and 5.310 / 7.357 ms after, illustrating timing
noise and that fewer requests do not guarantee a CPU speedup. Host mutexes,
physical-device flush costs and guest latency are not measured.

Normal fixtures enforce fewer than 8,500 writes for the contiguous and
fragmented FAT32 full-size replacement cases and fewer than 12,500 for
fragmented exFAT full-size creation. The profiler's optional write budgets
also bound FAT32 full-size creation at 8,500 and all full-size replacements
and exFAT creations at 12,500. Running `--enforce-write-budgets --revision
fabc274fd61ea4dd44961214a938131792c613dc --filesystem fat32` or `--filesystem
exfat` deliberately fails the original production workloads' new ceilings.
Budgets count complete operations, not reduced-validation simulations.

New production-helper cases check both entry values and neighboring bytes,
FAT32 reserved nibbles and mirrored copies, pre-write read/value failures,
first/second-copy torn writes, device retirement between copies and complete
retained evidence. Public transaction cases verify partial-source cancellation
after a paired update and unresolved retention/no retry after a paired write
fails. Existing full-size, cross-sector, publication, flush, allocation,
cleanup, device-removal and recovery-handoff tests remain enabled. All block
callbacks and fault injection are host fixtures; these results do not prove
hardware, guest SMP or power-loss recovery.


## Bounded FAT32 file retirement

The production retirement fixture measures the complete ownership proof,
preparation, namespace retirement, and cleanup, excluding later pin finish and
readback. O1 ASan/UBSan host measurements use 512-byte clusters and count driver
requests rather than physical I/O. Generated FAT media and recovery callbacks
are fixtures; the device backing store is excluded from kernel heap accounting.

| File layout | FAT copies | Peak requested heap | Retained metadata | Sector reads | Sector writes | Flushes | Host CPU ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 MiB contiguous | 1 | 37,449 | 5,368 | 528,392 | 262,145 | 3 | 127 |
| 512 MiB fragmented | 2 | 135,945 | 5,368 | 6,307,852 | 2,097,153 | 3 | 839 |

An independently mkfs.fat-formatted 256 MiB disposable volume populated by
mtools also exercised deletion of a 128 MiB file. The production driver
requested 41,600 peak bytes and issued 1,056,789 reads,
524,291 writes and three flushes, and used about 2,290 ms host CPU with buffered
file-backed I/O. An externally populated 128 MiB file with 38 noncontiguous links measured
69,260 peak bytes, 1,060,389 reads, 524,291 writes, three
flushes and 2,251 ms CPU. Other imported short, same-sector LFN and spanning-LFN
files were deleted, and independent mtools readback preserved an unrelated file.
`fsck.fat -n` found no namespace/allocation errors; the free-count summary was
left unknown deliberately. These measurements do not establish guest, hardware,
power-loss or reboot behavior.

The retirement payload is fixed at 5,368 requested bytes, or 171,776 across 32
pins, excluding registry storage, pinned nodes/superblocks and allocator
overhead. The temporary proof has a separate 8 MiB budget and is freed before
transaction allocation. There is no proportional old-file array or
unlink-specific byte cap. Preparing the chain is linear with mirrored sector
views; cleanup uses fresh checked FAT reads and the existing per-cluster
mirrored update helper. It does not repeatedly walk from the chain head.
Cleanup needs two FAT-sector reads and one write per configured copy per
cluster, plus the complete proof, chain preflight, directory and FSInfo I/O.
The single-view chain preflight can add another read per copy per cluster on
alternating fragmented layouts. This
conservative implementation can hold the mount lock for substantial time on
real media. Batching cleanup requires preserving the successor and complete
uncertain-sector evidence; it remains later engineering work. The shared
transaction also carries 16 bytes of cursor bookkeeping for other FAT32 safe
operations, with no additional heap requests on those paths.


## FAT32 ordinary write and resize resources

Production ordinary operations use constant recovery state: a 2,248-byte common
record plus 1,152 ordinary bytes, or 3,400 retained requested bytes per pin.
Thirty-two unresolved operations retain 108,800 transaction bytes plus 1,792
registry bytes, excluding pinned nodes/superblocks and allocator overhead.
The complete ownership proof has a separate 8 MiB budget and ends before pin
admission. Cached proof/count/tail invariants are described in
[storage lifecycle](../storage/lifecycle.md#fat32-ordinary-writing-and-resizing).
Every mounted write invalidates the certificate; only a completed ordinary
operation restores it. Rebuilding a proof invalidates prior per-node mapping
summaries before confirming new ones. This avoids repeated full namespace and old-chain scans
for small writes and incremental appends without weakening mutation admission.

O1 ASan/UBSan host measurements count driver sector requests; fixture backing
storage and input/output test buffers are excluded from kernel requested heap:

| Operation | Peak requested heap | Sector reads | Sector writes | Flushes | CPU ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty file to 3 MiB + 17, 512-byte clusters, one FAT | 5,641 | 24,584 | 24,580 | 3 | 6.8 |
| 128 MiB fragmented file, two FATs, overwrite last byte and append one | 37,450 | 528,402 | 8 | 4 | 55 |
| Subsequent one-byte append to the same file | 3,400 | 3 | 2 | 2 | 0.02 |
| Truncate that fragmented allocation to zero | 3,400 | 1,048,582 | 524,291 | 2 | 162 |

At 64 KiB clusters, the ordinary lifecycle fixture peaks at 69,648 requested
bytes, including the temporary cluster-sized read buffer. Recovery remains
3,400 bytes; no cluster-sized write buffer is allocated.

The first two rows include initial lookup reads; CPU timing ends after resolved
handoff and before readback. An independently formatted 256 MiB image, starting
with an empty file and an unrelated neighbor, exercised 3 MiB + 17 writing,
append, zero-filled growth, shrink and readback: 61,588 reads, 36,890 writes,
12 flushes and about 172 ms buffered host CPU. Independent mtools readback
matched the final 3 MiB + 21 bytes and preserved the neighbor. `fsck.fat -n`
reported no namespace/allocation errors, only the intentionally unknown
free-count hint. These are not guest, hardware or power-loss measurements.

At the target's normal unoptimized compilation, GCC reports a 688-byte ordinary
runner frame, 2,192-byte mapping/trim frames and a 2,256-byte mirrored FAT-update
frame. No file-sized kernel-stack structure is introduced. Data RMW and fresh
initialization use individual 512-byte sector requests. Cleanup uses linear
per-cluster checked reads and mirrored updates; fragmented mapping and initial
ownership proof can be expensive and hold operation_lock for substantial time.
Batching, broader directory scalability and persistent repair remain later
engineering work. An in-place overwrite with unchanged mapping/size needs one
flush; publication and allocation cleanup add ordering flushes only when needed.


## FAT32 ordinary create and mkdir resources

Ordinary create/mkdir reuse exclusive creation's transaction and namespace
proof. Only the ordinary VFS hook allocates a return node (72 bytes on x86-64).
Mkdir stages one cluster with dot entries; there is no file-sized inventory.
The 64-byte identity and every transaction allocation precede mutation.
The 8 MiB proof budget is independent and released before pin acquisition.

O1 ASan/UBSan fixture measurements include the return node and identity in peak
requested heap. Driver requests include admission and handoff, excluding subsequent
lifecycle operations and readback. Timings are buffered host CPU, not guest latency.
Two mirrored FATs and two FSInfo sectors are configured in the 512-byte cases.

| Operation | Peak heap bytes | Transaction bytes at handoff | Reads | Writes | Flushes | CPU ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Short empty file | 4,696 | 2,248 | 11 | 3 | 1 | 1.1 |
| Short mkdir | 4,696 | 2,252 | 15 | 6 | 2 | 0.03 |
| Unicode file, fragmented full parent and growth | 14,896 | 11,616 | 36 | 8 | 4 | 0.1 |
| Unicode mkdir, fragmented full parent and growth | 15,412 | 11,620 | 40 | 11 | 5 | 0.1 |
| 255-byte file name, two appended parent clusters | 23,664 | 23,016 | 34 | 14 | 7 | 0.3 |
| 255-byte mkdir name, two appended parent clusters | 23,668 | 23,020 | 38 | 17 | 8 | 0.2 |
| 255-byte mkdir name, full 64 KiB-cluster parent | 80,436 | 14,764 | 530 | 10 | 6 | 8 |

Retained transaction figures omit the identity: on uncertainty it remains in the
transaction (64 bytes); after confirmed publication it belongs to the cached node.
Staging buffers are released on either return. Worst retained metadata is therefore
23,084 bytes per unresolved mkdir plus a 56-byte registry entry, or 740,480 bytes
across 32 pins. Allocator overhead and superblocks are excluded. The large-cluster
peak is 14,764 transaction + 65,536 scratch + 72 node + 64 identity bytes.
At 64 KiB clusters, the ten write requests cover 264 sectors (two private
cluster initialization requests each contain 128 sectors); the smaller-cluster
rows use one sector per write request. GCC's normal unoptimized target compilation
reports a 1,552-byte common creation frame, 144 bytes for the ordinary node hook,
80 for the directory source and 3,056 for the existing spanning-name planner.
No cluster-sized or proportional array is added to the kernel stack.
Normal small creations do not allocate maximum-size growth images. Proof memory
can dominate on larger namespaces, and the mount lock spans the proof and I/O.

A disposable, independently formatted 256 MiB FAT32 image exercised maximum-length
Unicode mkdir with cross-sector publication and growth, ordinary nested creation,
3 MiB + 17 incremental writing/readback, empty-directory removal and supported
short-name rename/unlink. The complete workflow used 1,238,410 sector reads,
36,918 writes, 24 flushes, 23,668 peak requested heap bytes and about 1.4 seconds
host CPU. The read count includes readback: repeated offset reads restart FAT-chain
mapping and are expensive. This is a remaining read-performance issue, not extra
transaction inventory. Independent mtools readback matched all content and the
neighbor; `fsck.fat -n` found no namespace/allocation errors, with an unknown
free-count hint. No guest, hardware or power-loss verification is implied.

## FAT32 rename resources

The production rename fixture measures GCC O1 with ASan/UBSan on Linux,
controlled mounted block callbacks, and requested heap sizes. Figures exclude
allocator overhead, existing cached nodes, the pin registry, kernel caller
stacks and hardware/cache latency. The complete ownership proof finishes before
transaction allocation; its independent 8 MiB budget is not added to transaction
peak memory. An unobserved source can additionally create one 72-byte VFS node
and 64-byte identity before mutation.

| Rename workload | Peak extra requested heap | Retained transaction | Sector reads | Write requests | Flushes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Same-directory short name | 11,104 | 11,104 | 16 | 3 | 3 |
| Maximum ASCII name, shared source/visibility sector | 20,984 | 20,472 | 30 | 10 | 8 |
| Maximum ASCII directory move, two appended destination clusters | 29,240 | 28,728 | 48 | 17 | 10 |
| Earlier destination end marker, directory move | 20,472 | 11,104 | 38 | 6 | 7 |
| 255-byte Unicode directory name | 7,960 | 7,960 | 28 | 5 | 5 |
| Full directory, 64 KiB clusters | 86,008 | 20,472 | 658 | 8 | 7 |

The 512-byte-cluster workloads use two FAT copies and two FSInfo sectors; their
write requests are also sector writes. The 64 KiB initialization request covers
128 sectors, so eight write requests represent 135 sectors. Representative host
CPU time is below 0.25 ms for small fixtures and about 9 ms for the 64 KiB
workload; these are observations, not performance targets.

The transaction header is 2,256 bytes. Rename-only state is 2,584 bytes,
retirement images 3,120, creation images 3,144, directory-growth state 9,368,
and optional second-cluster FAT images 8,256. The latter is needed only with
512-byte clusters when a 21-slot name requires two appended clusters. Maximum
retained metadata is therefore 28,728 bytes; the largest live transaction peak
is 86,008 including temporary 64 KiB initialization scratch. Scratch is freed
on resolved and uncertain returns. Up to 32 unresolved transactions on different
mounts retain 919,296 bytes of driver metadata, or 921,088 including 56-byte
registry descriptors, excluding superblocks, cached nodes and allocator overhead.
The shared transaction header's rename pointer adds eight requested bytes to
other FAT32 transaction allocations; no rename auxiliary allocation occurs in
other operations.

A fragmented 3 MiB file move uses the same bounded transaction inventory, without
copying or storing the file chain. The measured rename plus subsequent lookup
and ownership revalidation requests 3,367 sector reads, eight writes and eight
flushes in about 1.1 ms of host CPU. Whole-namespace proof cost, strict directory
walk/growth limits and up to 99 alias scans remain scalability constraints.
No persistent ownership cache or repeated full-namespace proof is introduced
inside rename; its ancestry check is linear in validated directory depth.

A disposable 256 MiB volume independently formatted by `mkfs.fat` exercised
imported 255-byte file movement with two destination growth clusters, nonempty
directory movement and return to root, child creation/movement, source-slot reuse,
and production readback. The lifecycle requested 331 reads, 54 writes and 36
flushes, peaked at 29,240 additional bytes and used about 1.9 ms of host CPU.
Mtools verified the resulting long names, child bytes and an unchanged neighbor;
`fsck.fat -n` exited successfully with only the intentionally unknown free-cluster
summary. This is independent successful-layout interoperability, not guest,
physical-device, crash, reboot or durability validation.

The four-mirror directory-move fixture requests 23 sector writes and ten flushes;
active-FAT mode requests 11 writes and ten flushes, preserving inactive copies.
Fault injection covers every write and flush in both configurations. Compiler
stack-usage measurements are separate from requested heap accounting; the
unoptimized target driver uses a roughly 4.5 KiB rename frame, a 4,384-byte
ownership frame and a 1,616-byte directory-walk frame. These individual frame
sizes do not establish whole-kernel maximum stack usage.
