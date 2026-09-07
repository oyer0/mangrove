# MGFS volume labels

Stage 19.14b adds an optional label extension to the existing MGFS v1
superblock.  It does not change the format major/minor values or the original
200-byte superblock checksum, so existing unlabeled v1 images remain
compatible.

The extension occupies bytes 200 through 287 of superblock block zero:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 200 | 8 | `MGLABEL1` extension magic |
| 208 | 8 | UTF-8 label length in bytes, at most 63 |
| 216 | 63 | label bytes, without a terminating NUL |
| 279 | 1 | reserved, zero |
| 280 | 8 | CRC-64 of bytes 200 through 287, with this field zero |

An all-zero extension means that the filesystem has no label.  The extension
has its own checksum because the original v1 superblock checksum covers only
bytes 0 through 199.  Bytes 288 through 4095 remain reserved and zero.

`mkmgfs` accepts an optional `--label <label>` argument and validates bounded
UTF-8 label text before writing it.  The kernel exposes a valid stored label
through the existing bounded filesystem metadata callback; `volumed` applies
the additional mountpoint policy and falls back to the disk/partition name for
invalid or unusable labels.
