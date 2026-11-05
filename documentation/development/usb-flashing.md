# Compact USB flashing on Linux

Build the normal compact image and host MGFS checker without privileges:

```sh
make -j4 usb-image mgfsck
```

An isolated X250 diagnostic build can use
`BUILD_DIR=build/x250-compact X250_BOOT_TRACE=1` on that command. Normal builds
leave the overlay disabled. Omit `USB_IMAGE_SECTORS` for routine flashing.

Choose the whole removable USB device explicitly, preferably its stable
`/dev/disk/by-id/` name without a partition suffix. Unmount its partitions
manually before invoking the utility. For example, replacing the device path
with the selected whole-disk path:

```sh
sudo ./scripts/flash-usb.sh \
  --image build/Mangrove/MangroveUSB.img \
  --device /dev/disk/by-id/usb-YOUR_DEVICE \
  --mgfsck build/mgfsck
```

The utility never invokes sudo, grants privileges, auto-selects a device,
unmounts filesystems, or provides an unattended confirmation bypass.
`--expect-model 'Cruzer Blade'` additionally requires that exact SCSI model.
The displayed path, USB IDs, model, serial and capacity identify the destination;
confirmation requires typing the complete device-specific token at `/dev/tty`.
Confirmation uses separate sequential terminal input/output streams. Redirected
or piped stdin is never used for approval; an accessible controlling terminal
and its exact typed token are still required. Without a controlling terminal,
or on EOF, incorrect input or interruption, confirmation fails before the first
destination write.
Stable names do not bypass the identity or system-disk checks.

## Admission and supported layout

The physical-device CLI supports native Linux with inspectable sysfs/mount/swap
state. It requires a removable whole disk with USB ancestry, no active mounts,
swap or block holders, and no dependency from mounted/system filesystems.
It follows partition parents and stacked-device slaves, protects active Btrfs
members, and refuses environments where the host root disk cannot be resolved.
Containers with an opaque overlay root are unsupported. A partition device,
nonremovable disk, regular-file destination, or read-only device is refused.

The destination is opened exclusively and its descriptor remains pinned through
verification. Identity, capacity and admission are checked again after typed
confirmation and after verification. Logical sectors must be 512 bytes;
physical sectors may be powers of two from 512 through 4096 bytes, with zero
alignment offset and aligned partition starts. Capacity must be at least the
compact image size and a whole number of logical sectors.

Only the current compact [two-partition Mangrove layout](../storage/image-layout.md)
is accepted: 128 entries of 128 bytes, revision-1 headers of 92 bytes, the
documented fixed ESP/root ranges, and valid matching GPT copies and CRCs.
Hybrid MBRs, malformed names, duplicate/zero partition GUIDs, overlapping or
out-of-range partitions, unsupported geometry and capacity-sized input files
are refused before writing. GPT partition entries and disk/partition GUIDs
are preserved byte for byte.

## Write and verification contract

The source is frozen in a private temporary directory and its SHA-256 is
displayed. The MGFS root is extracted from that snapshot and independently
checked with the selected `mgfsck` executable before the target is admitted.
Checker errors, warnings, missing checker or timeout prevent flashing.

After confirmation, the utility writes every byte of the approximately 129 MiB
image prefix. It clears the obsolete backup GPT region and writes the complete
backup array/header at the destination end, then flushes before publishing the
updated protective MBR and primary header. Both usable-space bounds and header
CRCs describe the new capacity; filesystem and partition boundaries are not
expanded. Adjacent or overlapping old/new metadata locations are supported.
Every required region is explicitly written; skipped sparse blocks are never
assumed to contain zeros. The gap between the image and new GPT is untouched.

After flushing and Linux block-cache invalidation, readback compares the entire
compact region against the frozen image plus the explicit metadata changes.
It separately compares the relocated backup and validates both resulting GPT
copies. All partition contents, including the independently checked MGFS root,
must therefore match the source exactly. This requires about 129 MiB of reads
and writes plus tens of KiB of metadata, regardless of drive capacity. The
resulting device prefix intentionally has different GPT bytes from the input;
a whole-device SHA-256 or a plain prefix `cmp` is not the verification method.

Any write, flush, cache-invalidation, readback or identity error exits with
failure. Destructive replacement is not atomic: failure or interruption may
leave unbootable media, and no automatic rollback or successful flash is
claimed. Resolve the error and rerun. Unallocated space is left untouched.
Owned snapshot files are removed on success, ordinary failure and interruption.

`make test-usb-flash` uses regular files and synthetic Linux inventories only.
It tests different capacities, stale contents, overlapping relocation ranges,
GPT CRCs/identities, MGFS validation, rejected devices, confirmation, bounded
I/O, partial/error writes, failed flush/invalidation/readback and scratch cleanup.
The confirmation fixture uses real controlling pseudo-terminals with terminal,
piped and null stdin, redirected output, incorrect tokens, EOF, SIGINT and no
controlling terminal. Pre-write failures require zero target writes and
unchanged file-backed destination fingerprints. Accepted runs exercise the
full CLI transfer and GPT/content readback without mocking confirmation.
