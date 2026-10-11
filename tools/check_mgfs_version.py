#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only MGFS 1.2 admission for retained development images."""

import argparse
import os
import struct
import sys

from copy_partition import load_root_partition, SECTOR_SIZE


def check(path, gpt_root=False):
    fd = os.open(path, os.O_RDONLY)
    try:
        offset = 0
        if gpt_root:
            start, _ = load_root_partition(fd, os.fstat(fd).st_size)
            offset = start * SECTOR_SIZE
        header = os.pread(fd, 24, offset)
        if (len(header) != 24 or header[:8] != b"MGFSv1\0\0" or
                struct.unpack_from("<QQ", header, 8) != (1, 2)):
            raise RuntimeError("unsupported MGFS format (requires 1.2)")
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--gpt-root", action="store_true")
    args = parser.parse_args()
    try:
        check(args.image, args.gpt_root)
    except (OSError, RuntimeError, ValueError) as error:
        print("%s: %s; image preserved; back up and explicitly recreate it with --fresh" %
              (args.image, error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
