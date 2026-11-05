#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create the sparse regular-file container used by the USB image target."""
import argparse
from pathlib import Path
import stat

SECTOR_BYTES = 512
# Fixed ESP/root layout ends at LBA 264191, followed by 32 array sectors
# and the final backup header. The GPT tool places both copies afterward.
MIN_SECTORS = 264225
MAX_SECTORS = ((1 << 63) - 1) // SECTOR_BYTES


def sector_count(text):
    if not text.isascii() or not text.isdecimal():
        raise argparse.ArgumentTypeError('sector count must be decimal digits')
    value = int(text)
    if value < MIN_SECTORS or value > MAX_SECTORS:
        raise argparse.ArgumentTypeError(
            f'sector count must be between {MIN_SECTORS} and {MAX_SECTORS}')
    return value


def create_image(path, sectors):
    if not MIN_SECTORS <= sectors <= MAX_SECTORS:
        raise ValueError('USB image sector count is outside the supported range')
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        pass
    else:
        if not stat.S_ISREG(mode):
            raise ValueError('USB image output must be a regular file')
    # Truncate first so replacing a generated image cannot retain old tail
    # metadata. Seeking to capacity creates a hole, not a memory-sized buffer.
    with path.open('wb') as image:
        image.truncate(sectors * SECTOR_BYTES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sectors', required=True, type=sector_count)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        create_image(args.output, args.sectors)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
