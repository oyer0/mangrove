# MGFS volume labels

MGFS major version 1, minor version 1 and later store an optional label extension after
the fixed 200-byte superblock header. The extension does not change the
original header checksum coverage.

The extension occupies bytes 200 through 287 of superblock block zero:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 200 | 8 | `MGLABEL1` extension magic |
| 208 | 8 | UTF-8 label length in bytes, at most 63 |
| 216 | 63 | label bytes, without a terminating NUL |
| 279 | 1 | reserved, zero |
| 280 | 8 | CRC-64 of bytes 200 through 287, with this field zero |

An all-zero extension means that the filesystem has no label. The extension
has its own checksum because the original v1 superblock checksum covers only
bytes 0 through 199.  Bytes 288 through 4095 remain reserved and zero.

## Validation

Labels are UTF-8 and at most 63 bytes. An empty label is valid. Non-empty
labels must contain non-whitespace content and cannot begin or end with
Unicode whitespace. Invalid UTF-8, NUL, `/`, controls, bidi or formatting
characters, emoji and pictographic ranges, and decorative symbol ranges are
rejected. ASCII labels are limited to letters, decimal digits, ordinary
space, `_`, `-`, and `.`. The current validator deliberately does not perform
Unicode normalization or use a complete Unicode category database.

The same validation is used by the host formatter and the runtime label path.
`mkmgfs` accepts `--label <label>`. `diskutil` changes labels only on an
unmounted filesystem.

## Runtime mutation and recovery ownership

The management dispatcher validates the selected registration and read-only
policy under the storage-management/I/O guard. Before loading superblock state,
it acquires an exclusive private VFS recovery owner for the selected volume's
physical range. This uses the same registry and post-unlock handoff as formatting
and FAT32/exFAT label operations, without publishing a namespace mount.

The transaction retains original, proposed and observed 4 KiB superblock images,
the selected identity/geometry, and write, flush, readback and handoff results.
Only the label extension changes; the fixed header checksum and unrelated bytes
are preserved. An unchanged extension is a no-op with no write or flush.
Otherwise one identity-bound superblock write attempt is followed by a flush
and exact full-block readback, including explicit verification of clearing.
The write-through block cache invalidates overlapping parent/partition views
before every write attempt; it has no deferred dirty writeback.

Confirmed pre-write failures release the owner without media mutation. Any
possibly partial write, failed flush, failed readback or unsuccessful completion
retains uncertainty and returns `UNCERTAIN`. There is no retry, rollback or repair
based on an apparently valid later read. The owner prevents conflicting mounts,
formatting, labels and GPT management. Removal makes the device offline, while
the registry retains its slot and required backing resources until recovery
ownership resolves. Its saved physical parent is retained as well; changing a
registration ID cannot discard the exclusion.

On x86-64, the label transaction requests 12,472 bytes; its private superblock
and registry pin bring retained memory to 12,704 bytes, excluding allocator and
controller overhead. Thirty-two such owners retain 406,528 bytes. The shared
registry allows 32 pins and 32 block registrations; exhaustion refuses admission
without recycling protected owners or offline slots. A simple successful label
change uses two backend reads, one write and one flush, four heap requests and
50,792 peak requested bytes including the temporary production probe state.
No allocation-ownership scan or whole-volume readback is required.

These are runtime containment and backend completion guarantees. In-memory
evidence does not survive reboot, and a flushed superblock write is not a claim
of sector atomicity, power-loss durability or automatic post-crash recovery.

## Presentation

The kernel exposes a valid stored label through the filesystem metadata
callback. `lsdsk` displays that metadata. `volumed` uses a label as an
automount path component only when it is also safe for the VFS path policy;
otherwise it retains the stored label for inspection and falls back to the
current disk or partition name for the mountpoint.
