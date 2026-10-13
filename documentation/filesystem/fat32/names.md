# FAT32 filename comparison and imported aliases

## Namespace contract

FAT32 directory objects have a short alias and may have a long filename (LFN).
Collision checks consider both names. Mangrove compares strictly decoded
Unicode scalars using the **Unicode 15.1.0 Simple_Uppercase_Mapping** property.
The same comparison is used for imported LFN lookup, portable short-alias
lookup, creation collision checks, short-alias selection, and the complete name
queries used by unlink, rmdir, and rename.

This is a fixed Mangrove policy, not a claim of identical matching on every
Windows release or Linux mount configuration. The Microsoft FAT specification
requires a shared, case-insensitive short/long namespace but does not provide
a versioned Unicode mapping table or identify the OEM encoding of a volume.
Linux VFAT exposes codepage, character-set and case-comparison choices; mtools
also converts aliases through a selected DOS codepage and uses host wide-character
casing. Their behavior is not one universal Unicode full-casefold contract.

Each scalar maps to at most one scalar. There are no multi-character expansions,
locale-dependent mappings, or Unicode normalization. For example:

| Names | Result |
| --- | --- |
| `é.txt`, `É.TXT` | Equal |
| Greek `σ`, `ς`, `Σ` | Equal |
| Dotless `ı`, ASCII `I` | Equal |
| Turkish `İ`, ASCII `i` | Distinct |
| `ß`, `SS` | Distinct; no expansion |
| Kelvin sign `K`, ASCII `K` | Distinct under simple uppercase |
| Precomposed `é`, `e` plus U+0301 | Distinct; no normalization |

Leading ASCII spaces and trailing ASCII spaces/periods are ignored in comparison,
following the FAT specification's boundary-name rules. Embedded spaces and
leading periods remain significant. A name consisting entirely of ignored
boundary characters cannot match. Names without a mapping in the pinned release
retain their scalar values, including private-use, unassigned, and later-version
characters. Thus newer Unicode casing relationships are not inferred.

UTF-8 must be minimally encoded and contain valid scalars: overlong sequences,
truncated sequences, isolated continuation bytes, encoded surrogates and values
above U+10FFFF are rejected. U+FFFF is unavailable because FAT LFN slots reserve
it for padding. Imported UTF-16 must have paired surrogates, valid ordinal and
checksum metadata, at most 255 code units, and correct NUL/U+FFFF padding.
An exact multiple of 13 units needs no terminator. Comparisons operate directly
on the validated UTF-16, without a limited UTF-8 scratch buffer.

The VFS's existing filename and directory-entry ABI remains limited to 255
UTF-8 bytes. A valid imported LFN that cannot fit a directory-entry name can
still be listed through its portable short alias. The driver collision scan
does not discard that LFN merely because its UTF-8 spelling exceeds this ABI.

## Imported short-name policy

Only printable ASCII aliases with valid short-name characters and trailing
space padding are interpreted. NT lowercase bits affect their display, not
their case-insensitive identity. Valid ASCII punctuation remains supported.
Non-ASCII OEM/DBCS bytes, the first-byte `0x05` escape for OEM `0xe5`, and
uninterpretable short-name structures are **unsupported aliases**. They are
never copied into a UTF-8 pathname, guessed as CP437/CP850, or replaced by `_`.
The boot sector OEM-name string is not an encoding declaration.

A valid LFN attached to an unsupported alias remains readable through that LFN.
Entries with neither a representable LFN nor a portable alias are omitted from
directory enumeration; they do not stop enumeration of later readable entries.
Deleted entries and volume labels do not impose an OEM-alias restriction.

Create, mkdir, exclusive create-with-contents, unlink, rmdir, and rename refuse
with `VFS_ERR_UNSUPPORTED` if their containing directory has a live unsupported
object alias: the driver cannot prove the requested name is unambiguous.
This is a read-only, operation-level refusal, with no allocation publication,
recovery pin, quarantine, or loss of mount version confidence. Other directories
remain usable. Ordinary read lookup remains best effort through supported names;
it does not establish uniqueness against aliases of unknown encoding.
Identity-bound facts, reads, writes and conditional replacement retain their
existing semantics and ownership checks. Namespace uncertainty does not by itself
imply uncertain file allocation or storage completion.

Malformed LFN metadata encountered by a strict mutation/ownership scan is
different: it is a format error and revokes version confidence. Malformed
UTF-8 input is rejected before directory I/O. Read or allocation failures in
a read-only namespace scan remain retryable.
An LFN sequence followed by a deleted short entry is also rejected by strict
mutation scans: its live name slots could attach to a later short entry.
Ordinary lookup remains able to read unrelated valid entries. Every file
unlink, including short names and same- or cross-sector imported LFNs, uses a
complete namespace ownership proof and recovery-pinned operation. It deletes
the short entry first, then retires the remaining LFN sectors, and only then
reclaims the old file chain. The operation retains up
to three original/proposed directory-sector pairs and a constant-size cleanup
cursor. It validates the complete allocation, including storage past logical
EOF, without a deletion-specific byte limit. Each successor is retained before
any FAT copy is cleared; the cursor advances only after all configured copies
confirm the update. The ownership proof has an independent 8 MiB requested
memory budget; incomplete validation fails before mutation. Failed or ambiguous
sector writes, flushes, or reclamation retain the images, cleanup progress,
mount quarantine, and a discoverable pin. A crash may leave orphan LFN slots
or unreclaimed clusters; runtime containment is not a persistent journal.
Empty LFN directories, including imported names spanning sectors or cluster
boundaries, use the same pinned short-entry-first retirement. The driver
validates the complete namespace and directory contents, inventories the
directory's entire mapped chain, retires all name sectors, then reclaims its
clusters. The directory inventory is bounded by the 4,096-cluster directory
walk rather than by the 2 MiB file-content limit. An uncertain removal hides
namespace reads on that mount while its pin and recovery images remain.
The current 2 MiB safe-file content limit also requires a separate scalability
solution before the desktop release.

Exclusive create-with-contents accepts validated UTF-8 long names through the
VFS limit of 255 bytes and at most 20 LFN entries. A complete name set can
span up to three 512-byte sectors, including cluster boundaries. Reserved
FAT characters, controls, a leading space, and trailing space or period are
rejected. The driver constructs UTF-16 LFN entries in reverse ordinal order,
with the short-entry checksum and a deterministic portable `~1` through `~99`
alias. Each candidate is checked against both long names and short aliases.
Exhausted aliases fail before mutation. When no existing sector has sufficient
contiguous slots, safe creation can span deleted slots or use zero slots after
the first validated end marker. It can append up to two initialized directory
clusters for a complete set. For a spanning set, LFN-only sectors are written
and flushed before the short-entry sector. When an earlier end marker hides
the set, its sector is written last as the visibility boundary. Every affected
sector has retained original and proposed images, write progress, and a
recovery pin. Namespace reads are held while this transaction remains attached;
failed or ambiguous publication keeps the mount quarantined and the images
discoverable.
Legacy create/mkdir still reject names that their short-entry writer would
truncate or encode as unknown OEM bytes, preserving portable 8.3 names rather
than checking one name and publishing another.
Failed or ambiguous directory-sector or FAT-link writes quarantine the mount.
After a crash, prepared but unpublished LFNs may be orphaned, or a file entry
may remain hidden behind an old end marker. Independent repair may be needed;
ordered writes and flushes do not make creation crash atomic or durable.

## Mapping provenance and resource bounds

`tools/fat32_uppercase_15_1.txt` contains all 1,450 non-identity mappings from
field 12 of Unicode 15.1.0 `UnicodeData.txt`. The source file SHA-256 is
`2fc713e6a31a87c4850a37fe2caffa4218180fadb5de86b43a143ddb4581fb86`.
The generator verifies the checked-in pair-file hash, idempotence, scalar bounds,
ordering and exact expansion of 200 disjoint ranges. The compiled table uses
3,200 read-only bytes; ASCII has a direct fast path and other scalars use binary
search. Comparison allocates no heap memory and retains no mount cache.
The strict directory walker no longer needs its 1,024-byte decoded-name scratch.
LFN creation adds a bounded stack UTF-16 buffer and reuses the existing
transaction's complete original and proposed sector images and recovery pin.
It does not change the directory publication count or lock ordering.

```sh
python3 tools/generate_fat32_uppercase.py --check
# Optionally verify against a separately downloaded, exact upstream file:
python3 tools/generate_fat32_uppercase.py --check --unicode-data /path/to/UnicodeData.txt
```

The derived data is distributed under the [Unicode license](unicode-license.txt).
Normal builds and tests need no network access or host Unicode runtime.

## References and verification boundary

- [Microsoft FAT specification](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf), sections 6.1 and 7.
- [Unicode 15.1 UCD definitions](https://www.unicode.org/reports/tr44/tr44-32.html), simple case mappings.
- [Unicode 15.1 data mirror maintained by Unicode](https://raw.githubusercontent.com/unicode-org/unicodetools/main/unicodetools/data/ucd/15.1.0/UnicodeData.txt).
- [Linux VFAT behavior](https://docs.kernel.org/filesystems/vfat.html) and [comparison implementation](https://github.com/torvalds/linux/blob/master/fs/fat/namei_vfat.c).
- [mtools alias conversion](https://github.com/Distrotech/mtools/blob/master/file_name.c).

The namespace and create fixtures exercise production functions with controlled
block, heap, VFS and recovery-pin dependencies. Optional externally formatted
image checks exercise production mount, accented-name reads, and creation of a
long name. They do not establish Windows compatibility, a Linux guest mount,
hardware concurrency, or crash recovery.
