# Block, filesystem, and volume lifecycle

Mangrove separates storage mechanism, generic device reporting, and removable
volume policy:

```text
kernel block, GPT, filesystem drivers, and VFS -> mechanism and authority
deviced                                      -> generic device snapshots
volumed                                      -> removable-volume policy
mount, unmount, eject, lsdsk, diskutil       -> clients
```

## Device and partition identity

Every registered block object receives a monotonic boot-local instance ID.
Registry slots, display names such as `disk2`, USB ports, and GPT partition
numbers are not lifecycle identity. A partition has its own instance and an
authoritative parent relationship. Removing and recreating `part1`, or
replacing media in the same port, produces fresh block identities.

The registry marks an instance offline before teardown and admits no new I/O.
Block reads, writes, and flushes resolve liveness for that exact ID and are
serialized with removal. Long-lived mounts and destructive-operation requests
retain or revalidate instance and parent identity, preventing stale work from
reaching replacement media.

GPT discovery creates child block instances. Filesystem probes publish
recognized type and native label as copied metadata. Current supported
filesystems are MGFS, FAT32, and exFAT; unknown or corrupt media remains a
visible block object without becoming mountable.

## Mount authority and removal

The VFS mount table is the authoritative source for mounted state and path.
Root and role-associated boot storage are system-managed. Removable mounts are
restricted to policy-selected children of `/vol`; client commands cannot
choose arbitrary mount paths.

A clean unmount preflights open file/directory references and process working
directories, stops new filesystem I/O, flushes the filesystem and block
device, then detaches the namespace. Multi-volume eject preflights every
mounted child before the bounded batch detach, so a busy child prevents
partial teardown.

Physical removal is different from clean unmount. The block instance becomes
offline and VFS immediately detaches its namespace without attempting a flush.
Its superblock remains dead while stale handles unwind; later access returns
the device-gone result. A new device instance cannot revive those handles.

## `volumed` policy

Volumed registers the `volume` endpoint, subscribes to device, block, and USB
events, and rebuilds a bounded authoritative snapshot at startup and after
event overflow. It operates on leaf partitions, or a whole-disk filesystem
when no partition child exists. It never enumerates USB hardware itself.

Automount eligibility requires a current removable, non-system-managed,
recognized, unmounted, non-suppressed instance. Supported filesystems mount at
`/vol/<label>` when the label is safe as a path component, otherwise at the
current block name. Exact duplicate labels receive deterministic `-2`, `-3`,
and later suffixes without renaming existing mounts or changing stored labels.
Read-only media is mounted read-only; an unexpected writable-mount failure is
not retried as read-only.

Manual unmount and eject set suppression in the kernel block instance. That
state survives a volumed restart but disappears with physical removal or
reboot. Explicit mount clears suppression after success. Per-instance mount
failure and mountpoint ownership state cannot transfer to a replacement.

## Destructive administration

`diskutil` is the selected-disk administration context. The kernel accepts
format, label, and GPT mutation only from an authenticated, process-local
storage-management session and resolves the exact current block instance.
Mounted, busy, read-only, or system-managed targets are rejected. Formatter
writes are extent-bounded; GPT and formatter paths flush, re-probe, and verify
before reporting success. Formatting does not unmount or mount a target.

GPT mutation validates both table copies and their CRCs, uses aligned usable
ranges, writes backup structures before primary structures, flushes, refreshes
the partition model, and verifies the resulting topology. It does not repair
inconsistent tables or wipe deleted partition contents.

## Authoritative code

- `kernel/src/block.c`, `kernel/src/storage/gpt.c`, and filesystem drivers
- `kernel/src/vfs.c`, `kernel/src/device.c`, and storage syscall handling
- `libc/include/mg/device_service.h`, `mg/volume_service.h`, and `mg/storage.h`
- `userspace/deviced/main.c`, `userspace/volumed/main.c`, and
  `userspace/diskutil/main.c`
