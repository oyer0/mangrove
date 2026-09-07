# Mangrove documentation

This tree documents current Mangrove contracts and subsystem behavior. Source
code remains authoritative where a document explicitly names an implementation
detail.

## Filesystems

- [MGFS](filesystem/mgfs/README.md): native filesystem behavior, binary format,
  and labels.
- [exFAT](filesystem/exfat/README.md): supported filesystem behavior and
  formatter integration.
- [exFAT up-case provenance](filesystem/exfat/upcase-provenance.md): reproducible
  origin of the embedded recommended up-case table.

## Kernel

- [Boot flow and handoff](architecture/boot.md): UEFI root selection, kernel
  loading, and the `BOOT_INFO` boundary.
- [Memory layout](architecture/memory.md): fixed kernel regions, bootstrap
  mappings, and process address-space isolation.
- [Processes and syscalls](architecture/processes-and-syscalls.md): process,
  thread, handle, executable, and native syscall contracts.
- [Timekeeping](kernel/timekeeping.md): monotonic and realtime clock semantics.

## Network

- [DHCP lease lifecycle](network/dhcp-lease.md): lease ownership, deadlines,
  renewal, rebinding, and expiry behavior.
