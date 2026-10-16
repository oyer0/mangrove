# Native filesystem integration qualification

## Qualified integration boundary

This document records current guest filesystem integration coverage. It does
not establish global clean shutdown, arbitrary controller failure behavior,
SMP, physical hardware, or persistent recovery.

| Contract | Production route | Qualification evidence |
| --- | --- | --- |
| Boot and process completion | UEFI loader → kernel → native Ring 3 launch/syscalls → process wait/cleanup | Root-MGFS two-boot runner; explicit native operation rows, waited child status, unchanged-image verify and independent MGFS decoding/checking. |
| Secondary storage | xHCI → USB/BOT → block/GPT → deviced/volumed → authorized VFS | MGFS/FAT32/exFAT create/verify, mount identities, BUSY references, authorized unmount/remount, denied lifecycle requests, real read-only media and wrong-content/service/device/label controls. |
| Removal and directory identity | Controller teardown → logical offline publication → retained registrations → VFS/location/process references | Matched QMP deletion plus native observations, new replacement IDs, stale handles/CWD refusal, inherited CWD, rename ancestry, absolute recovery and continued root progress. |
| Read and lookup failures | Checked driver results → VFS/global or anchored paths → native error translation | Production-function boundary injection on all three filesystems; native data/metadata read faults, genuine EOF, no false absence or mutation, unmount and unchanged-media checking. |
| Uncertain mutation | Pinned operation → post-unlock handoff → authorized owner inspection → offline identity retention | Host write/flush/publication/cleanup faults and guest blkdebug/removal campaigns require the actual unresolved owner before/after process exit, plus management exclusion and root progress. |
| Safe files and applications | Snapshot/exclusive creation/conditional replacement → Reed/Shoot → production drivers | Exact contents at 0, 1, 511, 512, 4096, 8193 and 2097152 bytes; independent stale writer, conflict/cancellation/Save As, shell redirection/history/script failure, cold-boot persistence and independent media checks. |
| Systematic behavior | MFQS complete production host units; native/host fsstress adapters | Independent expected bytes/names/offsets/statuses, enforced observed witnesses, fixed/seeded campaigns, exact replay, imported preallocation, historical corruption and oracle negative controls. |
| Test integrity | MTS invokes existing tools serially | Required command completion and validated runner receipts; missing prerequisites/evidence/timeouts remain INCOMPLETE, failed groups remain FAIL, optional exclusions remain SKIPPED. |

Production entry points include `kernel/src/{init,process,syscall,vfs,device,block}.c`,
`kernel/src/usb/{xhci,xhci_storage}.c`, `kernel/src/storage/{mgfs,fat32,exfat,gpt}.c`,
`userspace/{deviced,volumed,shoot,reed,fstest,fsstress}/` and the public native
headers. Specific routes and fixture boundaries are described in
[storage lifecycle](../storage/lifecycle.md), [syscalls](../architecture/processes-and-syscalls.md)
and [focused tests](tests.md). Host fixtures control hardware, heap or scheduling
dependencies; they do not replace filesystem semantics and are not guest boots.

## Reproduction and evidence use

[MTS](test-suite.md) preserves `make test`, `make test-guest`, `make test-all`
and individual runners. The standard guest profile contains 27 boots. Its
required host/guest evidence is available independently of the comprehensive
profile, which is approximately 140 boots and has not run end to end.

Application, lifetime, fault and stress campaigns have separately completed
with their native transcripts, child statuses, QMP records, binary hashes,
checker output and exact stopped-media oracles. Reuse requires checking the
report/artifact hashes and source/configuration applicability. It does not
turn an older result into newly executed qualification or prove a newer
kernel binary wholesale. A changed common path needs focused regression and
impacted guest coverage; unchanged planner, lower-level lifetime and oracle
evidence can still be reused within that scope.

The checked-lookup regression also runs historical VFS against disposable
media: a metadata-read error previously became NOT_FOUND and could bypass an
active-CWD removal check. Current lookup preserves IO and aborts before writes.
Tests cover global/anchored, ordinary/trusted and identity-checked routes;
mount/account/configuration callers require confirmed absence. No native ABI
or filesystem format change is needed.

## Resource and verification limits

- Normal guest campaigns use one vCPU and 512 MiB RAM, private firmware variables
  and isolated disk-backed artifacts. Media is inspected only after QEMU exits.
  Boot/operation deadlines and artifact locations remain runner-specific.
- Safe contents remain limited to 2 MiB; mapped replacement, proof memory,
  namespace and directory-growth limits are described per filesystem. Larger
  otherwise valid unsupported layouts must fail before unsafe mutation.
- exFAT's conservative ordinary-transaction bound is 5,484,436 requested bytes
  per pin, or 175,501,952 for 32 retained pins, excluding allocator/VFS overhead.
  This is a bound, not normal-operation RSS or a guarantee that all pins fit.
- FAT32's large fragmented retirement fixture still issues approximately
  6.31 million driver reads and 2.10 million writes for 512 MiB. Earlier
  approximately 1.24-million-read create/mkdir fixtures and remaining repeated
  metadata work are scalability evidence, not measured physical I/O. Operation-
  local views improve some paths; see [metadata performance](fat-metadata-performance.md).
- The native stress profile uses eight files and a 64 KiB per-file model, bounded
  directories and explicit capability exclusions. It does not qualify every
  valid volume, Unicode name, workload size or concurrent process schedule.
- ASan/UBSan runs disable leak detection. macOS guest, guest SMP, hardware,
  electrical removal, reboot repair and power-loss durability are unverified.
  ACPI poweroff is not VFS-wide quiescence or a clean unmount. Confirmed
  secondary unmount and exact cold-boot readback establish narrower guarantees.
