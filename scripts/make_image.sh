#!/bin/sh
set -e

IMAGE=build/Mangrove/Mangrove.img
TEST_IMAGE=build/Mangrove/TestDisk.img
MKMGFS=build/mkmgfs
FRESH=0

if [ "${1:-}" = "--fresh" ]; then
    FRESH=1
elif [ "${1:-}" != "" ]; then
    echo "Usage: $0 [--fresh]" >&2
    exit 2
fi

mkdir -p build/Mangrove

rm -f "$IMAGE"

truncate -s 64M "$IMAGE"
mkfs.fat -F32 "$IMAGE"

mmd -i "$IMAGE" ::/EFI
mmd -i "$IMAGE" ::/EFI/BOOT
mmd -i "$IMAGE" ::/Mangrove

mcopy -i "$IMAGE" build/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i "$IMAGE" build/Mangrove/kernel.elf ::/Mangrove/

if [ "$FRESH" -eq 1 ] || [ ! -f "$TEST_IMAGE" ]; then
    echo "Creating fresh MGFS root image $TEST_IMAGE..."
    rm -f "$TEST_IMAGE"
    "$MKMGFS" \
        --blocks 16384 \
        --uuid 00000000-0000-0000-0000-000000000001 \
        --format-time-ns 0 \
        "$TEST_IMAGE"
else
    echo "Reusing existing MGFS root image $TEST_IMAGE"
fi
